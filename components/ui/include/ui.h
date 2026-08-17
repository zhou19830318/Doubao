/*
 * SPDX-FileCopyrightText: 2024-2026 AIWatch Contributors
 * SPDX-License-Identifier: MIT
 *
 * UI module — LVGL portrait (172×320) with iPhone 17 + Dynamic Island style
 *
 * Layout (top to bottom):
 *   Dynamic Island:  top pill with WiFi · State · Clock · OC dot  (y=4, h=30)
 *   Chinese subtitle: below island, 16px font                      (y=38)
 *   Main content:     state-specific centerpiece                   (y=60..280)
 *
 * States and their content:
 *   BOOT        → "Booting" large text
 *   CONNECTING  → "Connecting WiFi..." / "Connecting OpenClaw..."
 *   IDLE        → Big clock (36px) + Chinese date below (16px)
 *   LISTENING   → Audio waveform animation bars + "Listening" text
 *   SENDING     → "Sending..."
 *   THINKING    → "Thinking..." + elapsed time / detail
 *   STREAMING   → "Receiving..."
 *   RESPONSE    → AI response text
 *   TTS_PLAYING → "Speaking"
 *   ERROR       → Error message
 *   PLAYING_MP3 → Handled by mp3_ui overlay
 *   NOTIFYING   → Notification text
 *   SLEEP/ARMED → Minimal display
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "board.h"
// TODO(Task 8): 由 doubao 链路替换 — removed #include "openclaw_client.h" (component deleted in Task 1)
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Application-level states (aligned with HTML simulator design)
typedef enum {
    UI_STATE_SLEEP = 0,       // Deep sleep: screen off, RGB dim (dark gray #48484A)
    UI_STATE_ARMED,           // Wake word armed, waiting for trigger (indigo #5E5CE0)
    UI_STATE_BOOT,            // "Booting" large text (orange #FF9F0A)
    UI_STATE_CONNECTING,      // "Connecting..." text (blue #007AFF)
    UI_STATE_IDLE,            // Big clock + Chinese date (green #30D158)
    UI_STATE_LISTENING,       // Waveform bars + "Listening" (red #FF453A)
    UI_STATE_SENDING,         // "Sending..." text (blue #007AFF)
    UI_STATE_THINKING,        // "Thinking..." text + detail (orange #FF9500)
    UI_STATE_STREAMING,       // "Receiving..." text (purple #AF52DE)
    UI_STATE_RESPONSE,        // Response text (green #30D158)
    UI_STATE_TTS_LOADING,     // "Loading Audio..." text
    UI_STATE_TTS_PLAYING,     // "Speaking" text (green #30D158)
    UI_STATE_PLAYING_MP3,     // SD card MP3 playback (cyan #64D2FF)
    UI_STATE_NOTIFYING,       // Notification text (orange #FF9F0A)
    UI_STATE_ERROR,           // Error message (red #FF453A)
} ui_state_t;

// Initialize the main UI screen (landscape layout with Dynamic Island)
esp_err_t ui_init(void);

// Destroy the main UI screen and free all resources
void ui_destroy(void);

// Set event group for UI button callbacks (must be called before buttons work)
void ui_set_event_group(void *event_group);

// Set the main application state (updates Dynamic Island + main content)
void ui_set_state(ui_state_t state);
ui_state_t ui_get_state(void);

// Status bar updates → forwarded to Dynamic Island
void ui_set_wifi_status(bool connected, int rssi);
void ui_set_battery_status(int percent, bool charging);
void ui_set_openclaw_connected(bool connected);

// Set the response text (shown in center when in RESPONSE state)
void ui_set_response(const char *short_text, const char *full_text);

// Update thinking timer (shown in center label during THINKING)
void ui_set_thinking_time(uint32_t elapsed_ms);

// Update center label during THINKING with activity detail
void ui_set_thinking_detail(const char *detail, uint32_t elapsed_ms);

// Set LLM cost display (reserved)
void ui_set_cost(const char *cost_str);

// TODO(Task 8): 由 doubao 链路替换 — openclaw_info_t deleted in Task 1
// void ui_set_server_info(const openclaw_info_t *info);

// Status message for boot/connecting phases (shown in center label)
void ui_set_status_message(const char *msg);

// STT transcription display (yellow, lower zone)
void ui_set_stt_text(const char *text);

// TTS spoken text display (blue, lower zone)
void ui_set_tts_text(const char *text);

// ══════════════════════════════════════════════════════════════════════════
// Chat bubbles (Task 7: minimal text chat)
// ══════════════════════════════════════════════════════════════════════════

// User bubble (right-aligned, blue). While the most recent bubble is still a
// user bubble (active utterance), the text is REPLACED — used for streamed
// transcript updates; TRANSCRIPT_DONE 时整体替换。
void ui_add_user_bubble(const char *text);

// Bot bubble (left-aligned, gray, empty). Becomes the ui_bot_bubble_append
// target for the current reply.
void ui_add_bot_bubble(void);

// Append a streamed reply chunk to the current bot bubble (auto-creates one
// if needed). Task 7 uses full-text reset; streaming optimization in Task 13.
void ui_bot_bubble_append(const char *delta);

// Strip non-ASCII characters from text for display
void ui_sanitize_text(char *dst, const char *src, size_t dst_size);

// Get the stored full response text (for TTS playback)
const char *ui_get_full_response(void);

// Get the main screen LVGL object
lv_obj_t *ui_get_main_screen(void);

// Screen transition with animation
void ui_screen_load_anim(lv_obj_t *scr, lv_screen_load_anim_t anim, uint32_t time, uint32_t delay);

// Web server status indicator (reserved)
void ui_set_webserver_status(bool running);

// Task/progress info (reserved)
void ui_set_task_info(const char *task_text);
// TODO(Task 8): 由 doubao 链路替换 — openclaw_info_t deleted in Task 1
// void ui_set_task_info_detailed(const openclaw_info_t *info);

// External activity indicator (reserved)
void ui_set_external_activity(bool active, const char *detail, uint32_t elapsed_ms);


// ══════════════════════════════════════════════════════════════════════════
// RGB LED Ring Control (7× WS2812) — state-based colors
// ══════════════════════════════════════════════════════════════════════════

// Update LED ring color based on current UI state
void ui_update_led_for_state(ui_state_t state);

// Set all LEDs to a specific color (r, g, b: 0-255)
void ui_set_led_color(uint8_t r, uint8_t g, uint8_t b);

// Breathing animation for idle state
void ui_start_led_breathing(uint8_t r, uint8_t g, uint8_t b);
void ui_stop_led_breathing(void);

// Stop the LED animation task entirely (call on UI shutdown)
void ui_led_anim_stop(void);

// ══════════════════════════════════════════════════════════════════════════
// System Info & Activity Tracking
// ══════════════════════════════════════════════════════════════════════════

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
