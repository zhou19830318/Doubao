/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * Board initialization — multi-board support.
 * Common code + per-board sections via #ifdef.
 */

#include "board.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_task_wdt.h"
#include "led_strip.h"
#include <math.h>
#include "esp_random.h"

/* Conditional includes: S3 boards with IO expander + codec */
#if BOARD_HAS_IO_EXPANDER
#include "esp_io_expander.h"
#include "esp_io_expander_tca95xx_16bit.h"
#endif

#if !defined(CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2)
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/rtc_io.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#endif

#if BOARD_HAS_DISPLAY && !defined(CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2)
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#if defined(CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER)
#include "esp_lcd_spd2010.h"
#elif defined(CONFIG_HEYCLAWY_BOARD_WAVESHARE_AUDIO)
#include "esp_lcd_panel_jd9853.h"
#include "touch_axs5106l.h"
#endif
#include "esp_lcd_touch.h"
#include "esp_lvgl_port.h"
#include "rom/ets_sys.h"
#endif

#if defined(CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2)
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"  /* ST7789 built-in driver */
#include "esp_lvgl_port.h"
#endif

/* Board-specific codec headers */
#if defined(CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER)
#include "es7243e_adc.h"
#elif defined(CONFIG_HEYCLAWY_BOARD_WAVESHARE_AUDIO)
#include "es7210_adc.h"
#endif

static const char *TAG = "board";

// ============================================================================
// Internal state
// ============================================================================
#if !defined(CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2)
static led_strip_handle_t s_rgb_handle = NULL;
static i2s_chan_handle_t s_i2s_tx = NULL;
static i2s_chan_handle_t s_i2s_rx = NULL;
static i2c_master_bus_handle_t s_i2c0_bus = NULL;
#if BOARD_HAS_DISPLAY && defined(CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER)
static i2c_master_bus_handle_t s_i2c1_bus = NULL;
#endif
#if BOARD_HAS_IO_EXPANDER
static esp_io_expander_handle_t s_io_exp = NULL;
#endif
#if BOARD_HAS_DISPLAY
static esp_lcd_panel_handle_t s_lcd_panel = NULL;
static esp_lcd_panel_io_handle_t s_lcd_io = NULL;
static lv_disp_t *s_lvgl_disp = NULL;
#endif
static esp_codec_dev_handle_t s_play_dev = NULL;
static esp_codec_dev_handle_t s_record_dev = NULL;
static SemaphoreHandle_t s_codec_mutex = NULL;
#else /* M5StickCPlus2 */
static i2c_master_bus_handle_t s_i2c0_bus = NULL;
static i2s_chan_handle_t s_i2s_rx = NULL;   /* PDM mic RX on I2S0 */
static i2s_chan_handle_t s_i2s_tx = NULL;   /* Delta-sigma speaker TX on I2S1 */
static int32_t s_ds_accum = 0x8000;         /* Delta-sigma integrator state */
static esp_lcd_panel_handle_t s_lcd_panel = NULL;
static esp_lcd_panel_io_handle_t s_lcd_io = NULL;
static lv_disp_t *s_lvgl_disp = NULL;
static SemaphoreHandle_t s_codec_mutex = NULL;
#endif

// ============================================================================
// Board info
// ============================================================================

const char *board_get_name(void)
{
    return BOARD_NAME;
}

const char *board_get_mcu(void)
{
    return BOARD_MCU;
}

void *board_get_i2c0_handle(void)
{
    return (void *)s_i2c0_bus;
}

#if BOARD_HAS_DISPLAY
void board_tp_reset(bool active)
{
#if defined(CONFIG_HEYCLAWY_BOARD_WAVESHARE_AUDIO)
    if (s_io_exp) {
        esp_io_expander_set_level(s_io_exp, 1 << BOARD_IOEXP_TOUCH_RST, active ? 0 : 1);
    }
#endif
}
#endif

// ============================================================================
// I2C Bus Init
// ============================================================================

#if defined(CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER) || defined(CONFIG_HEYCLAWY_BOARD_WAVESHARE_AUDIO)

static esp_err_t board_i2c0_init(void)
{
    if (s_i2c0_bus != NULL) return ESP_OK;

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c0_bus), TAG, "I2C0 bus create failed");

    ESP_LOGI(TAG, "I2C0 initialized (SDA=%d, SCL=%d)", BOARD_I2C_SDA, BOARD_I2C_SCL);
    return ESP_OK;
}
#endif

#if defined(CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER)
static esp_err_t board_i2c1_init(void)
{
    if (s_i2c1_bus != NULL) return ESP_OK;

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BOARD_TOUCH_I2C_PORT,
        .sda_io_num = BOARD_TOUCH_SDA,
        .scl_io_num = BOARD_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c1_bus), TAG, "I2C1 bus create failed");

    ESP_LOGI(TAG, "I2C1 initialized (SDA=%d, SCL=%d)", BOARD_TOUCH_SDA, BOARD_TOUCH_SCL);
    return ESP_OK;
}
#endif /* SENSECAP_WATCHER */

// ============================================================================
// IO Expander (PCA9535 via TCA95xx driver)
// ============================================================================

static esp_err_t board_io_expander_init(void)
{
    if (s_io_exp != NULL) return ESP_OK;

    ESP_RETURN_ON_ERROR(
        esp_io_expander_new_i2c_tca95xx_16bit(s_i2c0_bus, BOARD_IO_EXP_ADDR, &s_io_exp),
        TAG, "IO expander init failed");

#if defined(CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER)
    // Configure Port 0 as inputs (detect/button pins)
    for (int i = 0; i <= 6; i++) {
        esp_io_expander_set_dir(s_io_exp, 1 << i, IO_EXPANDER_INPUT);
    }
    // P0.7 (SSCMA_RST) is output
    esp_io_expander_set_dir(s_io_exp, 1 << BOARD_IOEXP_SSCMA_RST, IO_EXPANDER_OUTPUT);

    // Configure Port 1 as outputs (power control)
    for (int i = 8; i <= 15; i++) {
        esp_io_expander_set_dir(s_io_exp, 1 << i, IO_EXPANDER_OUTPUT);
    }
#elif defined(CONFIG_HEYCLAWY_BOARD_WAVESHARE_AUDIO)
    // Outputs: LCD_RST(0), TOUCH_RST(1), CAM_EN(5), CAM_MUX(6), PA_EN(8)
    uint32_t output_mask = (1 << BOARD_IOEXP_LCD_RST) | (1 << BOARD_IOEXP_TOUCH_RST) |
                           (1 << BOARD_IOEXP_CAM_EN) | (1 << BOARD_IOEXP_CAM_MUX) |
                           (1 << BOARD_IOEXP_PA_EN);
    esp_io_expander_set_dir(s_io_exp, output_mask, IO_EXPANDER_OUTPUT);

    // Inputs: TOUCH_INT(2), BTN1(9), BTN2(10), BTN3(11)
    uint32_t input_mask = (1 << BOARD_IOEXP_TOUCH_INT) |
                          (1 << BOARD_IOEXP_BTN1) | (1 << BOARD_IOEXP_BTN2) | (1 << BOARD_IOEXP_BTN3);
    esp_io_expander_set_dir(s_io_exp, input_mask, IO_EXPANDER_INPUT);

    // Default all outputs low
    esp_io_expander_set_level(s_io_exp, output_mask, 0);
#endif

    ESP_LOGI(TAG, "IO expander initialized at 0x%02X", BOARD_IO_EXP_ADDR);
    return ESP_OK;
}

#if defined(CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER)
static esp_err_t board_power_on_sequence(void)
{
    if (s_io_exp == NULL) return ESP_ERR_INVALID_STATE;

    // Step 1: System power
    esp_io_expander_set_level(s_io_exp, 1 << BOARD_IOEXP_PWR_SYSTEM, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Step 2: All subsystem power rails (level=1 means HIGH for all masked pins)
    uint32_t power_mask = (1 << BOARD_IOEXP_PWR_SDCARD) |
                          (1 << BOARD_IOEXP_PWR_LCD) |
                          (1 << BOARD_IOEXP_PWR_AI) |
                          (1 << BOARD_IOEXP_PWR_PA) |
                          (1 << BOARD_IOEXP_PWR_GROVE) |
                          (1 << BOARD_IOEXP_PWR_BAT_ADC);
    esp_io_expander_set_level(s_io_exp, power_mask, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Power-on sequence complete");
    return ESP_OK;
}
#endif /* SENSECAP_WATCHER */

// ============================================================================
// RGB LED
// ============================================================================

esp_err_t board_rgb_init(void)
{
    if (s_rgb_handle != NULL) return ESP_OK;

    led_strip_config_t strip_config = {
        .strip_gpio_num = BOARD_RGB_GPIO,
        .max_leds = BOARD_RGB_LED_COUNT,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_rgb_handle),
                        TAG, "RGB LED init failed");

    led_strip_set_pixel(s_rgb_handle, 0, 0, 0, 0);
    led_strip_refresh(s_rgb_handle);

    ESP_LOGI(TAG, "RGB LED initialized (GPIO%d)", BOARD_RGB_GPIO);
    return ESP_OK;
}

esp_err_t board_rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_rgb_handle == NULL) return ESP_ERR_INVALID_STATE;
    for (int i = 0; i < BOARD_RGB_LED_COUNT; i++) {
        ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_rgb_handle, i, r, g, b), TAG, "RGB set failed");
    }
    return led_strip_refresh(s_rgb_handle);
}

esp_err_t board_rgb_set_single(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (s_rgb_handle == NULL) return ESP_ERR_INVALID_STATE;
    if (index >= BOARD_RGB_LED_COUNT) return ESP_ERR_INVALID_ARG;
    return led_strip_set_pixel(s_rgb_handle, index, r, g, b);
}

esp_err_t board_rgb_refresh(void)
{
    if (s_rgb_handle == NULL) return ESP_ERR_INVALID_STATE;
    return led_strip_refresh(s_rgb_handle);
}

/* ── RGB animation task ──────────────────────────────────────────────── */
static struct {
    board_rgb_mode_t mode;
    uint8_t r, g, b;
    bool running;
} s_rgb_anim = {0};

/* Gamma correction table for smoother LED brightness perception */
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

/* Apply gamma correction to RGB */
static inline void gamma_rgb(uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = s_gamma8[*r];
    *g = s_gamma8[*g];
    *b = s_gamma8[*b];
}

/* HSV to RGB helper (h: 0-360, s/v: 0-255) */
static void hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (s == 0) { *r = *g = *b = v; return; }
    uint8_t region = h / 60;
    uint16_t remainder = (h - (region * 60)) * 6;
    uint8_t p = (v * (255 - s)) >> 8;
    uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    uint8_t t = (v * (255 - ((s * (255 - (remainder & 0xFF))) >> 8))) >> 8;
    switch (region) {
    case 0:  *r = v; *g = t; *b = p; break;
    case 1:  *r = q; *g = v; *b = p; break;
    case 2:  *r = p; *g = v; *b = t; break;
    case 3:  *r = p; *g = q; *b = v; break;
    case 4:  *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
    }
}

static void rgb_anim_task(void *arg)
{
    float phase = 0.0f;
    int frame = 0;
    s_rgb_anim.running = true;

    while (1) {
        board_rgb_mode_t mode = s_rgb_anim.mode;
        uint8_t r = s_rgb_anim.r, g = s_rgb_anim.g, b = s_rgb_anim.b;
        int nleds = BOARD_RGB_LED_COUNT;

        switch (mode) {
        case RGB_MODE_SOLID:
            board_rgb_set(r, g, b);
            vTaskDelay(pdMS_TO_TICKS(200));
            break;

        case RGB_MODE_BREATHE: {
            phase += 0.04f;
            if (phase > 3.14159f) phase -= 3.14159f;
            float bright = sinf(phase);
            bright = bright * bright;
            uint8_t br = (uint8_t)(r * bright);
            uint8_t bg = (uint8_t)(g * bright);
            uint8_t bb = (uint8_t)(b * bright);
            gamma_rgb(&br, &bg, &bb);
            board_rgb_set(br, bg, bb);
            vTaskDelay(pdMS_TO_TICKS(20));
            break;
        }

        case RGB_MODE_BLINK:
            board_rgb_set(r, g, b);
            vTaskDelay(pdMS_TO_TICKS(500));
            board_rgb_set(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;

        case RGB_MODE_OFF:
            board_rgb_set(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;

        case RGB_MODE_RAINBOW_SPIN: {
            /* ═══ Option 1: Smooth Rotating Rainbow ═══
             * Flowing gradient sweeps around ring — every LED always lit
             * with a visible color.  Modeled after xiaozhi reference with
             * floating-point HSV, per-LED fractional offset, secondary
             * motion, and pulsing saturation.  NO gamma — already smooth. */
            const uint8_t bri = 50;
            float progress = fmodf(frame * 0.005f, 1.0f);          /* 0‥1 slow loop */
            uint16_t base_hue = (uint16_t)(progress * 360.0f);

            for (int i = 0; i < nleds; i++) {
                float led_off = (float)i / nleds;
                float motion  = progress * 2.0f;
                float hue_f   = base_hue + (led_off + motion) * 120.0f;
                uint16_t h    = ((int)hue_f) % 360;
                uint8_t  sat  = 200 + (uint8_t)(55.0f * sinf(progress * 3.14159f));
                uint8_t cr, cg, cb;
                hsv_to_rgb(h, sat, bri, &cr, &cg, &cb);
                led_strip_set_pixel(s_rgb_handle, i, cr, cg, cb);
            }
            led_strip_refresh(s_rgb_handle);
            frame++;
            vTaskDelay(pdMS_TO_TICKS(16));
            break;
        }

        case RGB_MODE_AURORA: {
            /* ═══ Option 2: Aurora Borealis ═══
             * Overlapping sine waves in green, teal, purple — organic
             * northern lights that shimmer and morph.  All LEDs always
             * lit with visible colors. */
            const uint8_t bri = 50;
            float t = frame * 0.008f;

            for (int i = 0; i < nleds; i++) {
                float pos = (float)i / nleds;
                float w1 = (1.0f + sinf(pos * 4.2f + t * 1.0f)) * 0.5f;
                float w2 = (1.0f + sinf(pos * 7.1f - t * 1.7f)) * 0.5f;
                float w3 = (1.0f + sinf(pos * 3.0f + t * 0.6f)) * 0.5f;
                /* Aurora palette: green dominant, purple/teal accents */
                uint8_t cr = (uint8_t)(bri * 0.25f * w3);
                uint8_t cg = (uint8_t)(bri * (0.3f + 0.7f * w1));
                uint8_t cb = (uint8_t)(bri * (0.2f + 0.6f * w2));
                led_strip_set_pixel(s_rgb_handle, i, cr, cg, cb);
            }
            led_strip_refresh(s_rgb_handle);
            frame++;
            vTaskDelay(pdMS_TO_TICKS(20));
            break;
        }

        case RGB_MODE_STARFIELD: {
            /* ═══ Option 3: Twinkling Stars ═══
             * Each LED fades in/out at its own unique rate with warm
             * golds and occasional cool blues.  Always at least dimly
             * lit so the ring looks full of stars. */
            const uint8_t bri = 50;
            for (int i = 0; i < nleds; i++) {
                float freq = 0.015f + (i * 7 % 11) * 0.004f;
                float ph   = (i * 137 % 360) * 0.01745f;
                float lum  = (1.0f + sinf(frame * freq + ph)) * 0.5f;
                lum = 0.12f + 0.88f * lum * lum * lum;   /* floor + cubic snap */
                uint16_t hue = (i * 47 + frame / 80) % 60;
                if (i % 3 == 0) hue = 210 + (i * 13 % 30);
                uint8_t sat = (i % 3 == 0) ? 140 : 70;
                uint8_t cr, cg, cb;
                hsv_to_rgb(hue, sat, (uint8_t)(bri * lum), &cr, &cg, &cb);
                led_strip_set_pixel(s_rgb_handle, i, cr, cg, cb);
            }
            led_strip_refresh(s_rgb_handle);
            frame++;
            vTaskDelay(pdMS_TO_TICKS(20));
            break;
        }

        case RGB_MODE_FIRE: {
            /* ═══ Option 4: Fire Embers ═══
             * Warm flickering reds, oranges, yellows.  All LEDs glow —
             * never fully dark.  Pseudo-random noise for organic feel. */
            const uint8_t bri = 50;
            for (int i = 0; i < nleds; i++) {
                float base  = (1.0f + sinf(frame * 0.04f + i * 2.1f)) * 0.25f;
                float flick = (1.0f + sinf(frame * 0.09f - i * 3.7f)) * 0.25f;
                uint32_t hash = (frame * 2654435761U + i * 40503U) >> 24;
                float noise = (float)hash / 255.0f * 0.25f;
                float inten = base + flick + noise;
                if (inten > 1.0f) inten = 1.0f;
                inten = 0.15f + 0.85f * inten * inten;   /* floor + squared */
                uint16_t hue = (uint16_t)(inten * 35.0f);
                uint8_t  sat = 255 - (uint8_t)(inten * 30.0f);
                uint8_t cr, cg, cb;
                hsv_to_rgb(hue, sat, (uint8_t)(bri * inten), &cr, &cg, &cb);
                led_strip_set_pixel(s_rgb_handle, i, cr, cg, cb);
            }
            led_strip_refresh(s_rgb_handle);
            frame++;
            vTaskDelay(pdMS_TO_TICKS(25));
            break;
        }

        case RGB_MODE_OCEAN: {
            /* ═══ Option 5: Ocean Waves ═══
             * Deep blues and teals with bright crests sweeping around.
             * Two wave layers at different speeds — all LEDs always lit
             * with a deep blue minimum. */
            const uint8_t bri = 50;
            float t = frame * 0.012f;

            for (int i = 0; i < nleds; i++) {
                float pos  = (float)i / nleds * 6.283f;
                float deep = (1.0f + sinf(pos * 1.5f - t)) * 0.5f;
                float surf = (1.0f + sinf(pos * 3.0f - t * 2.3f)) * 0.5f;
                float comb = deep * 0.6f + surf * 0.4f;
                /* Always-visible deep blue base + teal/white at peaks */
                uint8_t cr = (uint8_t)(bri * 0.08f * comb);
                uint8_t cg = (uint8_t)(bri * (0.12f + 0.45f * comb));
                uint8_t cb = (uint8_t)(bri * (0.35f + 0.55f * comb));
                led_strip_set_pixel(s_rgb_handle, i, cr, cg, cb);
            }
            led_strip_refresh(s_rgb_handle);
            frame++;
            vTaskDelay(pdMS_TO_TICKS(20));
            break;
        }

        case RGB_MODE_CHASE: {
            /* Smooth comet chasing around ring with long gradient tail */
            float head_f = fmodf(frame * 0.3f, (float)nleds);
            for (int i = 0; i < nleds; i++) {
                float dist = fmodf(head_f - i + nleds, (float)nleds);
                float fade = 0.0f;
                if (dist < 1.0f) fade = 1.0f;
                else if (dist < 4.0f) fade = 1.0f / (dist * dist);
                uint8_t pr = (uint8_t)(r * fade);
                uint8_t pg = (uint8_t)(g * fade);
                uint8_t pb = (uint8_t)(b * fade);
                gamma_rgb(&pr, &pg, &pb);
                led_strip_set_pixel(s_rgb_handle, i, pr, pg, pb);
            }
            led_strip_refresh(s_rgb_handle);
            frame++;
            vTaskDelay(pdMS_TO_TICKS(20));
            break;
        }

        case RGB_MODE_PULSE_WAVE: {
            /* Smooth sine wave pulse travels around ring with gamma */
            for (int i = 0; i < nleds; i++) {
                float pos = (float)i / nleds;
                float wave = (1.0f + sinf((pos * 6.283f) - phase)) * 0.5f;
                wave = wave * wave;
                uint8_t pr = (uint8_t)(r * wave);
                uint8_t pg = (uint8_t)(g * wave);
                uint8_t pb = (uint8_t)(b * wave);
                gamma_rgb(&pr, &pg, &pb);
                led_strip_set_pixel(s_rgb_handle, i, pr, pg, pb);
            }
            led_strip_refresh(s_rgb_handle);
            phase += 0.08f;
            if (phase > 6.283f) phase -= 6.283f;
            vTaskDelay(pdMS_TO_TICKS(20));
            break;
        }

        case RGB_MODE_SPARKLE: {
            /* Random sparkle in base color */
            for (int i = 0; i < nleds; i++) {
                if ((esp_random() & 0x07) == 0) {
                    /* Sparkle flash */
                    led_strip_set_pixel(s_rgb_handle, i, r, g, b);
                } else {
                    /* Dim base */
                    led_strip_set_pixel(s_rgb_handle, i, r / 8, g / 8, b / 8);
                }
            }
            led_strip_refresh(s_rgb_handle);
            vTaskDelay(pdMS_TO_TICKS(60));
            break;
        }
        }
    }
}

esp_err_t board_rgb_animate(board_rgb_mode_t mode, uint8_t r, uint8_t g, uint8_t b)
{
    s_rgb_anim.mode = mode;
    s_rgb_anim.r = r;
    s_rgb_anim.g = g;
    s_rgb_anim.b = b;
    return ESP_OK;
}

esp_err_t board_rgb_task_start(void)
{
    if (s_rgb_anim.running) return ESP_OK;
    xTaskCreatePinnedToCore(rgb_anim_task, "rgb_anim", 4096, NULL, 1, NULL, 1);
    return ESP_OK;
}

// ============================================================================
// LCD Display (SPD2010 QSPI / ST7789 SPI) + LVGL
// ============================================================================
#if BOARD_HAS_DISPLAY

#if defined(CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER)
static void lvgl_rounder_cb(lv_disp_drv_t *disp_drv, lv_area_t *area)
{
    // SPD2010 requires x coords divisible by 4
    area->x1 = (area->x1 >> 2) << 2;
    area->x2 = ((area->x2 >> 2) << 2) + 3;
}
#endif

esp_err_t board_display_init(void)
{
    // Initialize backlight via LEDC PWM
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&bl_timer), TAG, "LEDC timer failed");

    const ledc_channel_config_t bl_channel = {
        .gpio_num = BOARD_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&bl_channel), TAG, "LEDC channel failed");

    ESP_LOGI(TAG, "Display backlight initialized (GPIO%d)", BOARD_LCD_BL);

#if defined(CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER)
    // Initialize QSPI bus for LCD
    const spi_bus_config_t buscfg = SPD2010_PANEL_BUS_QSPI_CONFIG(
        BOARD_QSPI_PCLK,
        BOARD_QSPI_DATA0,
        BOARD_QSPI_DATA1,
        BOARD_QSPI_DATA2,
        BOARD_QSPI_DATA3,
        BOARD_LCD_H_RES * 80 * sizeof(uint16_t)
    );
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "LCD SPI init failed");

    // Install panel IO
    const esp_lcd_panel_io_spi_config_t io_config = SPD2010_PANEL_IO_QSPI_CONFIG(
        BOARD_LCD_CS, NULL, NULL
    );
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_config, &s_lcd_io),
                        TAG, "LCD panel IO failed");

    // Install SPD2010 panel driver
    const spd2010_vendor_config_t vendor_config = {
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BOARD_LCD_BPP,
        .vendor_config = (void *)&vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_spd2010(s_lcd_io, &panel_config, &s_lcd_panel), TAG, "LCD panel create failed");
#elif defined(CONFIG_HEYCLAWY_BOARD_WAVESHARE_AUDIO)
    // Reset LCD via IO expander
    if (s_io_exp) {
        esp_io_expander_set_level(s_io_exp, 1 << BOARD_IOEXP_LCD_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        esp_io_expander_set_level(s_io_exp, 1 << BOARD_IOEXP_LCD_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    // SPI bus
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = BOARD_LCD_SCLK,
        .mosi_io_num = BOARD_LCD_MOSI,
        .miso_io_num = BOARD_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_H_RES * BOARD_LCD_V_RES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "SPI init failed");

    // Panel IO
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_LCD_DC,
        .cs_gpio_num = BOARD_LCD_CS,
        .pclk_hz = BOARD_LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits = BOARD_LCD_CMD_BITS,
        .lcd_param_bits = BOARD_LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_cfg, &s_lcd_io), TAG, "Panel IO failed");

    // JD9853 Panel
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1, // reset handled by IO expander
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BOARD_LCD_BPP,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9853(s_lcd_io, &panel_cfg, &s_lcd_panel), TAG, "Panel init failed");
    // Set gap for 1.47" panel. If content appears on right edge, try 0.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_lcd_panel, 34, 0), TAG, "Panel set gap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_lcd_panel, true), TAG, "LCD invert failed");
#endif

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_lcd_panel), TAG, "LCD reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_lcd_panel), TAG, "LCD init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_lcd_panel, false, false), TAG, "LCD mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_lcd_panel, true), TAG, "LCD on failed");

    ESP_LOGI(TAG, "LCD initialized (%dx%d)", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return ESP_OK;
}

esp_err_t board_display_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint32_t duty = (1024 * percent) / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty), TAG, "ledc_set_duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1), TAG, "ledc_update_duty");
    return ESP_OK;
}

// ============================================================================
// Touch Panel (SPD2010 touch on I2C1)
// ============================================================================
// Custom driver using direct i2c_master API — the managed component's
// esp_lcd_panel_io_rx_param() fails on ESP-IDF v5.5 because the new
// i2c_master driver rejects zero-length writes in transmit_receive().

#if defined(CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER)
static i2c_master_dev_handle_t s_touch_i2c_dev = NULL;

static esp_err_t spd2010_i2c_write(const uint8_t *data, size_t len)
{
    return i2c_master_transmit(s_touch_i2c_dev, data, len, 100);
}

static esp_err_t spd2010_i2c_read(uint8_t *data, size_t len)
{
    return i2c_master_receive(s_touch_i2c_dev, data, len, 100);
}

/* SPD2010 touch protocol helpers — ported from managed component */

static esp_err_t spd2010_read_fw_version(void)
{
    uint8_t buf[18];
    buf[0] = 0x26; buf[1] = 0x00;
    ESP_RETURN_ON_ERROR(spd2010_i2c_write(buf, 2), TAG, "Touch FW ver write");
    esp_rom_delay_us(200);
    ESP_RETURN_ON_ERROR(spd2010_i2c_read(buf, 18), TAG, "Touch FW ver read");
    uint16_t dver = (buf[5] << 8) | buf[4];
    ESP_LOGI(TAG, "SPD2010 touch FW ver: %u", dver);
    return ESP_OK;
}

static esp_err_t spd2010_write_cmd(uint8_t cmd, uint8_t b1, uint8_t b2, uint8_t b3)
{
    uint8_t buf[4] = {cmd, b1, b2, b3};
    esp_err_t ret = spd2010_i2c_write(buf, 4);
    esp_rom_delay_us(200);
    return ret; /* Caller handles errors gracefully */
}

static esp_err_t spd2010_clear_int(void) { return spd2010_write_cmd(0x02, 0x00, 0x01, 0x00); }
static esp_err_t spd2010_cpu_start(void) { return spd2010_write_cmd(0x04, 0x00, 0x01, 0x00); }
static esp_err_t spd2010_point_mode(void) { return spd2010_write_cmd(0x50, 0x00, 0x00, 0x00); }
static esp_err_t spd2010_touch_start(void) { return spd2010_write_cmd(0x46, 0x00, 0x00, 0x00); }

static esp_err_t spd2010_read_data_impl(esp_lcd_touch_handle_t tp)
{
    uint8_t buf[4 + 10 * 6]; // header + max 10 fingers * 6 bytes
    /* Read status + length */
    buf[0] = 0x20; buf[1] = 0x00;
    if (spd2010_i2c_write(buf, 2) != ESP_OK) return ESP_OK; /* Graceful: touch not ready */
    esp_rom_delay_us(200);
    if (spd2010_i2c_read(buf, 4) != ESP_OK) return ESP_OK;
    esp_rom_delay_us(200);

    uint8_t status_low = buf[0];
    uint8_t status_high = buf[1];
    uint16_t read_len = (buf[3] << 8) | buf[2];
    bool pt_exist = status_low & 0x01;
    bool gesture = status_low & 0x02;
    bool tic_busy = (status_high >> 7) & 1;
    bool tic_in_bios = (status_high >> 6) & 1;
    bool tic_in_cpu = (status_high >> 5) & 1;
    bool cpu_run = (status_high >> 3) & 1;
    bool aux = status_low & 0x08;
    (void)tic_busy;

    if (tic_in_bios) {
        spd2010_clear_int();
        spd2010_cpu_start();
    } else if (tic_in_cpu) {
        spd2010_point_mode();
        spd2010_touch_start();
        spd2010_clear_int();
    } else if (cpu_run && read_len == 0) {
        spd2010_clear_int();
    } else if (pt_exist || gesture) {
        if (read_len > sizeof(buf)) read_len = sizeof(buf);
        /* Read HDP */
        buf[0] = 0x00; buf[1] = 0x03;
        ESP_RETURN_ON_ERROR(spd2010_i2c_write(buf, 2), TAG, "Touch HDP write");
        esp_rom_delay_us(200);
        ESP_RETURN_ON_ERROR(spd2010_i2c_read(buf, read_len), TAG, "Touch HDP read");
        esp_rom_delay_us(200);

        uint8_t check_id = buf[4];
        if (check_id <= 0x0A && pt_exist) {
            uint8_t touch_num = (read_len - 4) / 6;
            if (touch_num > CONFIG_ESP_LCD_TOUCH_MAX_POINTS)
                touch_num = CONFIG_ESP_LCD_TOUCH_MAX_POINTS;
            portENTER_CRITICAL(&tp->data.lock);
            tp->data.points = touch_num;
            for (int i = 0; i < touch_num; i++) {
                int off = i * 6;
                tp->data.coords[i].x = ((buf[7+off] & 0xF0) << 4) | buf[5+off];
                tp->data.coords[i].y = ((buf[7+off] & 0x0F) << 8) | buf[6+off];
                tp->data.coords[i].strength = buf[8+off];
            }
            portEXIT_CRITICAL(&tp->data.lock);
        }

        /* Read HDP status and clear */
        uint8_t hdp_buf[8];
        hdp_buf[0] = 0xFC; hdp_buf[1] = 0x02;
        spd2010_i2c_write(hdp_buf, 2);
        esp_rom_delay_us(200);
        spd2010_i2c_read(hdp_buf, 8);
        esp_rom_delay_us(200);
        uint8_t hdp_status = hdp_buf[5];
        if (hdp_status == 0x82) {
            spd2010_clear_int();
        }
    } else if (cpu_run && aux) {
        spd2010_clear_int();
    }

    return ESP_OK;
}

static bool spd2010_get_xy_impl(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                                 uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    portENTER_CRITICAL(&tp->data.lock);
    *point_num = (tp->data.points > max_point_num ? max_point_num : tp->data.points);
    for (size_t i = 0; i < *point_num; i++) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;
        if (strength) strength[i] = tp->data.coords[i].strength;
    }
    tp->data.points = 0;
    portEXIT_CRITICAL(&tp->data.lock);
    return (*point_num > 0);
}

static esp_err_t spd2010_del_impl(esp_lcd_touch_handle_t tp)
{
    free(tp);
    return ESP_OK;
}
#endif

esp_err_t board_touch_init(void)
{
#if defined(CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER)
    ESP_RETURN_ON_ERROR(board_i2c1_init(), TAG, "I2C1 for touch");

    /* Create direct I2C device at SPD2010 address */
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x53,
        .scl_speed_hz = BOARD_TOUCH_I2C_FREQ,
    };
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(s_i2c1_bus, &dev_cfg, &s_touch_i2c_dev),
        TAG, "Touch I2C device add failed");

    /* Read FW version to verify touch is alive */
    esp_err_t ret = spd2010_read_fw_version();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Touch FW version read failed (0x%x), retrying after delay...", ret);
        vTaskDelay(pdMS_TO_TICKS(100));
        ret = spd2010_read_fw_version();
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Touch panel not responding at 0x53 (err=0x%x)", ret);
        i2c_master_bus_rm_device(s_touch_i2c_dev);
        s_touch_i2c_dev = NULL;
        return ret;
    }

    /* Create touch handle manually */
    esp_lcd_touch_handle_t tp = calloc(1, sizeof(esp_lcd_touch_t));
    ESP_RETURN_ON_FALSE(tp, ESP_ERR_NO_MEM, TAG, "Touch alloc failed");
    tp->io = NULL; /* We don't use panel IO */
    tp->read_data = spd2010_read_data_impl;
    tp->get_xy = spd2010_get_xy_impl;
    tp->del = spd2010_del_impl;
    tp->data.lock.owner = portMUX_FREE_VAL;
    tp->config.x_max = BOARD_LCD_H_RES;
    tp->config.y_max = BOARD_LCD_V_RES;
    tp->config.rst_gpio_num = GPIO_NUM_NC;
    tp->config.int_gpio_num = GPIO_NUM_NC;

    s_touch_handle = tp;
    ESP_LOGI(TAG, "Touch panel initialized (custom I2C driver, 0x53)");
#elif defined(CONFIG_HEYCLAWY_BOARD_WAVESHARE_AUDIO)
    // AXS5106L Touch via custom driver
    touch_axs5106l_config_t touch_cfg = TOUCH_AXS5106L_CONFIG_DEFAULT();
    touch_cfg.x_max = BOARD_LCD_H_RES - 1;
    touch_cfg.y_max = BOARD_LCD_V_RES - 1;
    ESP_RETURN_ON_ERROR(touch_axs5106l_init(&touch_cfg), TAG, "Touch hardware init failed");
    ESP_RETURN_ON_ERROR(touch_axs5106l_register_lvgl(), TAG, "Touch LVGL registration failed");
#endif
    return ESP_OK;
}

// ============================================================================
// LVGL Setup
// ============================================================================

esp_err_t board_lvgl_init(void)
{
    // Initialize LVGL port
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port init failed");

    // Add display
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_lcd_io,
        .panel_handle = s_lcd_panel,
        .buffer_size = BOARD_LCD_H_RES * 10 * sizeof(lv_color_t),
        .double_buffer = false,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
        },
    };
    s_lvgl_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_lvgl_disp == NULL) {
        ESP_LOGE(TAG, "LVGL display add failed");
        return ESP_FAIL;
    }

#if defined(CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER)
    // Install rounder callback for SPD2010
    lv_disp_drv_t *drv = s_lvgl_disp->driver;
    drv->rounder_cb = lvgl_rounder_cb;

    // Add touch input for SenseCAP Watcher (using s_touch_handle)
    if (s_touch_handle != NULL) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = s_lvgl_disp,
            .handle = s_touch_handle,
        };
        lv_indev_t *touch_indev = lvgl_port_add_touch(&touch_cfg);
        if (touch_indev == NULL) {
            ESP_LOGW(TAG, "LVGL touch add failed (non-critical)");
        }
    }
#endif

    ESP_LOGI(TAG, "LVGL initialized with display and touch");
    return ESP_OK;
}

lv_disp_t *board_get_lvgl_disp(void)
{
    return s_lvgl_disp;
}

#endif /* BOARD_HAS_DISPLAY */

// ============================================================================
// Audio (I2S + ES8311 speaker + mic codec via esp_codec_dev framework)
// ============================================================================

/* Mic gain in dB (ES7243E range: 0-37.5dB, Seeed default: 30dB) */
#define MIC_GAIN_DB 37.5f

esp_err_t board_audio_init(void)
{
    if (s_i2s_tx != NULL) return ESP_OK;

    s_codec_mutex = xSemaphoreCreateMutex();

    // Create I2S channels (TX for speaker, RX for mic)
    i2s_chan_config_t chan_cfg = {
        .id = BOARD_I2S_NUM,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 8,      /* Balanced: enough for smooth playback without excessive RAM usage */
        .dma_frame_num = 256,   /* 256 frames per DMA buffer */
        .auto_clear = true,
        .intr_priority = 5,     /* Higher interrupt priority for timely DMA handling */
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_i2s_tx, &s_i2s_rx), TAG, "I2S channel create failed");

    // I2S config matching Seeed BSP: mono, left-aligned, bit-shifted
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_SAMPLE_RATE),
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_SCLK,
            .ws   = BOARD_I2S_LRCK,
            .dout = BOARD_I2S_DOUT,
            .din  = BOARD_I2S_DSIN,
            .invert_flags = { false, false, false },
        },
    };

    // TX (speaker): use default SLOT_BOTH
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg), TAG, "I2S TX init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "I2S TX enable failed");

    // RX (mic): use both slots to capture stereo data from ES7210
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_rx, &std_cfg), TAG, "I2S RX init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_rx), TAG, "I2S RX enable failed");

    ESP_LOGI(TAG, "Audio I2S initialized (MCLK=%d, SCLK=%d, LRCK=%d, DIN=%d, DOUT=%d)",
             BOARD_I2S_MCLK, BOARD_I2S_SCLK, BOARD_I2S_LRCK, BOARD_I2S_DSIN, BOARD_I2S_DOUT);

    // Create I2S data interface for codec dev framework
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = BOARD_I2S_NUM,
        .rx_handle = s_i2s_rx,
        .tx_handle = s_i2s_tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!data_if) {
        ESP_LOGE(TAG, "Failed to create I2S data interface");
        return ESP_FAIL;
    }

    // --- Speaker codec (ES8311 DAC) ---
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t spk_i2c_cfg = {
        .port = BOARD_I2C_PORT,
        .addr = BOARD_ES8311_ADDR << 1,  // 8-bit address
        .bus_handle = s_i2c0_bus,
    };
    const audio_codec_ctrl_if_t *spk_ctrl = audio_codec_new_i2c_ctrl(&spk_i2c_cfg);
    if (!spk_ctrl) {
        ESP_LOGE(TAG, "Failed to create ES8311 I2C ctrl");
        return ESP_FAIL;
    }
    esp_codec_dev_hw_gain_t hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 };
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = spk_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_NC,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = hw_gain,
    };
    const audio_codec_if_t *es8311_dev = es8311_codec_new(&es8311_cfg);
    if (!es8311_dev) {
        ESP_LOGE(TAG, "Failed to create ES8311 codec");
        return ESP_FAIL;
    }
    esp_codec_dev_cfg_t spk_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_dev,
        .data_if = data_if,
    };
    s_play_dev = esp_codec_dev_new(&spk_dev_cfg);
    if (!s_play_dev) {
        ESP_LOGE(TAG, "Failed to create speaker codec dev");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ES8311 speaker codec initialized");

    // --- Microphone codec (board-specific ADC) ---
#if defined(CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER)
    /* ES7243E ADC */
    audio_codec_i2c_cfg_t mic_i2c_cfg = {
        .port = BOARD_I2C_PORT,
        .addr = BOARD_ES7243E_ADDR << 1,
        .bus_handle = s_i2c0_bus,
    };
    const audio_codec_ctrl_if_t *mic_ctrl = audio_codec_new_i2c_ctrl(&mic_i2c_cfg);
    if (!mic_ctrl) {
        ESP_LOGW(TAG, "ES7243E I2C ctrl failed (addr=0x%02x)", BOARD_ES7243E_ADDR);
        return ESP_OK;
    }
    es7243e_codec_cfg_t es7243e_cfg = { .ctrl_if = mic_ctrl };
    const audio_codec_if_t *mic_codec = es7243e_codec_new(&es7243e_cfg);
    if (!mic_codec) {
        ESP_LOGW(TAG, "ES7243E codec create failed");
        return ESP_OK;
    }
#elif defined(CONFIG_HEYCLAWY_BOARD_WAVESHARE_AUDIO)
    /* ES7210 ADC (4-channel, dual mic array) */
    audio_codec_i2c_cfg_t mic_i2c_cfg = {
        .port = BOARD_I2C_PORT,
        .addr = BOARD_ES7210_ADDR << 1,  // 8-bit address
        .bus_handle = s_i2c0_bus,
    };
    const audio_codec_ctrl_if_t *mic_ctrl = audio_codec_new_i2c_ctrl(&mic_i2c_cfg);
    if (!mic_ctrl) {
        ESP_LOGW(TAG, "ES7210 I2C ctrl failed (addr=0x%02x)", BOARD_ES7210_ADDR);
        return ESP_OK;
    }
    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = mic_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *mic_codec = es7210_codec_new(&es7210_cfg);
    if (!mic_codec) {
        ESP_LOGW(TAG, "ES7210 codec create failed");
        return ESP_OK;
    }
#else
    const audio_codec_if_t *mic_codec = NULL;
#endif

    if (mic_codec) {
        esp_codec_dev_cfg_t mic_dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN,
            .codec_if = mic_codec,
            .data_if = data_if,
        };
        s_record_dev = esp_codec_dev_new(&mic_dev_cfg);
        if (!s_record_dev) {
            ESP_LOGW(TAG, "Failed to create mic codec dev");
        }
    }

    // Open both codec devices with proper sample config
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = BOARD_AUDIO_SAMPLE_RATE,
    };
    xSemaphoreTake(s_codec_mutex, portMAX_DELAY);
    if (s_play_dev) {
        int ret = esp_codec_dev_open(s_play_dev, &fs);
        if (ret != 0) ESP_LOGW(TAG, "Speaker codec open failed: %d", ret);
    }
    if (s_record_dev) {
        // Mic: stereo bus with channel extraction
        fs.channel = 2;
        /* Waveshare board has dual mics. Try to extract from channel 1 (Right)
         * as channel 0 (Left) produced very low RMS. */
        fs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
        int ret = esp_codec_dev_open(s_record_dev, &fs);
        if (ret != 0) ESP_LOGW(TAG, "Mic codec open failed: %d", ret);
        esp_codec_dev_set_in_gain(s_record_dev, MIC_GAIN_DB);
    }
    xSemaphoreGive(s_codec_mutex);

    ESP_LOGI(TAG, "Mic codec initialized (gain=%.0fdB)", MIC_GAIN_DB);
    return ESP_OK;
}

esp_err_t board_audio_record(int16_t *buffer, size_t samples, size_t *samples_read)
{
    if (s_record_dev == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_codec_mutex, portMAX_DELAY);
    int ret = esp_codec_dev_read(s_record_dev, buffer, samples * sizeof(int16_t));
    xSemaphoreGive(s_codec_mutex);
    if (samples_read) *samples_read = (ret == 0) ? samples : 0;
    return (ret == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t board_audio_play(const int16_t *buffer, size_t samples, size_t *samples_written)
{
    if (s_play_dev == NULL) return ESP_ERR_INVALID_STATE;
    
    /* For MP3 playback, avoid mutex overhead by using try-lock.
     * If lock is not available, it means reconfig is in progress,
     * so we skip this write to avoid blocking the audio pipeline. */
    if (xSemaphoreTake(s_codec_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        /* Reconfiguration in progress, skip this write */
        if (samples_written) *samples_written = 0;
        return ESP_ERR_INVALID_STATE;
    }
    
    int ret = esp_codec_dev_write(s_play_dev, (void *)buffer, samples * sizeof(int16_t));
    xSemaphoreGive(s_codec_mutex);
    
    if (samples_written) *samples_written = (ret == 0) ? samples : 0;
    return (ret == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t board_audio_set_volume(int volume_percent)
{
    if (!s_play_dev) return ESP_ERR_INVALID_STATE;
    if (volume_percent < 0) volume_percent = 0;
    if (volume_percent > 100) volume_percent = 100;

    xSemaphoreTake(s_codec_mutex, portMAX_DELAY);
    int ret = esp_codec_dev_set_out_vol(s_play_dev, volume_percent);
    xSemaphoreGive(s_codec_mutex);

    ESP_LOGI(TAG, "Volume set to %d%%", volume_percent);
    return (ret == 0) ? ESP_OK : ESP_FAIL;
}

/* Short tick sound for touch feedback — a 2ms click at 16kHz */
void board_play_tick(void)
{
    if (!s_play_dev) return;
    /* Generate a ~2ms click: a single cycle square wave pulse */
    static const int16_t tick_pcm[] = {
        8000, 8000, 8000, 8000, -8000, -8000, -8000, -8000,
        6000, 6000, -6000, -6000, 4000, -4000, 2000, -2000,
        8000, 8000, 8000, 8000, -8000, -8000, -8000, -8000,
        6000, 6000, -6000, -6000, 4000, -4000, 2000, -2000,
    };
    size_t written = 0;
    board_audio_play(tick_pcm, sizeof(tick_pcm) / sizeof(tick_pcm[0]), &written);
}

#if !defined(CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2)

esp_err_t board_audio_get_volume(int *volume_percent)
{
    if (!s_play_dev || !volume_percent) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_codec_mutex, portMAX_DELAY);
    int ret = esp_codec_dev_get_out_vol(s_play_dev, volume_percent);
    xSemaphoreGive(s_codec_mutex);
    return (ret == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t board_audio_mute(bool mute)
{
    if (!s_play_dev) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_codec_mutex, portMAX_DELAY);
    int ret = esp_codec_dev_set_out_mute(s_play_dev, mute);
    xSemaphoreGive(s_codec_mutex);
    return (ret == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t board_audio_reconfig(uint32_t sample_rate, uint8_t channels)
{
    if (!s_play_dev || !s_i2s_tx || !s_i2s_rx) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Reconfig audio: rate=%lu, ch=%d", (unsigned long)sample_rate, channels);

    xSemaphoreTake(s_codec_mutex, portMAX_DELAY);

    /* Close DAC only (ADC stays open — AFE/mic consumer is already paused).
     * Then disable both I2S channels to safely change clock/slot config. */
    esp_codec_dev_close(s_play_dev);

    i2s_chan_info_t tx_info = {0};
    i2s_channel_get_info(s_i2s_tx, &tx_info);
    if (tx_info.is_enabled) i2s_channel_disable(s_i2s_tx);

    i2s_chan_info_t rx_info = {0};
    i2s_channel_get_info(s_i2s_rx, &rx_info);
    if (rx_info.is_enabled) i2s_channel_disable(s_i2s_rx);

    /* Use reconfig (not init) to avoid "channel has initialized already" error
     * on paired full-duplex I2S. TX and RX share the same BCLK/LRCK pins,
     * so both must get the same clock config. */
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);

    i2s_slot_mode_t slot_mode = (channels > 1) ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO;
    i2s_std_slot_config_t tx_slot_cfg = {
        .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
        .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
        .slot_mode = slot_mode,
        .slot_mask = I2S_STD_SLOT_BOTH,
        .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
        .ws_pol = false,
        .bit_shift = true,
        .left_align = true,
        .big_endian = false,
        .bit_order_lsb = false,
    };

    /* RX slot config must match TX for proper stereo/mono handling */
    i2s_std_slot_config_t rx_slot_cfg = tx_slot_cfg;

    esp_err_t ret;
    ret = i2s_channel_reconfig_std_clock(s_i2s_tx, &clk_cfg);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "TX clock reconfig: %d", ret); goto reconfig_fail; }
    ret = i2s_channel_reconfig_std_slot(s_i2s_tx, &tx_slot_cfg);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "TX slot reconfig: %d", ret); goto reconfig_fail; }
    
    /* Reconfigure BOTH clock AND slot for RX to avoid conflicts */
    ret = i2s_channel_reconfig_std_clock(s_i2s_rx, &clk_cfg);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "RX clock reconfig: %d", ret); goto reconfig_fail; }
    ret = i2s_channel_reconfig_std_slot(s_i2s_rx, &rx_slot_cfg);
    if (ret != ESP_OK) { ESP_LOGW(TAG, "RX slot reconfig: %d (may be OK if RX not in use)", ret); }

    /* Re-enable RX first, then TX (TX=master on this board) */
    i2s_channel_enable(s_i2s_rx);
    i2s_channel_enable(s_i2s_tx);

    /* Re-open DAC with new sample rate */
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = (channels > 0) ? channels : 1,
        .channel_mask = 0,
        .sample_rate = sample_rate,
    };
    ret = esp_codec_dev_open(s_play_dev, &fs);
    if (ret != 0) {
        ESP_LOGE(TAG, "DAC reopen failed: %d", ret);
        goto reconfig_fail;
    }

    xSemaphoreGive(s_codec_mutex);

    /* Longer delay to let I2S hardware and codec stabilize after reconfiguration */
    vTaskDelay(pdMS_TO_TICKS(10));  /* Increased from 5ms to 10ms for better stability */

    ESP_LOGI(TAG, "Audio reconfigured to %lu Hz, %d ch", (unsigned long)sample_rate, fs.channel);
    return ESP_OK;

reconfig_fail:
    /* Try to restore I2S to default rate and reopen DAC */
    i2s_std_clk_config_t fallback_clk = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_SAMPLE_RATE);
    i2s_std_slot_config_t fallback_slot = {
        .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
        .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
        .slot_mode = I2S_SLOT_MODE_MONO,
        .slot_mask = I2S_STD_SLOT_BOTH,
        .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
        .ws_pol = false,
        .bit_shift = true,
        .left_align = true,
        .big_endian = false,
        .bit_order_lsb = false,
    };
    i2s_channel_reconfig_std_clock(s_i2s_tx, &fallback_clk);
    i2s_channel_reconfig_std_slot(s_i2s_tx, &fallback_slot);
    i2s_channel_reconfig_std_clock(s_i2s_rx, &fallback_clk);
    i2s_channel_reconfig_std_slot(s_i2s_rx, &fallback_slot);
    i2s_channel_enable(s_i2s_rx);
    i2s_channel_enable(s_i2s_tx);
    esp_codec_dev_sample_info_t fallback_fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = BOARD_AUDIO_SAMPLE_RATE,
    };
    esp_codec_dev_open(s_play_dev, &fallback_fs);
    xSemaphoreGive(s_codec_mutex);
    return ESP_FAIL;
}

/**
 * Re-open ADC device after MP3 playback.
 * During MP3 playback, ADC was left open but I2S config changed to 48kHz stereo.
 * This function closes and re-opens ADC with correct 16kHz mono config for wake word.
 */
esp_err_t board_audio_reopen_adc(void)
{
    if (!s_record_dev || !s_i2s_rx) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Reopening ADC for wake word detection");

    xSemaphoreTake(s_codec_mutex, portMAX_DELAY);

    /* Close ADC first */
    esp_codec_dev_close(s_record_dev);

    /* Disable RX channel */
    i2s_chan_info_t rx_info = {0};
    i2s_channel_get_info(s_i2s_rx, &rx_info);
    if (rx_info.is_enabled) {
        i2s_channel_disable(s_i2s_rx);
    }

    /* Re-enable RX channel with current config (should be 16kHz mono already) */
    i2s_channel_enable(s_i2s_rx);

    /* Re-open ADC with wake word sample rate */
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = BOARD_AUDIO_SAMPLE_RATE,  /* 16000 Hz */
    };
    esp_err_t ret = esp_codec_dev_open(s_record_dev, &fs);
    if (ret != 0) {
        ESP_LOGE(TAG, "ADC reopen failed: %d", ret);
        xSemaphoreGive(s_codec_mutex);
        return ret;
    }

    xSemaphoreGive(s_codec_mutex);

    /* Small delay to let ADC stabilize */
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "ADC reopened successfully at %d Hz", BOARD_AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

#else
/* M5Stick uses delta-sigma buzzer — no codec reconfig available */
esp_err_t board_audio_get_volume(int *volume_percent)
{
    (void)volume_percent;
    return ESP_ERR_INVALID_STATE;
}
esp_err_t board_audio_mute(bool mute)
{
    (void)mute;
    return ESP_ERR_INVALID_STATE;
}
esp_err_t board_audio_reconfig(uint32_t sample_rate, uint8_t channels)
{
    (void)sample_rate;
    (void)channels;
    return ESP_ERR_INVALID_STATE;
}
#endif

// ============================================================================
// Knob (rotary encoder + button via IO expander) — SenseCAP Watcher only
// ============================================================================
#if BOARD_HAS_KNOB

static volatile int s_knob_delta = 0;

/* Timer-based quadrature decoder — polls both pins at 5ms intervals.
 * Much more reliable than ISR during CPU-intensive operations (recording). */
static esp_timer_handle_t s_knob_timer = NULL;
static uint8_t s_knob_prev_state = 0;

/* Quadrature lookup table: maps (prev_state << 2 | cur_state) to direction.
 * Valid transitions produce +1 or -1, invalid/no-change produce 0. */
static const int8_t s_knob_lut[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0,
};

static void knob_timer_cb(void *arg)
{
    uint8_t a = gpio_get_level(BOARD_KNOB_A);
    uint8_t b = gpio_get_level(BOARD_KNOB_B);
    uint8_t cur = (a << 1) | b;
    uint8_t idx = (s_knob_prev_state << 2) | cur;
    s_knob_delta += s_knob_lut[idx];
    s_knob_prev_state = cur;
}

esp_err_t board_knob_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOARD_KNOB_A) | (1ULL << BOARD_KNOB_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,  /* no ISR — timer polls instead */
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "Knob GPIO config failed");

    /* Sample initial state */
    uint8_t a = gpio_get_level(BOARD_KNOB_A);
    uint8_t b = gpio_get_level(BOARD_KNOB_B);
    s_knob_prev_state = (a << 1) | b;

    /* 5ms periodic timer for responsive quadrature decoding */
    esp_timer_create_args_t timer_args = {
        .callback = knob_timer_cb,
        .name = "knob_poll",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_knob_timer), TAG, "Knob timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_knob_timer, 20000), TAG, "Knob timer start failed");  /* 20ms = 50Hz (was 5ms) */

    ESP_LOGI(TAG, "Knob encoder initialized — timer-based polling (A=%d, B=%d)", BOARD_KNOB_A, BOARD_KNOB_B);
    return ESP_OK;
}

bool board_knob_button_pressed(void)
{
    if (s_io_exp == NULL) return false;
    uint32_t level = 0;
    esp_io_expander_get_level(s_io_exp, 1 << BOARD_IOEXP_KNOB_BTN, &level);
    return (level & (1 << BOARD_IOEXP_KNOB_BTN)) == 0;
}

int board_knob_get_delta(void)
{
    int d = s_knob_delta;
    s_knob_delta = 0;
    return d;
}

#endif /* BOARD_HAS_KNOB */

// ============================================================================
// User buttons (Waveshare: BOOT + IO expander buttons)
// ============================================================================
#if BOARD_HAS_USER_BUTTONS

esp_err_t board_buttons_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOARD_BOOT_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "Button GPIO config failed");
    ESP_LOGI(TAG, "User buttons initialized (BOOT=%d)", BOARD_BOOT_BUTTON);
    return ESP_OK;
}

bool board_boot_button_pressed(void)
{
    return gpio_get_level(BOARD_BOOT_BUTTON) == 0;  // Active low
}

bool board_user_button_pressed(int btn_num)
{
#if defined(CONFIG_HEYCLAWY_BOARD_WAVESHARE_AUDIO)
    if (s_io_exp == NULL) return false;
    int pin;
    switch (btn_num) {
    case 1: pin = BOARD_IOEXP_BTN1; break;
    case 2: pin = BOARD_IOEXP_BTN2; break;
    case 3: pin = BOARD_IOEXP_BTN3; break;
    default: return false;
    }
    uint32_t level = 0;
    esp_io_expander_get_level(s_io_exp, 1 << pin, &level);
    return (level & (1 << pin)) == 0;  // Active low
#else
    (void)btn_num;
    return false;
#endif
}

#endif /* BOARD_HAS_USER_BUTTONS */

// ============================================================================
// SD Card
// ============================================================================

#if defined(CONFIG_HEYCLAWY_BOARD_WAVESHARE_AUDIO)
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

static sdmmc_card_t *s_sdcard = NULL;
static bool s_sdcard_mounted = false;

esp_err_t board_sdcard_init(void)
{
    if (s_sdcard_mounted) return ESP_OK;

    ESP_LOGI(TAG, "Mounting SD card (SDMMC 1-bit)...");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 10,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk = BOARD_SD_CLK;
    slot_config.cmd = BOARD_SD_CMD;
    slot_config.d0 = BOARD_SD_D0;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    /* Note: SDMMC mode doesn't use CS pin, skip board_sd_cs() call */

    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &s_sdcard);
    if (ret != ESP_OK) {
        board_sd_cs(false);
        ESP_LOGE(TAG, "SD card mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_sdcard_mounted = true;
    sdmmc_card_print_info(stdout, s_sdcard);
    ESP_LOGI(TAG, "SD card mounted at /sdcard");
    return ESP_OK;
}

esp_err_t board_sdcard_deinit(void)
{
    if (!s_sdcard_mounted) return ESP_OK;
    esp_err_t ret = esp_vfs_fat_sdcard_unmount("/sdcard", s_sdcard);
    if (ret == ESP_OK) {
        s_sdcard_mounted = false;
        s_sdcard = NULL;
        ESP_LOGI(TAG, "SD card unmounted");
    }
    return ret;
}

bool board_sdcard_is_inserted(void)
{
    return s_sdcard_mounted;
}

#elif defined(CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER)
/* SenseCAP: SD card on SPI2 (shared with camera). Requires SPI2 init first. */
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

static sdmmc_card_t *s_sdcard = NULL;
static bool s_sdcard_mounted = false;

esp_err_t board_sdcard_init(void)
{
    if (s_sdcard_mounted) return ESP_OK;

    ESP_LOGI(TAG, "Mounting SD card (SPI mode)...");

    /* Ensure SPI2 bus is initialized (shared with camera) */
    board_spi2_init();

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 10,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = BOARD_SD_CS;
    slot_config.host_id = SPI2_HOST;

    esp_err_t ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &s_sdcard);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card SPI mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_sdcard_mounted = true;
    sdmmc_card_print_info(stdout, s_sdcard);
    ESP_LOGI(TAG, "SD card mounted at /sdcard");
    return ESP_OK;
}

esp_err_t board_sdcard_deinit(void)
{
    if (!s_sdcard_mounted) return ESP_OK;
    esp_err_t ret = esp_vfs_fat_sdspi_unmount("/sdcard", s_sdcard);
    if (ret == ESP_OK) {
        s_sdcard_mounted = false;
        s_sdcard = NULL;
    }
    return ret;
}

bool board_sdcard_is_inserted(void)
{
    return s_sdcard_mounted;
}

#else
/* No SD card support for this board */
esp_err_t board_sdcard_init(void) { ESP_LOGW(TAG, "SD card not available on this board"); return ESP_OK; }
esp_err_t board_sdcard_deinit(void) { return ESP_OK; }
bool board_sdcard_is_inserted(void) { return false; }
#endif
uint16_t board_battery_get_voltage_mv(void) { return 0; }
uint8_t board_battery_get_percent(void) { return 0; }
bool board_battery_is_charging(void)
{
#if defined(CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER)
    if (s_io_exp == NULL) return false;
    uint32_t level = 0;
    esp_io_expander_get_level(s_io_exp, 1 << BOARD_IOEXP_CHRG_DET, &level);
    return (level & (1 << BOARD_IOEXP_CHRG_DET)) == 0;
#else
    return false;
#endif
}

// ============================================================================
// IO expander / SPI2 accessors
// ============================================================================

#if BOARD_HAS_IO_EXPANDER
void *board_get_io_expander(void)
{
    return (void *)s_io_exp;
}
#endif

#if BOARD_HAS_CAMERA
#include "driver/spi_master.h"
static bool s_spi2_initialized = false;

esp_err_t board_spi2_init(void)
{
    if (s_spi2_initialized) return ESP_OK;

    const spi_bus_config_t spi2_cfg = {
        .mosi_io_num = BOARD_SPI2_MOSI,
        .miso_io_num = BOARD_SPI2_MISO,
        .sclk_io_num = BOARD_SPI2_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 65536,
    };
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &spi2_cfg, SPI_DMA_CH_AUTO);
    if (ret == ESP_OK) {
        s_spi2_initialized = true;
        ESP_LOGI(TAG, "SPI2 bus initialized (SCLK=%d, MOSI=%d, MISO=%d)",
                 BOARD_SPI2_SCLK, BOARD_SPI2_MOSI, BOARD_SPI2_MISO);
    }
    gpio_config_t sd_cs_cfg = {
        .pin_bit_mask = (1ULL << BOARD_SD_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&sd_cs_cfg);
    gpio_set_level(BOARD_SD_CS, 1);

    return ret;
}
#endif /* BOARD_HAS_CAMERA */

// ============================================================================
// PA enable (Waveshare: via IO expander)
// ============================================================================
#if defined(CONFIG_HEYCLAWY_BOARD_WAVESHARE_AUDIO)
esp_err_t board_pa_enable(bool enable)
{
    if (s_io_exp == NULL) return ESP_ERR_INVALID_STATE;
    return esp_io_expander_set_level(s_io_exp, 1 << BOARD_IOEXP_PA_EN, enable ? 1 : 0);
}

esp_err_t board_sd_cs(bool enable)
{
#ifdef BOARD_IOEXP_SD_CS
    if (s_io_exp == NULL) return ESP_ERR_INVALID_STATE;
    return esp_io_expander_set_level(s_io_exp, 1 << BOARD_IOEXP_SD_CS, enable ? 1 : 0);
#else
    (void)enable;
    return ESP_OK;
#endif
}
#endif

// ============================================================================
// Full board init
// ============================================================================

esp_err_t board_init(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Board: %s (%s)", BOARD_NAME, BOARD_MCU);
    ESP_LOGI(TAG, "========================================");

    esp_err_t ret = ESP_OK;

    // I2C0 (IO expander, codecs, RTC)
    ret = board_i2c0_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C0 init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // IO Expander
    ret = board_io_expander_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "IO expander init failed: %s - continuing with limited functionality", esp_err_to_name(ret));
    }

#if defined(CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER)
    if (s_io_exp) {
        ret = board_power_on_sequence();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Power-on sequence failed: %s", esp_err_to_name(ret));
        }
    }
#elif defined(CONFIG_HEYCLAWY_BOARD_WAVESHARE_AUDIO)
    // Enable PA for speaker output
    board_pa_enable(true);
#endif

    // RGB LED
    ret = board_rgb_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "RGB init failed: %s", esp_err_to_name(ret));

#if BOARD_HAS_DISPLAY
    // Display (backlight + LCD panel)
    ret = board_display_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "Display init failed: %s", esp_err_to_name(ret));

    // LVGL
    ret = board_lvgl_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "LVGL init failed: %s", esp_err_to_name(ret));

    // Touch panel
    ret = board_touch_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "Touch init failed: %s", esp_err_to_name(ret));
#endif

    // Audio
    ret = board_audio_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "Audio init failed: %s", esp_err_to_name(ret));

#if BOARD_HAS_KNOB
    ret = board_knob_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "Knob init failed: %s", esp_err_to_name(ret));
#endif

#if BOARD_HAS_USER_BUTTONS
    ret = board_buttons_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "Buttons init failed: %s", esp_err_to_name(ret));
#endif

    // Set initial LED to blue to show we're alive
    board_rgb_set(0, 0, 32);
#if BOARD_HAS_DISPLAY
    board_display_set_brightness(100);
#endif

    ESP_LOGI(TAG, "Board initialization complete");
    return ESP_OK;
}

void board_power_off(void)
{
#if defined(CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER)
    if (s_io_exp != NULL) {
        uint32_t all_power = 0;
        for (int i = 8; i <= 15; i++) all_power |= (1 << i);
        esp_io_expander_set_level(s_io_exp, all_power, 0);
    }
#endif
    esp_restart();
}

void board_prepare_deep_sleep(void)
{
#if defined(CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER)
    if (s_io_exp != NULL) {
        /* 1. Wait for knob button release — if user is still holding
         *    the long-press button, releasing it will trigger INT. */
        ESP_LOGI(TAG, "Waiting for button release before deep sleep...");
        for (int i = 0; i < 200; i++) {  /* up to 10s */
            uint32_t level = 0;
            esp_io_expander_get_level(s_io_exp, 1 << BOARD_IOEXP_KNOB_BTN, &level);
            if (level & (1 << BOARD_IOEXP_KNOB_BTN)) break;  /* released (HIGH) */
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        vTaskDelay(pdMS_TO_TICKS(100));  /* debounce after release */

        /* 2. Power off peripherals that can generate interrupts.
         *    Matches factory firmware: LCD, AI, SD, PA, Grove, BAT_ADC off. */
        uint32_t periph_mask = (1 << BOARD_IOEXP_PWR_SDCARD)  |
                               (1 << BOARD_IOEXP_PWR_LCD)     |
                               (1 << BOARD_IOEXP_PWR_AI)      |
                               (1 << BOARD_IOEXP_PWR_PA)      |
                               (1 << BOARD_IOEXP_PWR_GROVE)   |
                               (1 << BOARD_IOEXP_PWR_BAT_ADC);
        esp_io_expander_set_level(s_io_exp, periph_mask, 0);
        ESP_LOGI(TAG, "Powered off peripherals for deep sleep");
        vTaskDelay(pdMS_TO_TICKS(200));

        /* 3. Reconfigure all input pins EXCEPT knob button (P0.3) as outputs.
         *    PCA9535 INT fires on ANY input pin change. After peripherals are
         *    powered off, floating pins (touch INT, SD det, charge det, etc.)
         *    will trigger spurious interrupts → immediate wake from deep sleep.
         *    Setting them as outputs prevents them from generating INT. */
        uint32_t non_wake_inputs = (1 << BOARD_IOEXP_CHRG_DET)  |
                                   (1 << BOARD_IOEXP_STDBY_DET) |
                                   (1 << BOARD_IOEXP_VBUS_DET)  |
                                   (1 << BOARD_IOEXP_SD_DET)    |
                                   (1 << BOARD_IOEXP_TOUCH_INT) |
                                   (1 << BOARD_IOEXP_SSCMA_SYNC);
        esp_io_expander_set_dir(s_io_exp, non_wake_inputs, IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(s_io_exp, non_wake_inputs, 0);
        ESP_LOGI(TAG, "Non-wake input pins reconfigured as outputs");

        /* 4. Read ALL input pins to clear PCA9535 INT latch. */
        uint32_t dummy = 0;
        esp_io_expander_get_level(s_io_exp, 0xFFFF, &dummy);
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_io_expander_get_level(s_io_exp, 0xFFFF, &dummy);
        ESP_LOGI(TAG, "IO expander INT cleared (inputs=0x%04lx)", dummy);
    }

    /* 5. Configure RTC pull-up on GPIO2 and verify it's HIGH. */
    rtc_gpio_pullup_en(GPIO_NUM_2);
    rtc_gpio_pulldown_dis(GPIO_NUM_2);

    /* Wait for GPIO2 to go HIGH (INT deasserted) */
    for (int i = 0; i < 100; i++) {
        if (gpio_get_level(GPIO_NUM_2) == 1) break;
        if (s_io_exp != NULL) {
            uint32_t dummy = 0;
            esp_io_expander_get_level(s_io_exp, 0xFFFF, &dummy);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    int gpio2 = gpio_get_level(GPIO_NUM_2);
    ESP_LOGW(TAG, "GPIO2 (INT) level before deep sleep: %d %s",
             gpio2, gpio2 ? "HIGH (OK)" : "LOW (will wake immediately!)");

    esp_sleep_enable_ext0_wakeup(GPIO_NUM_2, 0);  /* Wake on LOW */
#elif defined(CONFIG_HEYCLAWY_BOARD_WAVESHARE_AUDIO)
    for (int i = 0; i < 100 && gpio_get_level(GPIO_NUM_0) == 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
#endif
}

void board_reboot(void)
{
    esp_restart();
}

// ============================================================================
// ██  M5StickC Plus2 Implementation
// ============================================================================
#if defined(CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2)

/* Power: set HOLD pin HIGH to keep device powered */
static esp_err_t board_m5_power_hold(bool on)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_HOLD_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(BOARD_HOLD_PIN, on ? 1 : 0);
    return ESP_OK;
}

/* I2C init */
static esp_err_t board_i2c0_init(void)
{
    if (s_i2c0_bus) return ESP_OK;
    const i2c_master_bus_config_t cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_i2c0_bus), TAG, "I2C0 init failed");
    ESP_LOGI(TAG, "I2C0 initialized (SDA=%d, SCL=%d)", BOARD_I2C_SDA, BOARD_I2C_SCL);
    return ESP_OK;
}

/* RGB/LED: M5Stick has a single red LED on GPIO19, driven via LEDC for brightness */
static bool s_led_initialized = false;

esp_err_t board_rgb_init(void)
{
    if (s_led_initialized) return ESP_OK;

    /* Use LEDC for PWM brightness control on the red LED */
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "LED timer config failed");

    ledc_channel_config_t ch = {
        .gpio_num = BOARD_LED_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "LED channel config failed");

    s_led_initialized = true;
    ESP_LOGI(TAG, "LED initialized (GPIO%d, red only)", BOARD_LED_GPIO);
    return ESP_OK;
}

esp_err_t board_rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led_initialized) return ESP_ERR_INVALID_STATE;
    /* Map any color to red LED brightness (use max of r,g,b) */
    uint8_t brightness = r;
    if (g > brightness) brightness = g;
    if (b > brightness) brightness = b;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, brightness);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    return ESP_OK;
}

/* Animation state — simplified for single LED (breathe/blink/solid) */
static struct {
    board_rgb_mode_t mode;
    uint8_t r, g, b;
    bool running;
} s_rgb_anim = {0};

static void rgb_anim_task(void *arg)
{
    int phase = 0;
    s_rgb_anim.running = true;
    while (1) {
        board_rgb_mode_t mode = s_rgb_anim.mode;
        uint8_t r = s_rgb_anim.r, g = s_rgb_anim.g, b = s_rgb_anim.b;
        uint8_t bright = r;
        if (g > bright) bright = g;
        if (b > bright) bright = b;

        switch (mode) {
        case RGB_MODE_OFF:
            board_rgb_set(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        case RGB_MODE_SOLID:
            board_rgb_set(r, g, b);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        case RGB_MODE_BREATHE:
        case RGB_MODE_RAINBOW_SPIN:
        case RGB_MODE_AURORA:
        case RGB_MODE_OCEAN: {
            /* Smooth sine breathe */
            float t = sinf((float)phase * 3.14159f / 128.0f);
            t = t * t;  /* squared for smoother curve */
            uint8_t val = (uint8_t)(t * bright);
            board_rgb_set(val, val, val);
            phase = (phase + 1) & 0xFF;
            vTaskDelay(pdMS_TO_TICKS(20));
            break;
        }
        case RGB_MODE_BLINK:
            board_rgb_set((phase & 1) ? r : 0, (phase & 1) ? g : 0, (phase & 1) ? b : 0);
            phase++;
            vTaskDelay(pdMS_TO_TICKS(300));
            break;
        default:
            /* All other modes → breathe fallback */
            board_rgb_set(bright, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(50));
            break;
        }
    }
}

esp_err_t board_rgb_animate(board_rgb_mode_t mode, uint8_t r, uint8_t g, uint8_t b)
{
    s_rgb_anim.mode = mode;
    s_rgb_anim.r = r;
    s_rgb_anim.g = g;
    s_rgb_anim.b = b;
    return ESP_OK;
}

esp_err_t board_rgb_task_start(void)
{
    if (s_rgb_anim.running) return ESP_OK;
    xTaskCreatePinnedToCore(rgb_anim_task, "rgb", 2048, NULL, 2, NULL, 0);
    return ESP_OK;
}

/* Display: ST7789V2 via SPI */
esp_err_t board_display_init(void)
{
    if (s_lcd_panel) return ESP_OK;

    /* Backlight via LEDC */
    ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_2,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&bl_timer);

    ledc_channel_config_t bl_ch = {
        .gpio_num = BOARD_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_2,
        .timer_sel = LEDC_TIMER_2,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&bl_ch);

    /* SPI bus */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_H_RES * BOARD_LCD_V_RES * 2 / 10,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    /* Panel IO */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_LCD_DC,
        .cs_gpio_num = BOARD_LCD_CS,
        .pclk_hz = BOARD_LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits = BOARD_LCD_CMD_BITS,
        .lcd_param_bits = BOARD_LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST,
                        &io_cfg, &s_lcd_io), TAG, "Panel IO init failed");

    /* ST7789 panel */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_LCD_RST,
        .rgb_endian = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = BOARD_LCD_BPP,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_lcd_io, &panel_cfg, &s_lcd_panel),
                        TAG, "ST7789 panel init failed");

    esp_lcd_panel_reset(s_lcd_panel);
    esp_lcd_panel_init(s_lcd_panel);
    esp_lcd_panel_invert_color(s_lcd_panel, true);  /* ST7789 needs color inversion */
    esp_lcd_panel_swap_xy(s_lcd_panel, true);        /* Landscape mode */
    esp_lcd_panel_mirror(s_lcd_panel, true, false);
    /* ST7789 IC is 240×320, display is 135×240. Rotation 1 offsets: */
    esp_lcd_panel_set_gap(s_lcd_panel, 40, 53);
    esp_lcd_panel_disp_on_off(s_lcd_panel, true);

    ESP_LOGI(TAG, "LCD ST7789V2 initialized (%dx%d SPI)", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return ESP_OK;
}

esp_err_t board_display_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint32_t duty = (1023 * percent) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
    return ESP_OK;
}

/* Touch: not available on M5StickCPlus2 */
esp_err_t board_touch_init(void) { return ESP_OK; }

/* LVGL */
esp_err_t board_lvgl_init(void)
{
    if (s_lvgl_disp) return ESP_OK;
    if (!s_lcd_panel) return ESP_ERR_INVALID_STATE;

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "LVGL port init failed");

    /* LVGL display — landscape 240×135 */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_lcd_io,
        .panel_handle = s_lcd_panel,
        .buffer_size = 240 * 10 * sizeof(uint16_t),
        .double_buffer = false,
        .hres = 240,   /* landscape width */
        .vres = 135,   /* landscape height */
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
    };
    s_lvgl_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_lvgl_disp) {
        ESP_LOGE(TAG, "LVGL display add failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "LVGL initialized (landscape 240x135)");
    return ESP_OK;
}

lv_disp_t *board_get_lvgl_disp(void)
{
    return s_lvgl_disp;
}

/* Audio: PDM mic input (I2S0 RX) + LEDC PWM speaker output (GPIO2, precise timing) */
esp_err_t board_audio_init(void)
{
    s_codec_mutex = xSemaphoreCreateMutex();

    /* PDM mic on I2S0 (RX only) */
    i2s_chan_config_t chan_cfg = {
        .id = BOARD_I2S_NUM,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear = true,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_i2s_rx), TAG, "I2S RX channel failed");

    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(BOARD_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = BOARD_PDM_CLK,
            .din = BOARD_PDM_DATA,
            .invert_flags = { .clk_inv = false },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_pdm_rx_mode(s_i2s_rx, &pdm_rx_cfg), TAG, "PDM RX init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_rx), TAG, "PDM RX enable failed");
    ESP_LOGI(TAG, "PDM mic initialized (CLK=%d, DATA=%d)", BOARD_PDM_CLK, BOARD_PDM_DATA);

    /* Speaker: I2S1 standard mode TX — 1-bit delta-sigma via bitstream on GPIO2.
     * Each PCM sample → 96 bits (3×32) of delta-sigma output through I2S DMA.
     * I2S runs at 48kHz stereo 16-bit (1.536 Mbps bitstream), giving 96× oversampling
     * of 16kHz audio for ~8-bit effective resolution after noise shaping. */
    i2s_chan_config_t tx_chan_cfg = {
        .id = I2S_NUM_1,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 8,
        .dma_frame_num = 256,
        .auto_clear = true,
    };
    ESP_RETURN_ON_ERROR(i2s_new_channel(&tx_chan_cfg, &s_i2s_tx, NULL), TAG, "I2S1 TX channel failed");

    /* 48kHz → 3× the audio sample rate for higher oversampling */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_GPIO_UNUSED,
            .ws = I2S_GPIO_UNUSED,
            .dout = BOARD_BUZZER_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_tx, &std_cfg), TAG, "I2S1 STD init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_tx), TAG, "I2S1 TX enable failed");
    ESP_LOGI(TAG, "Delta-sigma speaker initialized (I2S1 STD TX, GPIO%d, 48kHz, 96x OSR)",
             BOARD_BUZZER_GPIO);

    return ESP_OK;
}

esp_err_t board_audio_record(int16_t *buffer, size_t samples, size_t *samples_read)
{
    if (!s_i2s_rx) return ESP_ERR_INVALID_STATE;
    size_t bytes_read = 0;
    xSemaphoreTake(s_codec_mutex, portMAX_DELAY);
    esp_err_t ret = i2s_channel_read(s_i2s_rx, buffer, samples * sizeof(int16_t),
                                      &bytes_read, pdMS_TO_TICKS(1000));
    xSemaphoreGive(s_codec_mutex);
    if (samples_read) *samples_read = bytes_read / sizeof(int16_t);
    return ret;
}

esp_err_t board_audio_play(const int16_t *buffer, size_t samples, size_t *samples_written)
{
    if (!s_codec_mutex || !s_i2s_tx) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_codec_mutex, portMAX_DELAY);

    /* 1-bit delta-sigma modulation with 3× oversampling (48kHz I2S for 16kHz audio).
     * Each PCM sample produces 3 I2S frames (96 DS bits) → 96× oversampling
     * → ~8-bit effective resolution with noise shaping to inaudible frequencies. */
    const size_t CHUNK = 128;        /* input samples per iteration */
    const int DS_OVERSAMPLE = 3;     /* 3 DS frames per PCM sample (16kHz → 48kHz) */
    int32_t ds_buf[CHUNK * DS_OVERSAMPLE];
    size_t total = 0;
    int32_t acc = s_ds_accum;

    while (total < samples) {
        size_t n = samples - total;
        if (n > CHUNK) n = CHUNK;
        size_t ds_idx = 0;

        for (size_t i = 0; i < n; i++) {
            /* Amplify (2×) and convert to delta-sigma drive value */
            int32_t amplified = (int32_t)buffer[total + i] * 2;
            if (amplified > 32767) amplified = 32767;
            if (amplified < -32768) amplified = -32768;
            int32_t v = -32768 - amplified;

            /* Generate 3 × 32 = 96 bits of delta-sigma output per sample */
            for (int rep = 0; rep < DS_OVERSAMPLE; rep++) {
                uint32_t bitdata = 0;
                uint32_t bit = 0x80000000u;
                do {
                    if ((acc += v) < 0) {
                        acc += 0x10000;
                        bitdata |= bit;
                    }
                } while (bit >>= 1);
                ds_buf[ds_idx++] = (int32_t)bitdata;
            }
        }

        size_t bytes_written = 0;
        i2s_channel_write(s_i2s_tx, ds_buf, ds_idx * sizeof(int32_t),
                          &bytes_written, pdMS_TO_TICKS(1000));
        total += n;
    }
    s_ds_accum = acc;

    xSemaphoreGive(s_codec_mutex);
    if (samples_written) *samples_written = total;
    return ESP_OK;
}

esp_err_t board_audio_set_volume(int volume_percent)
{
    (void)volume_percent;  /* Buzzer has no volume control */
    ESP_LOGD(TAG, "Volume set to %d%% (buzzer, no effect)", volume_percent);
    return ESP_OK;
}

void board_play_tick(void)
{
    if (!s_codec_mutex) return;
    /* Short 1kHz beep (30ms = 480 samples at 16kHz) */
    const int n = 480;
    int16_t buf[480];
    for (int i = 0; i < n; i++) {
        float phase = (float)i / 16.0f * 2.0f * 3.14159f;
        buf[i] = (int16_t)(sinf(phase) * 20000.0f);
    }
    size_t written;
    board_audio_play(buf, n, &written);
}

/* Buttons: 3 GPIO buttons (active low, directly on GPIO) */
esp_err_t board_buttons_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOARD_BTN_A) | (1ULL << BOARD_BTN_B) | (1ULL << BOARD_BTN_C),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,  /* GPIOs 34-39 are input-only, no internal pull-up */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "Button GPIO config failed");
    ESP_LOGI(TAG, "Buttons initialized (A=%d, B=%d, C=%d)", BOARD_BTN_A, BOARD_BTN_B, BOARD_BTN_C);
    return ESP_OK;
}

bool board_boot_button_pressed(void)
{
    return gpio_get_level(BOARD_BTN_A) == 0;  /* Button A = primary action */
}

bool board_user_button_pressed(int btn_num)
{
    switch (btn_num) {
    case 1: return gpio_get_level(BOARD_BTN_A) == 0;
    case 2: return gpio_get_level(BOARD_BTN_B) == 0;
    case 3: return gpio_get_level(BOARD_BTN_C) == 0;
    default: return false;
    }
}

/* SD card: not available */
esp_err_t board_sdcard_init(void) { return ESP_OK; }
esp_err_t board_sdcard_deinit(void) { return ESP_OK; }
bool board_sdcard_is_inserted(void) { return false; }

/* Battery */
uint16_t board_battery_get_voltage_mv(void) { return 3700; }
uint8_t board_battery_get_percent(void) { return 50; }
bool board_battery_is_charging(void) { return false; }

/* Board init */
esp_err_t board_init(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Board: %s (%s)", BOARD_NAME, BOARD_MCU);
    ESP_LOGI(TAG, "========================================");

    /* Keep power on */
    board_m5_power_hold(true);

    /* I2C */
    esp_err_t ret = board_i2c0_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "I2C0 init failed: %s", esp_err_to_name(ret));

    /* RGB/LED */
    ret = board_rgb_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "LED init failed: %s", esp_err_to_name(ret));

    /* Display */
    ret = board_display_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "Display init failed: %s", esp_err_to_name(ret));

    /* LVGL */
    ret = board_lvgl_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "LVGL init failed: %s", esp_err_to_name(ret));

    /* Audio (PDM mic + buzzer) */
    ret = board_audio_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "Audio init failed: %s", esp_err_to_name(ret));

    /* Buttons */
    ret = board_buttons_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "Buttons init failed: %s", esp_err_to_name(ret));

    /* Initial LED */
    board_rgb_set(0, 0, 32);
    board_display_set_brightness(100);

    ESP_LOGI(TAG, "Board initialization complete");
    return ESP_OK;
}

void board_power_off(void)
{
    board_display_set_brightness(0);
    board_rgb_set(0, 0, 0);
    board_m5_power_hold(false);  /* Release HOLD → device powers off */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();  /* Fallback if USB is connected */
}

void board_prepare_deep_sleep(void)
{
    board_display_set_brightness(0);
    board_rgb_set(0, 0, 0);
    /* Button C (GPIO35) wakes from deep sleep */
    esp_sleep_enable_ext0_wakeup(BOARD_BTN_C, 0);  /* Wake on LOW (button pressed) */
    /* Release HOLD pin — on wake, firmware must set it HIGH again */
}

void board_reboot(void)
{
    esp_restart();
}

#endif /* CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2 */
