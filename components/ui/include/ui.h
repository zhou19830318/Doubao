/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * UI module — LVGL main screen for 412x412 round display
 *
 * Layout zones (top to bottom, respecting circular shape):
 *   Status bar: thin bar with WiFi, OC dot, cost  (y≈35, narrow)
 *   Center:     large status word or response      (y≈100-300, wide)
 *   Bottom:     interaction hint                   (y≈360, narrow)
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "board.h"
#include "openclaw_client.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Application-level states (aligned with HTML simulator design)
typedef enum {
    UI_STATE_SLEEP = 0,       // Deep sleep: screen off, RGB dim (dark gray #48484A)
    UI_STATE_ARMED,           // Wake word armed, waiting for trigger (indigo #5E5CE0)
    UI_STATE_BOOT,            // AI Logo + "启动中" (orange #FF9F0A)
    UI_STATE_CONNECTING,      // Colorful conic-gradient spinner (blue #007AFF)
    UI_STATE_IDLE,            // Big clock + date + liquid sphere (green #30D158)
    UI_STATE_LISTENING,       // "我在听" + colorful waveform bars (red #FF453A)
    UI_STATE_SENDING,         // ChatGPT 3-dot animation (blue #007AFF)
    UI_STATE_THINKING,        // AI square + 4-direction pulse (orange #FF9500)
    UI_STATE_STREAMING,       // 5 purple wave bars (purple #AF52DE)
    UI_STATE_RESPONSE,        // Chat bubble + typewriter effect (green #30D158)
    UI_STATE_TTS_LOADING,     // Fetching TTS audio
    UI_STATE_TTS_PLAYING,     // 🔊 + 10 audio wave bars (green #30D158)
    UI_STATE_PLAYING_MP3,     // SD card MP3 playback (cyan #64D2FF)
    UI_STATE_NOTIFYING,       // ⏰ big icon + shake animation (orange #FF9F0A)
    UI_STATE_ERROR,           // ❌ error message (red #FF453A)
} ui_state_t;

// Initialize the main UI screen
esp_err_t ui_init(void);

// Destroy the main UI screen and free all resources
void ui_destroy(void);

// Set event group for UI button callbacks (must be called before buttons work)
void ui_set_event_group(void *event_group);

// Set the main application state (updates entire screen)
void ui_set_state(ui_state_t state);
ui_state_t ui_get_state(void);

// Status bar updates
void ui_set_wifi_status(bool connected, int rssi);
void ui_set_battery_status(int percent, bool charging);
void ui_set_openclaw_connected(bool connected);

// Set the short response text (displayed big in center)
void ui_set_response(const char *short_text, const char *full_text);

// Update thinking timer (called periodically while THINKING)
void ui_set_thinking_time(uint32_t elapsed_ms);

/* Update big label during THINKING with activity detail (tool name, etc) */
void ui_set_thinking_detail(const char *detail, uint32_t elapsed_ms);

// Set LLM cost display in status bar (e.g., "$0.42")
void ui_set_cost(const char *cost_str);

// Update server info display (from openclaw_info_t snapshot)
void ui_set_server_info(const openclaw_info_t *info);

// Status message for boot/connecting phases
void ui_set_status_message(const char *msg);

// Strip non-ASCII (emoji) characters from text for display
void ui_sanitize_text(char *dst, const char *src, size_t dst_size);

// Get the stored full response text (for TTS playback)
const char *ui_get_full_response(void);

// Get the main screen LVGL object (for tasks screen navigation)
lv_obj_t *ui_get_main_screen(void);

// Screen transition with animation (SquareLine pattern: _ui_screen_change)
void ui_screen_load_anim(lv_obj_t *scr, lv_scr_load_anim_t anim, uint32_t time, uint32_t delay);

// Web server status indicator in status bar
void ui_set_webserver_status(bool running);

// Set task/progress info in upper area (between status bar and center)
void ui_set_task_info(const char *task_text);

// Set detailed task info from structured cron data
void ui_set_task_info_detailed(const openclaw_info_t *info);

// Show/hide external activity indicator with detail and elapsed timer
void ui_set_external_activity(bool active, const char *detail, uint32_t elapsed_ms);


// ══════════════════════════════════════════════════════════════════════
// RGB LED Ring Control (7× WS2812) — state-based colors
// ══════════════════════════════════════════════════════════════════════

// Update LED ring color based on current UI state
void ui_update_led_for_state(ui_state_t state);

// Set all LEDs to a specific color (r, g, b: 0-255)
void ui_set_led_color(uint8_t r, uint8_t g, uint8_t b);

// Breathing animation for idle state
void ui_start_led_breathing(uint8_t r, uint8_t g, uint8_t b);
void ui_stop_led_breathing(void);

// ══════════════════════════════════════════════════════════════════════
// System Info & Activity Tracking
// ══════════════════════════════════════════════════════════════════════

// Get conversation count
uint32_t ui_get_chat_count(void);
void ui_increment_chat_count(void);

// Reset activity timer (called on user interaction)
void ui_reset_activity_timer(void);
uint32_t ui_get_activity_seconds(void);

// Get uptime in seconds
uint32_t ui_get_uptime_seconds(void);

#ifdef __cplusplus
}
#endif
