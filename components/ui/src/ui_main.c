/*
 * SPDX-FileCopyrightText: 2024-2026 AIClaw Contributors
 * SPDX-License-Identifier: MIT
 *
 * Main UI module - State machine and screen management
 * Refactored to use modular components (chat, status bar, state chip, MP3)
 */

#include "ui.h"
#include "ui_status_bar.h"
#include "ui_state_chip.h"
#include "ui_mp3_ui.h"
#include "ui_state_gif.h"
#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_lvgl_port.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "ui_main";

/* Screen and state objects */
static lv_obj_t *s_screen = NULL;
static ui_state_t s_state = UI_STATE_BOOT;
static EventGroupHandle_t s_events = NULL;

/* Response storage */
static char s_full_response[4096];

/* Clock update timer */
static esp_timer_handle_t s_clock_timer = NULL;

/* RGB LED control (kept from original) */
extern void ui_update_led_for_state(ui_state_t state);

/* State labels and Chinese descriptions (aligned with HTML simulator) */
static const char *state_labels[] = {
    [UI_STATE_SLEEP]       = "SLEEP",
    [UI_STATE_ARMED]       = "ARMED",
    [UI_STATE_BOOT]        = "BOOT",
    [UI_STATE_CONNECTING]  = "CONNECTING",
    [UI_STATE_IDLE]        = "IDLE",
    [UI_STATE_LISTENING]   = "LISTENING",
    [UI_STATE_SENDING]     = "SENDING",
    [UI_STATE_THINKING]    = "THINKING",
    [UI_STATE_STREAMING]   = "STREAMING",
    [UI_STATE_RESPONSE]    = "RESPONSE",
    [UI_STATE_TTS_LOADING] = "TTS_LOAD",
    [UI_STATE_TTS_PLAYING] = "SPEAKING",
    [UI_STATE_PLAYING_MP3] = "MP3",
    [UI_STATE_NOTIFYING]   = "NOTIFY",
    [UI_STATE_ERROR]       = "ERROR",
};

static const char *state_chinese[] = {
    [UI_STATE_SLEEP]       = "深度睡眠",
    [UI_STATE_ARMED]       = "布防中",
    [UI_STATE_BOOT]        = "系统启动",
    [UI_STATE_CONNECTING]  = "连接中...",
    [UI_STATE_IDLE]        = "待机",
    [UI_STATE_LISTENING]   = "录音中",
    [UI_STATE_SENDING]     = "发送中",
    [UI_STATE_THINKING]    = "AI思考中",
    [UI_STATE_STREAMING]   = "接收中",
    [UI_STATE_RESPONSE]    = "AI回复",
    [UI_STATE_TTS_LOADING] = "加载语音",
    [UI_STATE_TTS_PLAYING] = "语音播放",
    [UI_STATE_PLAYING_MP3] = "音乐播放",
    [UI_STATE_NOTIFYING]   = "提醒",
    [UI_STATE_ERROR]       = "错误",
};

/* State colors (aligned with HTML simulator) */
static lv_color_t get_state_color(ui_state_t state)
{
    switch (state) {
    case UI_STATE_SLEEP:       return lv_color_hex(0x48484a);
    case UI_STATE_ARMED:       return lv_color_hex(0x5e5ce0);
    case UI_STATE_BOOT:        return lv_color_hex(0xff9f0a);
    case UI_STATE_CONNECTING:  return lv_color_hex(0x0a84ff);
    case UI_STATE_IDLE:        return lv_color_hex(0x30d158);
    case UI_STATE_LISTENING:   return lv_color_hex(0xff453a);
    case UI_STATE_SENDING:     return lv_color_hex(0x0a84ff);
    case UI_STATE_THINKING:    return lv_color_hex(0xff9f0a);
    case UI_STATE_STREAMING:   return lv_color_hex(0xbf5af2);
    case UI_STATE_RESPONSE:    return lv_color_hex(0x30d158);
    case UI_STATE_TTS_LOADING: return lv_color_hex(0x30d158);
    case UI_STATE_TTS_PLAYING: return lv_color_hex(0x30d158);
    case UI_STATE_PLAYING_MP3: return lv_color_hex(0x64d2ff);
    case UI_STATE_NOTIFYING:   return lv_color_hex(0xff9f0a);
    case UI_STATE_ERROR:       return lv_color_hex(0xff453a);
    default:                   return lv_color_hex(0x808080);
    }
}

/* Forward declarations */
static void update_state_display(ui_state_t state);
static void handle_state_transition(ui_state_t old_state, ui_state_t new_state);
static esp_err_t start_clock_timer(void);
static void stop_clock_timer(void);

/* ══════════════════════════════════════════════════════════════════════
 * UI Initialization
 * ══════════════════════════════════════════════════════════════════════ */

esp_err_t ui_init(void)
{
    ESP_LOGI(TAG, "Initializing UI...");

    /* Set timezone to China Standard Time (UTC+8) */
    setenv("TZ", "CST-8", 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to CST-8 (China Standard Time, UTC+8)");

    /* Create main screen */
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x0e0e10), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    /* Load screen first so GIF objects are created on the correct screen */
    lv_scr_load(s_screen);

    /* Initialize sub-modules */
    ui_status_bar_init(s_screen);
    /* DISABLED: Chat module to save memory - causes crash on LISTENING */
    /* ui_chat_init(s_screen); */
    ui_state_chip_init(s_screen);
    ui_mp3_ui_init(s_screen);

    /* Initialize RGB LED */
    /* RGB LED is initialized in board init, just update for current state */

    /* Set initial state (GIF now uses correct active screen via lv_scr_act()) */
    ui_set_state(UI_STATE_BOOT);

    /* Start clock update timer */
    start_clock_timer();

    ESP_LOGI(TAG, "UI initialized successfully");
    return ESP_OK;
}

void ui_destroy(void)
{
    /* Stop clock update timer */
    stop_clock_timer();
    
    if (s_screen) {
        lv_obj_del(s_screen);
        s_screen = NULL;
    }
    /* RGB LED cleanup handled by board module */
}

/* ══════════════════════════════════════════════════════════════════════
 * State Management
 * ══════════════════════════════════════════════════════════════════════ */

void ui_set_state(ui_state_t state)
{
    ui_state_t old_state = s_state;
    s_state = state;

    ESP_LOGI(TAG, "State transition: %s → %s", 
             state_labels[old_state], state_labels[state]);

    /* Update RGB LED for new state */
    ui_update_led_for_state(state);

    /* Update GIF animation for new state - this also cleans up previous GIF */
    ui_state_gif_show_for_state(state);

    /* Handle state-specific transitions */
    handle_state_transition(old_state, state);

    /* Update display */
    update_state_display(state);

    /* Update state chip */
    if (state <= UI_STATE_ERROR) {
        ui_state_chip_update(state_labels[state], state_chinese[state], get_state_color(state));
    }
}

ui_state_t ui_get_state(void)
{
    return s_state;
}

/* ══════════════════════════════════════════════════════════════════════
 * State Transition Handler
 * ══════════════════════════════════════════════════════════════════════ */

static void handle_state_transition(ui_state_t old_state, ui_state_t new_state)
{
    /* Hide MP3 UI when leaving MP3 state */
    if (old_state == UI_STATE_PLAYING_MP3 && new_state != UI_STATE_PLAYING_MP3) {
        ui_mp3_ui_hide();
    }

    /* Show MP3 UI when entering MP3 state */
    if (new_state == UI_STATE_PLAYING_MP3) {
        /* MP3 UI will be shown by ui_mp3_player callbacks */
    }

    /* Clear chat when entering certain states */
    if (new_state == UI_STATE_IDLE || new_state == UI_STATE_ARMED) {
        /* Keep chat history in IDLE/ARMED */
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * State Display Update
 * ══════════════════════════════════════════════════════════════════════ */

static void update_state_display(ui_state_t state)
{
    /* Note: LVGL operations should be called from LVGL task context */
    /* Remove lvgl_port_lock/unlock as they may cause issues */
    
    switch (state) {
    case UI_STATE_BOOT:
        /* No message - save memory */
        break;

    case UI_STATE_CONNECTING:
        /* No message - save memory */
        break;

    case UI_STATE_IDLE:
        /* No message - save memory, chat will be cleared on LISTENING */
        break;

    case UI_STATE_LISTENING:
        /* Clear chat before recording to free memory - DISABLED */
        /* ESP_LOGI(TAG, "Before LISTENING: Free heap=%d", esp_get_free_internal_heap_size()); */
        /* ui_chat_clear(); */
        /* ESP_LOGI(TAG, "After clear: Free heap=%d", esp_get_free_internal_heap_size()); */
        break;

    case UI_STATE_SENDING:
        /* Sending indicator - no chat message */
        break;

    case UI_STATE_THINKING:
        /* Thinking indicator - no chat message */
        /* TODO: Start rotation animation if needed */
        break;

    case UI_STATE_STREAMING:
        /* Receiving indicator - no chat message */
        break;

    case UI_STATE_RESPONSE:
        /* Response will be added via ui_set_response() */
        break;

    case UI_STATE_TTS_LOADING:
        /* TTS loading - no chat message */
        break;

    case UI_STATE_TTS_PLAYING:
        /* TTS playing - no chat message */
        break;

    case UI_STATE_PLAYING_MP3:
        /* MP3 UI handled separately */
        break;

    case UI_STATE_NOTIFYING:
        /* Notification - no chat message */
        break;

    case UI_STATE_ERROR:
        /* Error - no chat message */
        break;

    default:
        break;
    }

    /* No unlock needed - removed lock above */
}

/* ══════════════════════════════════════════════════════════════════════
 * API Implementations
 * ══════════════════════════════════════════════════════════════════════ */

void ui_set_event_group(void *event_group)
{
    s_events = (EventGroupHandle_t)event_group;
}

void ui_set_wifi_status(bool connected, int rssi)
{
    ui_status_bar_set_wifi(connected, rssi);
}

void ui_set_battery_status(int percent, bool charging)
{
    /* TODO: Add battery icon to status bar if needed */
    (void)percent;
    (void)charging;
}

void ui_set_openclaw_connected(bool connected)
{
    ui_status_bar_set_oc_connected(connected);
}

void ui_set_response(const char *short_text, const char *full_text)
{
    if (!short_text) return;

    /* Store full response for TTS */
    if (full_text) {
        strncpy(s_full_response, full_text, sizeof(s_full_response) - 1);
        s_full_response[sizeof(s_full_response) - 1] = '\0';
    }

    /* Add AI message to chat - DISABLED */
    /* ui_chat_add_ai_message(short_text); */

    ESP_LOGI(TAG, "Response: %.100s", short_text);
}

const char *ui_get_full_response(void)
{
    return s_full_response;
}

void ui_set_thinking_time(uint32_t elapsed_ms)
{
    /* TODO: Update thinking timer display if needed */
    (void)elapsed_ms;
}

void ui_set_thinking_detail(const char *detail, uint32_t elapsed_ms)
{
    /* TODO: Show detailed thinking info */
    (void)detail;
    (void)elapsed_ms;
}

void ui_set_cost(const char *cost_str)
{
    /* TODO: Show cost in status bar if needed */
    (void)cost_str;
}

void ui_set_server_info(const openclaw_info_t *info)
{
    /* Not used in new UI design */
    (void)info;
}

void ui_set_status_message(const char *msg)
{
    /* DISABLED: Chat module not initialized */
    /* if (msg) { */
    /*     ui_chat_add_system_message(msg); */
    /* } */
    (void)msg;
}

void ui_sanitize_text(char *dst, const char *src, size_t dst_size)
{
    if (!dst || !src) return;
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

lv_obj_t *ui_get_main_screen(void)
{
    return s_screen;
}

void ui_set_webserver_status(bool running)
{
    /* TODO: Show webserver status indicator if needed */
    (void)running;
}

void ui_set_task_info(const char *task_text)
{
    /* TODO: Show task info in chat or separate area */
    (void)task_text;
}

void ui_set_task_info_detailed(const openclaw_info_t *info)
{
    /* TODO: Show detailed task info */
    (void)info;
}

void ui_set_external_activity(bool active, const char *detail, uint32_t elapsed_ms)
{
    /* TODO: Show external activity overlay */
    (void)active;
    (void)detail;
    (void)elapsed_ms;
}

void ui_increment_chat_count(void)
{
    /* Chat count not displayed in new UI */
}

uint32_t ui_get_chat_count(void)
{
    return 0;
}

void ui_reset_chat_count(void)
{
    /* No-op */
}

void ui_widget_show_json(const char *json)
{
    /* Widget display disabled */
    ESP_LOGI(TAG, "Widget command received but display disabled");
}

void ui_widget_clear(void)
{
    /* No-op */
}

/* Screen transition (kept for compatibility) */
void ui_screen_load_anim(lv_obj_t *scr, lv_scr_load_anim_t anim, uint32_t time, uint32_t delay)
{
    lv_scr_load_anim(scr, anim, time, delay, false);
}

/* ── Clock update timer ─────────────────────────────────────────────── */

static void clock_update_timer_cb(void *arg)
{
    (void)arg;
    
    /* Get current time */
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    /* Update status bar clock */
    if (lvgl_port_lock(100)) {
        ui_status_bar_set_clock(timeinfo.tm_hour, timeinfo.tm_min);
        lvgl_port_unlock();
    }
}

static esp_err_t start_clock_timer(void)
{
    if (s_clock_timer) {
        return ESP_OK;  /* Already started */
    }
    
    const esp_timer_create_args_t timer_args = {
        .callback = clock_update_timer_cb,
        .name = "clock_update"
    };
    
    esp_err_t err = esp_timer_create(&timer_args, &s_clock_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create clock timer: %s", esp_err_to_name(err));
        return err;
    }
    
    /* Start timer with 1 second period (update every second) */
    err = esp_timer_start_periodic(s_clock_timer, 1000000);  /* 1 second in microseconds */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start clock timer: %s", esp_err_to_name(err));
        esp_timer_delete(s_clock_timer);
        s_clock_timer = NULL;
        return err;
    }
    
    ESP_LOGI(TAG, "Clock update timer started");
    return ESP_OK;
}

static void stop_clock_timer(void)
{
    if (s_clock_timer) {
        esp_timer_stop(s_clock_timer);
        esp_timer_delete(s_clock_timer);
        s_clock_timer = NULL;
        ESP_LOGI(TAG, "Clock update timer stopped");
    }
}
