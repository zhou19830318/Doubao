/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * RGB LED Ring Control Module — 7× WS2812 LEDs with state-based colors
 * Aligned with HTML simulator design
 */

#include "ui.h"
#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <string.h>

#if BOARD_HAS_RGB_RING

static const char *TAG = "rgb_led";

// State-based color palette (from HTML simulator — per-LED gradients)
// Each state has 7 colors (one per LED in the ring)
typedef struct {
    uint8_t r[7], g[7], b[7];
} rgb_state_colors_t;

static const rgb_state_colors_t STATE_COLORS[] = {
    [UI_STATE_SLEEP]       = {{0x1A,0x1A,0x1A,0x1A,0x1A,0x1A,0x1A},
                               {0x1A,0x1A,0x1A,0x1A,0x1A,0x1A,0x1A},
                               {0x1C,0x1C,0x1C,0x1C,0x1C,0x1C,0x1C}},
    [UI_STATE_ARMED]       = {{0x2A,0x2A,0x2A,0x2A,0x2A,0x2A,0x2A},
                               {0x2A,0x2A,0x2A,0x2A,0x2A,0x2A,0x2A},
                               {0x5C,0x5C,0x5C,0x5C,0x5C,0x5C,0x5C}},
    [UI_STATE_BOOT]        = {{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
                               {0x9F,0x6B,0x3B,0x6B,0x9F,0x6B,0x3B},
                               {0x0A,0x00,0x00,0x00,0x0A,0x00,0x00}},
    [UI_STATE_CONNECTING]  = {{0x00,0x00,0x00,0x00,0x00,0x00,0x00},
                               {0x7A,0x66,0x44,0x66,0x7A,0x66,0x44},
                               {0xFF,0xCC,0x99,0xCC,0xFF,0xCC,0x99}},
    [UI_STATE_IDLE]        = {{0x00,0x00,0x00,0x00,0x00,0x00,0x00},
                               {0x33,0x44,0x55,0x44,0x33,0x44,0x55},
                               {0x10,0x18,0x20,0x18,0x10,0x18,0x20}},
    [UI_STATE_LISTENING]   = {{0xFF,0xCC,0x99,0xCC,0xFF,0xCC,0x99},
                               {0x45,0x00,0x00,0x00,0x45,0x00,0x00},
                               {0x3A,0x00,0x00,0x00,0x3A,0x00,0x00}},
    [UI_STATE_SENDING]     = {{0x00,0x00,0x00,0x00,0x00,0x00,0x00},
                               {0x7A,0x55,0x33,0x55,0x7A,0x55,0x33},
                               {0xFF,0xCC,0x88,0xCC,0xFF,0xCC,0x88}},
    [UI_STATE_THINKING]    = {{0xFF,0xCC,0x99,0xCC,0xFF,0xCC,0x99},
                               {0x95,0x77,0x55,0x77,0x95,0x77,0x55},
                               {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    [UI_STATE_STREAMING]   = {{0xBF,0x99,0x77,0x99,0xBF,0x99,0x77},
                               {0x52,0x33,0x22,0x33,0x52,0x33,0x22},
                               {0xDE,0xCC,0xAA,0xCC,0xDE,0xCC,0xAA}},
    [UI_STATE_RESPONSE]    = {{0x00,0x00,0x00,0x00,0x00,0x00,0x00},
                               {0x40,0x50,0x60,0x50,0x40,0x50,0x60},
                               {0x18,0x20,0x28,0x20,0x18,0x20,0x28}},
    [UI_STATE_TTS_LOADING] = {{0x00,0x00,0x00,0x00,0x00,0x00,0x00},
                               {0x40,0x50,0x60,0x50,0x40,0x50,0x60},
                               {0x20,0x28,0x30,0x28,0x20,0x28,0x30}},
    [UI_STATE_TTS_PLAYING] = {{0x30,0x20,0x10,0x20,0x30,0x20,0x10},
                               {0xD1,0xA0,0x70,0xA0,0xD1,0xA0,0x70},
                               {0x58,0x40,0x28,0x40,0x58,0x40,0x28}},
    [UI_STATE_PLAYING_MP3] = {{0xFF,0xFF,0xFF,0x30,0x00,0x5E,0xBF},
                               {0x45,0x9F,0xEB,0xD1,0x7A,0x5C,0x5A},
                               {0x3A,0x0A,0x3B,0x58,0xFF,0xE0,0xF2}},
    [UI_STATE_NOTIFYING]   = {{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
                               {0x9F,0x6B,0x9F,0x6B,0x9F,0x6B,0x9F},
                               {0x0A,0x00,0x0A,0x00,0x0A,0x00,0x0A}},
    [UI_STATE_ERROR]       = {{0xFF,0xFF,0xCC,0xFF,0xFF,0xFF,0xCC},
                               {0x45,0x00,0x00,0x00,0x45,0x00,0x00},
                               {0x3A,0x00,0x00,0x00,0x3A,0x00,0x00}},
};

static TaskHandle_t s_breathing_task = NULL;
static volatile bool s_breathing_stop = false;

// Breathing animation task
static void breathing_task(void *arg)
{
    uint8_t base_r = ((uint8_t*)arg)[0];
    uint8_t base_g = ((uint8_t*)arg)[1];
    uint8_t base_b = ((uint8_t*)arg)[2];
    
    float brightness = 0.0f;
    float direction = 0.02f;
    
    while (!s_breathing_stop) {
        brightness += direction;
        if (brightness >= 1.0f) {
            brightness = 1.0f;
            direction = -0.02f;
        } else if (brightness <= 0.2f) {
            brightness = 0.2f;
            direction = 0.02f;
        }
        
        uint8_t r = (uint8_t)(base_r * brightness);
        uint8_t g = (uint8_t)(base_g * brightness);
        uint8_t b = (uint8_t)(base_b * brightness);
        
        board_rgb_set(r, g, b);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    
    s_breathing_task = NULL;
    vTaskDelete(NULL);
}

void ui_update_led_for_state(ui_state_t state)
{
    if (state > UI_STATE_ERROR) {
        ESP_LOGW(TAG, "Invalid state: %d", state);
        return;
    }
    
    // Stop breathing if running
    ui_stop_led_breathing();
    
    // Set each LED individually from the per-LED palette
    for (int i = 0; i < 7 && i < BOARD_RGB_LED_COUNT; i++) {
        board_rgb_set_single(i,
            STATE_COLORS[state].r[i], STATE_COLORS[state].g[i], STATE_COLORS[state].b[i]);
    }
    board_rgb_refresh();
    
    ESP_LOGD(TAG, "LED state=%d per-LED palette applied", state);
}

void ui_set_led_color(uint8_t r, uint8_t g, uint8_t b)
{
    ui_stop_led_breathing();
    board_rgb_set(r, g, b);
}

void ui_start_led_breathing(uint8_t r, uint8_t g, uint8_t b)
{
    ui_stop_led_breathing();
    
    static uint8_t color_params[3];
    color_params[0] = r;
    color_params[1] = g;
    color_params[2] = b;
    
    s_breathing_stop = false;
    xTaskCreate(breathing_task, "led_breathe", 2048, color_params, 5, &s_breathing_task);
}

void ui_stop_led_breathing(void)
{
    if (s_breathing_task) {
        s_breathing_stop = true;
        vTaskDelay(pdMS_TO_TICKS(50)); // Wait for task to exit
    }
}

#else /* !BOARD_HAS_RGB_RING */

// Stub implementations for boards without RGB ring
void ui_update_led_for_state(ui_state_t state) { (void)state; }
void ui_set_led_color(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
void ui_start_led_breathing(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
void ui_stop_led_breathing(void) {}

#endif /* BOARD_HAS_RGB_RING */
