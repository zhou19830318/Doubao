/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * RGB LED Ring Control Module — 7× WS2812 LEDs with state-based animations
 * Animation modes match the HTML LED Patterns simulator (index.html).
 *
 * Supported modes:
 *   SOLID        — all LEDs uniform color
 *   BREATHE      — cubic sine brightness pulse
 *   BLINK        — on/off toggle at 500ms period
 *   CHASE        — bright head with exponential falloff tail
 *   PULSE_WAVE   — sine wave traversing the ring
 *   RAINBOW_SPIN — HSV rainbow rotation
 *   SPARKLE      — pseudo-random sparkle with deterministic seed
 */

#include "ui.h"
#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <string.h>
#include <math.h>

#if BOARD_HAS_RGB_RING

static const char *TAG = "rgb_led";

/* ── Animation mode enum ───────────────────────────────────────────────── */
typedef enum {
    LED_ANIM_SOLID = 0,
    LED_ANIM_BREATHE,
    LED_ANIM_BLINK,
    LED_ANIM_CHASE,
    LED_ANIM_PULSE_WAVE,
    LED_ANIM_RAINBOW_SPIN,
    LED_ANIM_SPARKLE,
} led_anim_mode_t;

/* ── Per-state animation configuration ─────────────────────────────────── */
/* Colors are matched to the HTML LED_STATES table in index.html.
 * The HTML applies a ×6 brightness scale internally for the glow effect;
 * on hardware these base values are used directly with per-frame modulation. */
typedef struct {
    led_anim_mode_t mode;
    uint8_t r, g, b;   /* Base color (0-255) — used by SOLID/BREATHE/BLINK/CHASE/PULSE/SPARKLE */
} led_state_config_t;

/* ── Gamma correction table (same as board.c for consistency) ──────────── */
static const uint8_t s_gamma8[] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  2,  2,  2,
    2,  3,  3,  3,  3,  3,  3,  3,  4,  4,  4,  4,  4,  5,  5,  5,
    5,  6,  6,  6,  6,  7,  7,  7,  7,  8,  8,  8,  9,  9,  9, 10,
   10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 14, 14, 15, 15, 16, 16,
   17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 24, 24, 25,
   25, 26, 27, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 35, 35, 36,
   37, 38, 39, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 50,
   51, 52, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 66, 67, 68,
   69, 70, 72, 73, 74, 75, 77, 78, 79, 81, 82, 83, 85, 86, 87, 89,
   90, 92, 93, 95, 96, 98, 99,101,102,104,105,107,109,110,112,114,
  115,117,119,120,122,124,126,127,129,131,133,135,137,138,140,142,
  144,146,148,150,152,154,156,158,160,162,164,167,169,171,173,175,
  177,180,182,184,186,189,191,193,196,198,200,203,205,208,210,213,
  215,218,220,223,225,228,231,233,236,239,241,244,247,249,252,255,
};

static inline void gamma_correct(uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = s_gamma8[*r];
    *g = s_gamma8[*g];
    *b = s_gamma8[*b];
}

/* ── HSV → RGB conversion ──────────────────────────────────────────────── */
static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v,
                       uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (s == 0) { *r = *g = *b = v; return; }
    uint8_t region = h / 60;
    uint16_t remainder = (h - (region * 60)) * 6;
    uint8_t p = (v * (255 - s)) >> 8;
    uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
    switch (region) {
    case 0:  *r = v; *g = t; *b = p; break;
    case 1:  *r = q; *g = v; *b = p; break;
    case 2:  *r = p; *g = v; *b = t; break;
    case 3:  *r = p; *g = q; *b = v; break;
    case 4:  *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
    }
}

/* ── Global animation state (read by task, written by API) ─────────────── */
static volatile bool      s_anim_running = false;
static volatile ui_state_t s_current_state = UI_STATE_BOOT;
static volatile led_anim_mode_t s_override_mode = LED_ANIM_SOLID;
static volatile bool      s_override_active = false;
static volatile uint8_t   s_override_r = 0, s_override_g = 0, s_override_b = 0;

static TaskHandle_t s_anim_task_handle = NULL;

/* ── Per-state config table ────────────────────────────────────────────── */
/* Colors directly from HTML LED_STATES definitions (index.html L964-977). */
static const led_state_config_t STATE_CONFIGS[] = {
    [UI_STATE_SLEEP]       = { LED_ANIM_SOLID,       0x48, 0x48, 0x4A },  /* dark gray #48484A */
    [UI_STATE_ARMED]       = { LED_ANIM_SOLID,       0x5E, 0x5C, 0xE0 },  /* indigo #5E5CE0 */
    [UI_STATE_BOOT]        = { LED_ANIM_RAINBOW_SPIN, 0x00, 0x00, 0x00 },  /* rainbow */
    [UI_STATE_CONNECTING]  = { LED_ANIM_RAINBOW_SPIN, 0x00, 0x00, 0x00 },  /* rainbow */
    [UI_STATE_IDLE]        = { LED_ANIM_SOLID,       0x00, 0x32, 0x00 },  /* green solid — ready and waiting */
    [UI_STATE_LISTENING]   = { LED_ANIM_PULSE_WAVE,   0xF0, 0x00, 0x00 },  /* red pulse wave — HTML: [40,0,0] red */
    [UI_STATE_SENDING]     = { LED_ANIM_CHASE,        0x00, 0x60, 0xF0 },  /* blue chase — HTML: [0,16,40] */
    [UI_STATE_THINKING]    = { LED_ANIM_CHASE,        0xF0, 0x90, 0x00 },  /* amber chase — HTML: [40,24,0] */
    [UI_STATE_STREAMING]   = { LED_ANIM_SPARKLE,      0xC0, 0x30, 0xF0 },  /* purple sparkle — HTML: [32,8,48] ×6 */
    [UI_STATE_RESPONSE]    = { LED_ANIM_SOLID,        0x00, 0x80, 0x00 },  /* green solid */
    [UI_STATE_TTS_LOADING] = { LED_ANIM_CHASE,        0x00, 0xC0, 0xC0 },  /* cyan chase */
    [UI_STATE_TTS_PLAYING] = { LED_ANIM_BREATHE,      0xF0, 0x00, 0xC0 },  /* magenta breathe — HTML: [40,0,32] ×6 */
    [UI_STATE_ERROR]       = { LED_ANIM_BLINK,        0xF0, 0x00, 0x00 },  /* red blink — HTML: [32,0,0] red */
    [UI_STATE_NOTIFYING]   = { LED_ANIM_BREATHE,      0xF0, 0xC0, 0x00 },  /* amber breathe — HTML: [40,32,0] ×6 */
    [UI_STATE_PLAYING_MP3] = { LED_ANIM_SOLID,        0x60, 0x60, 0x00 },  /* yellow solid — HTML: [16,16,0] ×6 */
};

/* ══════════════════════════════════════════════════════════════════════════
 * Animation Task — renders one frame each 30ms (≈33 fps)
 * ══════════════════════════════════════════════════════════════════════════ */
#define ANIM_PERIOD_MS  30
#define NLEDS           BOARD_RGB_LED_COUNT  /* 7 */

static void led_anim_task(void *arg)
{
    (void)arg;
    uint32_t frame = 0;
    s_anim_running = true;
    ESP_LOGI(TAG, "LED animation task started");

    while (s_anim_running) {
        /* ── Determine active mode and base color ────────────────────── */
        led_anim_mode_t mode;
        uint8_t br, bg, bb;

        if (s_override_active) {
            mode = s_override_mode;
            br = s_override_r;
            bg = s_override_g;
            bb = s_override_b;
        } else {
            ui_state_t st = s_current_state;
            if (st > UI_STATE_ERROR) st = UI_STATE_BOOT;
            mode = STATE_CONFIGS[st].mode;
            br = STATE_CONFIGS[st].r;
            bg = STATE_CONFIGS[st].g;
            bb = STATE_CONFIGS[st].b;
        }

        float t = frame * (ANIM_PERIOD_MS / 1000.0f);  /* seconds elapsed */
        int n = NLEDS;

        /* ── Compute per-LED colors for this frame ───────────────────── */
        for (int i = 0; i < n; i++) {
            float r = 0.0f, g = 0.0f, b = 0.0f;

            switch (mode) {

            case LED_ANIM_SOLID:
                r = br; g = bg; b = bb;
                break;

            case LED_ANIM_BREATHE: {
                /* Cubic sine — matches HTML: (sin(t/0.8 + si*0.5)+1)/2, cubed */
                float phase = t / 0.8f + (s_current_state % 10) * 0.5f;
                float bright = (sinf(phase) + 1.0f) / 2.0f;
                bright = bright * bright * bright;
                r = br * bright;
                g = bg * bright;
                b = bb * bright;
                break;
            }

            case LED_ANIM_BLINK: {
                /* 500ms period — matches HTML: sin(now/500 + si) > 0 */
                float phase = t / 0.5f + (float)(s_current_state % 10);
                bool on = sinf(phase * (float)M_PI) > 0.0f;
                r = on ? br : 0.0f;
                g = on ? bg : 0.0f;
                b = on ? bb : 0.0f;
                break;
            }

            case LED_ANIM_CHASE: {
                /* Head moves around ring, exponential falloff.
                 * Matches HTML: head = (now/140) % n, fade = exp(-dist * 0.7) */
                float head = fmodf(t / 0.140f, (float)n);
                float fwd = fmodf(head - (float)i + (float)n, (float)n);
                float rev = fmodf((float)i - head + (float)n, (float)n);
                float dist = (fwd < rev) ? fwd : rev;
                float fade = expf(-dist * 0.7f);
                r = br * fade;
                g = bg * fade;
                b = bb * fade;
                break;
            }

            case LED_ANIM_PULSE_WAVE: {
                /* Standing wave traversing the ring.
                 * Matches HTML: pos=i/n, wave=(1+sin(pos*2π - now/300))/2, w=wave² */
                float pos = (float)i / (float)n;
                float wave = (1.0f + sinf(pos * 2.0f * (float)M_PI - t / 0.300f)) / 2.0f;
                float w = wave * wave;
                r = br * w;
                g = bg * w;
                b = bb * w;
                break;
            }

            case LED_ANIM_RAINBOW_SPIN: {
                /* Rainbow hues rotating.
                 * Matches HTML: hue = (now/12 + i*360/n) % 360, hsvToRgb(hue, 1, 0.45) */
                uint16_t hue = ((uint16_t)(t * 83.33f + i * 360.0f / n)) % 360;
                uint8_t cr, cg, cb;
                hsv_to_rgb(hue, 255, 115, &cr, &cg, &cb);  /* v=0.45×255≈115 */
                r = cr; g = cg; b = cb;
                break;
            }

            case LED_ANIM_SPARKLE: {
                /* Pseudo-random sparkle with deterministic seed.
                 * Matches HTML: seed=(si*7 + i*13 + floor(now/80)*3) % 100, spark=seed<18 */
                uint32_t seed = ((s_current_state & 0xF) * 7 + i * 13 +
                                 (frame / 3) * 3) % 100;
                bool spark = seed < 18;
                if (spark) {
                    r = br; g = bg; b = bb;
                } else {
                    /* 18% ambient glow */
                    r = br * 0.18f;
                    g = bg * 0.18f;
                    b = bb * 0.18f;
                }
                break;
            }

            } /* switch(mode) */

            /* ── Clamp, gamma correct, write to strip ─────────────────── */
            uint8_t cr = (uint8_t)(r > 255.0f ? 255 : (r < 0.0f ? 0 : r));
            uint8_t cg = (uint8_t)(g > 255.0f ? 255 : (g < 0.0f ? 0 : g));
            uint8_t cb = (uint8_t)(b > 255.0f ? 255 : (b < 0.0f ? 0 : b));
            gamma_correct(&cr, &cg, &cb);
            /* HW: driver configured for GRB but physical LEDs use RGB order.
             * Swap R↔G to compensate for the driver's GRB→RGB mismatch. */
            board_rgb_set_single(i, cg, cr, cb);
        }

        board_rgb_refresh();
        frame++;
        vTaskDelay(pdMS_TO_TICKS(ANIM_PERIOD_MS));
    }

    /* Task exiting — turn off LEDs */
    board_rgb_set(0, 0, 0);
    s_anim_task_handle = NULL;
    ESP_LOGI(TAG, "LED animation task stopped");
    vTaskDelete(NULL);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════ */

static void ensure_anim_task(void)
{
    if (s_anim_task_handle == NULL) {
        s_anim_running = true;
        BaseType_t ret = xTaskCreate(led_anim_task, "led_anim", 4096,
                                     NULL, 5, &s_anim_task_handle);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create LED anim task");
            s_anim_running = false;
        }
    }
}

void ui_update_led_for_state(ui_state_t state)
{
    if (state > UI_STATE_ERROR) {
        ESP_LOGW(TAG, "Invalid state: %d", state);
        return;
    }

    /* Clear any override — resume normal state-based animation */
    s_override_active = false;
    s_current_state = state;

    ESP_LOGD(TAG, "LED state=%d mode=%d", state, STATE_CONFIGS[state].mode);

    ensure_anim_task();
}

void ui_set_led_color(uint8_t r, uint8_t g, uint8_t b)
{
    /* Switch to SOLID override */
    s_override_active = true;
    s_override_mode  = LED_ANIM_SOLID;
    s_override_r = r;
    s_override_g = g;
    s_override_b = b;
    ensure_anim_task();
}

void ui_start_led_breathing(uint8_t r, uint8_t g, uint8_t b)
{
    /* Switch to BREATHE override */
    s_override_active = true;
    s_override_mode  = LED_ANIM_BREATHE;
    s_override_r = r;
    s_override_g = g;
    s_override_b = b;
    ensure_anim_task();
}

void ui_stop_led_breathing(void)
{
    /* Return to state-based animation */
    if (s_override_active && s_override_mode == LED_ANIM_BREATHE) {
        s_override_active = false;
    }
}

/* Optional: stop animation task entirely (call on shutdown) */
void ui_led_anim_stop(void)
{
    s_anim_running = false;
    /* Give the task a moment to exit */
    vTaskDelay(pdMS_TO_TICKS(60));
}

#else /* !BOARD_HAS_RGB_RING */

/* Stub implementations for boards without RGB ring */
void ui_update_led_for_state(ui_state_t state) { (void)state; }
void ui_set_led_color(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
void ui_start_led_breathing(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
void ui_stop_led_breathing(void) {}
void ui_led_anim_stop(void) {}

#endif /* BOARD_HAS_RGB_RING */
