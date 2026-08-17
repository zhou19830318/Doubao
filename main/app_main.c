/*
 * SPDX-FileCopyrightText: 2024-2026 AIWatch Contributors
 * SPDX-License-Identifier: MIT
 *
 * AIWatch — OpenClaw ESP32 Interface Device
 *
 * This file handles initialization and main event loop only.
 * Logic is split into: voice_chat.c, serial_cmd.c, app_tasks.c, app_state.c
 */

#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"

#include "board.h"
#include "app_config.h"
#include "app_state.h"
#include "esp_lvgl_port.h"
// TODO(Task 10): 由 doubao 链路替换 — removed #include "voice_chat.h" (voice_chat.c deleted in Task 1)
#include "serial_cmd.h"
#include "app_tasks.h"
#include "settings.h"
#include "error_log.h"
/* dev 便利值：secrets.h 是 gitignored 磁盘文件，fresh checkout 可能缺失，
 * 必须用编译守卫；缺失时回落空串。 */
#if __has_include("secrets.h")
#include "secrets.h"
#ifdef SECRETS_WIFI_SSID
#define AIDB_DEV_WIFI_SSID SECRETS_WIFI_SSID
#else
#define AIDB_DEV_WIFI_SSID ""
#endif
#ifdef SECRETS_WIFI_PASSWORD
#define AIDB_DEV_WIFI_PASSWORD SECRETS_WIFI_PASSWORD
#else
#define AIDB_DEV_WIFI_PASSWORD ""
#endif
#ifdef SECRETS_DOUBAO_API_KEY
#define AIDB_DEV_API_KEY SECRETS_DOUBAO_API_KEY
#else
#define AIDB_DEV_API_KEY ""
#endif
#else
#define AIDB_DEV_WIFI_SSID ""
#define AIDB_DEV_WIFI_PASSWORD ""
#define AIDB_DEV_API_KEY ""
#endif
#include "ui_provisioning.h"

#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

#include "wifi_manager.h"
// TODO(Task 7/8): 由 doubao 链路替换 — removed openclaw_client.h/tts_client.h/stt_client.h (components deleted in Task 1)
#include "notes_manager.h"
#include "ui.h"
#include "ui_tasks.h"
#include "ui_mp3_ui.h"
#include "mp3_player.h"
#include "webserver.h"
#include "wake_word.h"
#include "app_state_machine.h"
#include "doubao_voice.h"

static const char *TAG = "aiwatch";

/* ─── WiFi callback ──────────────────────────────────────────────────── */
/* ─── MP3 player completion callback ──────────────────────────────────── */
static void on_mp3_complete(void)
{
    ESP_LOGI(TAG, "MP3 playback finished");

    /* CRITICAL: Reset continuous conversation mode after MP3 playback. */
    extern bool g_continue_listening;
    if (g_continue_listening) {
        ESP_LOGI(TAG, "Resetting continuous conversation mode after MP3");
        g_continue_listening = false;
    }

    /* Reset activity timer so sleep countdown starts fresh after playback */
    app_reset_activity_timer();

    /* ══════════════════════════════════════════════════════════════════════
     * IMPORTANT: This callback runs in the audio pipeline task context,
     * NOT the LVGL task.  Direct LVGL calls (ui_mp3_ui_hide, app_set_state,
     * etc.) without lvgl_port_lock would be deferred or lost, causing the
     * MP3 overlay to remain on screen after playback ends.
     *
     * Queue a deferred "stop" command instead.  app_process_mp3_cmd()
     * runs in the main task (= LVGL context) and already handles:
     *   mp3_player_stop() → ui_mp3_ui_hide() → app_set_state(IDLE)
     * ══════════════════════════════════════════════════════════════════════ */
    app_queue_mp3_cmd("stop");
}

static void on_wifi_state(wifi_state_t state)
{
    /* Safety check: ensure event group is initialized */
    if (!g_app_events) {
        ESP_LOGW(TAG, "Event group not ready, ignoring WiFi state change: %d", state);
        return;
    }
    
    switch (state) {
    case WIFI_STATE_CONNECTED:
        ESP_LOGI(TAG, "WiFi connected");
        xEventGroupSetBits(g_app_events, WIFI_CONNECTED_BIT);
        lvgl_port_lock(0);
        ui_set_wifi_status(true, wifi_manager_get_rssi());
        lvgl_port_unlock();
        break;
    case WIFI_STATE_CONNECTING:
        lvgl_port_lock(0);
        ui_set_wifi_status(false, 0);
        app_state_request(UI_STATE_CONNECTING);
        ui_set_status_message("Connecting WiFi...");
        lvgl_port_unlock();
        break;
    case WIFI_STATE_FAILED:
        /* Non-UI operations first (avoid holding LVGL lock during vTaskDelay
         * inside wifi_manager_start_ap) */
        error_log_add(ERR_SRC_WIFI, ERR_SEV_ERROR, "WiFi connection failed");
        ESP_LOGI(TAG, "WiFi failed, starting AP for config...");
        wifi_manager_start_ap(NULL, NULL);

        // Immediately start web server for captive portal
        xEventGroupSetBits(g_app_events, WEBSERVER_TOGGLE_BIT);

        /* UI operations — LVGL lock required (recursive mutex, safe in callback) */
        lvgl_port_lock(0);
        ui_set_wifi_status(false, 0);
        app_set_state(UI_STATE_ERROR);
        ui_set_status_message("WiFi failed!\nStarting AP...");
#if BOARD_HAS_DISPLAY
        ui_show_provisioning_qr(NULL);  // Will show http://192.168.4.1
#endif
        lvgl_port_unlock();
        break;
    default:
        lvgl_port_lock(0);
        ui_set_wifi_status(false, 0);
        lvgl_port_unlock();
        break;
    }
}

/* ─── OpenClaw state callback ────────────────────────────────────────── */

/* Notification callback: incoming message not initiated by this device */
// TODO(Task 8): 由 doubao 链路替换 — openclaw_client deleted in Task 1; parse_device_commands kept
// /* ── External chat [DEVICE:xxx] command parser ─────────────────────────
//  * Called by openclaw_client when an external (non-device-initiated) chat
//  * final response arrives — e.g. from Feishu, WhatsApp, WebUI.
//  * Accumulates ALL content items so device commands are found regardless
//  * of which content block the AI placed them in. */
// static void on_external_device_cmd(const char *text)
// {
//     if (!text || !text[0]) return;
//     char *copy = strdup(text);
//     if (!copy) return;
//     int n = parse_device_commands(copy);
//     if (n > 0) {
//         ESP_LOGI(TAG, "External device cmd: parsed %d command(s) from chat response", n);
//     }
//     free(copy);
// }

// TODO(Task 8): 由 doubao 链路替换 — openclaw_client deleted in Task 1 (notify callback)
// static void on_openclaw_notify(const char *text, const char *source)
// {
//     ESP_LOGI(TAG, "Notification received from %s: %.40s", source ? source : "unknown", text);
//
//     /* ── Device commands always go first, regardless of state ──
//      * [DEVICE:mp3=...], [DEVICE:volume=...], etc. must be processed even
//      * when the device is busy with TTS playback or sending audio.
//      * The state guard below only applies to voice notifications. */
//     char *text_copy = strdup(text);
//     if (text_copy) {
//         int cmd_count = parse_device_commands(text_copy);
//         free(text_copy);
//         if (cmd_count > 0) {
//             ESP_LOGI(TAG, "Parsed %d DEVICE command(s) from notification", cmd_count);
//             return;
//         }
//     }
//
//     const settings_t *cfg = settings_get();
//     if (!cfg->auto_notify) {
//         ESP_LOGI(TAG, "Notification suppressed (auto_notify=off): %.40s", text);
//         return;
//     }
//
//     /* Skip voice notifications while transcribing or speaking.
//      * LISTENING (waiting for user) or RESPONSE (showing text) should not block reminders. */
//     ui_state_t st = ui_get_state();
//     if (st == UI_STATE_SENDING || st == UI_STATE_TTS_LOADING || st == UI_STATE_TTS_PLAYING) {
//         ESP_LOGI(TAG, "Notification skipped (device busy in state %d)", st);
//         return;
//     }
//
//     /* Wake device if sleeping */
//     app_reset_activity_timer();
//
//     /* Show on display if available — LVGL lock required (callback runs in
//      * WebSocket task, not LVGL task) */
// #if BOARD_HAS_DISPLAY
//     lvgl_port_lock(0);
//     app_set_state(UI_STATE_RESPONSE);
//     ui_set_response("Notification", text);
//     lvgl_port_unlock();
// #endif
//
//     /* Always TTS the notification */
//     xEventGroupSetBits(g_app_events, TTS_PLAY_BIT);
//     /* Store text for TTS playback — reuse g_tts_text buffer */
//     extern char g_tts_text[1024];
//     /* Append to buffer if it already contains text (multi-notification handling) */
//     size_t cur_len = strlen(g_tts_text);
//     if (cur_len > 0 && cur_len < sizeof(g_tts_text) - 10) {
//         strncat(g_tts_text, "。 ", sizeof(g_tts_text) - cur_len - 1);
//         cur_len = strlen(g_tts_text);
//     }
//     strncpy(g_tts_text + cur_len, text, sizeof(g_tts_text) - cur_len - 1);
//     g_tts_text[sizeof(g_tts_text) - 1] = '\0';
//
//     /* RGB notification pattern (amber pulse) */
// #if BOARD_HAS_RGB_RING
//     board_rgb_animate(RGB_MODE_BREATHE, 20, 16, 0);  /* amber */
// #endif
// }

// TODO(Task 8): 由 doubao 链路替换 — openclaw_client deleted in Task 1 (state callback)
// static void on_openclaw_state(openclaw_state_t state)
// {
//     /* LVGL lock required — this callback runs in the WebSocket task, not the
//      * LVGL task. All cases below call UI functions (ui_set_*, app_set_state,
//      * app_state_request) which invoke LVGL APIs. Recursive mutex — safe. */
//     lvgl_port_lock(0);
//     switch (state) {
//     case OPENCLAW_STATE_CONNECTED: {
//         ui_state_t cur = ui_get_state();
//         ESP_LOGI(TAG, "OpenClaw connected");
//         xEventGroupSetBits(g_app_events, OC_CONNECTED_BIT);
//         ui_set_openclaw_connected(true);
//         openclaw_request_health();
//         openclaw_request_usage();
//         if (cur <= UI_STATE_CONNECTING || cur == UI_STATE_ERROR) {
//             app_state_request(UI_STATE_IDLE);
//         }
//         break;
//     }
//     case OPENCLAW_STATE_CONNECTING:
//     case OPENCLAW_STATE_AUTHENTICATING:
//         ui_set_openclaw_connected(false);
//         app_state_request(UI_STATE_CONNECTING);
//         ui_set_status_message("Connecting OpenClaw...");
//         break;
//     case OPENCLAW_STATE_CHAT_THINKING:
//         app_set_state(UI_STATE_THINKING);
//         break;
//     case OPENCLAW_STATE_CHAT_STREAMING:
//         app_set_state(UI_STATE_STREAMING);
//         break;
//     case OPENCLAW_STATE_DISCONNECTED:
//         ESP_LOGW(TAG, "OpenClaw disconnected — will auto-reconnect");
//         xEventGroupClearBits(g_app_events, OC_CONNECTED_BIT);
//         ui_set_openclaw_connected(false);
//         {
//             ui_state_t cur = ui_get_state();
//             if (cur == UI_STATE_IDLE || cur == UI_STATE_BOOT || cur == UI_STATE_CONNECTING) {
//                 app_state_request(UI_STATE_CONNECTING);
//                 ui_set_status_message("Reconnecting...");
//             }
//         }
//         error_log_add(ERR_SRC_OPENCLAW, ERR_SEV_WARNING, "WebSocket disconnected");
//         break;
//     case OPENCLAW_STATE_ERROR:
//         ui_set_openclaw_connected(false);
//         app_set_state(UI_STATE_ERROR);
//         ui_set_status_message("OpenClaw error");
//         error_log_add(ERR_SRC_OPENCLAW, ERR_SEV_ERROR, "OpenClaw connection error");
//         break;
//     default:
//         break;
//     }
//     lvgl_port_unlock();
// }

/* Forward-declare webserver toggle handler */
static void handle_webserver_toggle(void);

/* ── One-shot TTS announcement task (PSRAM stack) ────────────────────── */
static volatile bool s_announce_running = false;
static char s_announce_msg[128];
static StaticTask_t s_announce_tcb;
static StackType_t *s_announce_stack = NULL;

static void announce_tts_task(void *arg)
{
    const char *text = (const char *)arg;
    ESP_LOGI(TAG, "TTS announce start: %s", text);
    wake_word_pause();
    /* TODO(Task 7): 由 doubao 链路替换 — tts_speak deleted in Task 1 (tts_client component) */
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    wake_word_resume();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS announce failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "TTS announce complete");
    }
    s_announce_running = false;
    vTaskDelete(NULL);
}

static void speak_announcement(const char *text)
{
    if (s_announce_running) {
        ESP_LOGW(TAG, "TTS announce busy, skipping: %s", text);
        return;
    }
    s_announce_running = true;
    snprintf(s_announce_msg, sizeof(s_announce_msg), "%s", text);

#if CONFIG_IDF_TARGET_ESP32S3
    /* Allocate PSRAM stack once (reused across calls) */
    if (!s_announce_stack) {
        s_announce_stack = heap_caps_calloc(1, 16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_announce_stack) {
        ESP_LOGE(TAG, "Failed to alloc announce stack");
        s_announce_running = false;
        return;
    }
    memset(s_announce_stack, 0, 16384);  /* Clear stack for reuse */
    xTaskCreateStaticPinnedToCore(announce_tts_task, "tts_ann", 16384,
                                   s_announce_msg, 3, s_announce_stack,
                                   &s_announce_tcb, 1);
#else
    xTaskCreatePinnedToCore(announce_tts_task, "tts_ann", 12288,
                            s_announce_msg, 3, NULL, 0);
#endif
}

/* ─── Doubao voice wiring (Task 6b: api_key web 配置) ─────────────────── */

/* Task 7/8 会替换为完整的事件处理（状态机/UI/音频）；此处仅日志占位 */
static void on_doubao_event(doubao_event_type_t type, const void *data, size_t len)
{
    switch (type) {
    case DOUBAO_EVT_SESSION_CREATED:
        ESP_LOGI(TAG, "Doubao session created: id=%s",
                 doubao_get_session_id() ? doubao_get_session_id() : "(none)");
        break;
    case DOUBAO_EVT_ERROR:
        ESP_LOGE(TAG, "Doubao error: %s", data ? (const char *)data : "(null)");
        break;
    case DOUBAO_EVT_DISCONNECTED:
        ESP_LOGW(TAG, "Doubao disconnected");
        break;
    default:
        break;
    }
}

/* cfg 指向 settings 静态字符串即可（doubao_init 内部深拷贝，无需 malloc） */
static void doubao_init_from_settings(void)
{
    const settings_t *cfg = settings_get();
    const char *key = cfg->api_key[0] ? cfg->api_key : AIDB_DEV_API_KEY;
    if (!key[0]) {
        ESP_LOGW(TAG, "Doubao API key 未配置（settings 为空且无 secrets.h dev 值）— 连接将报 AUTH 错误");
    }

    doubao_cfg_t dc = {
        .api_key = key,
        .voice = "zh_female_vv_jupiter_bigtts",
        .instructions = "你是一个桌面上放置的语音助手，用简洁的中文回答。",
        .speed = 0,
        .loudness = 0,
    };
    esp_err_t ret = doubao_init(&dc, on_doubao_event);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "doubao_init failed: %s", esp_err_to_name(ret));
    }
}

/* webserver POST /api/doubao 保存成功后调用（httpd 任务上下文）。
 * 必须重新 doubao_init：api_key 被 doubao_voice/ws_client 深拷贝，
 * 仅 disconnect+connect 不会让新 Key 生效。 */
static void on_doubao_api_key_changed(void)
{
    ESP_LOGI(TAG, "Doubao API key changed via web — reconnecting");
    doubao_disconnect();
    doubao_init_from_settings();
    doubao_connect();
}

/* ─── Main ───────────────────────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  AIWatch v%s", APP_VERSION_STRING);
    ESP_LOGI(TAG, "  OpenClaw ESP32 Interface Device");
    ESP_LOGI(TAG, "========================================");

    /* Suppress noisy I2C peripheral errors (occasional bus contention, harmless).
     * tca95xx_16, touch_axs5106l, and esp_io_expander share the same I2C bus
     * and occasionally fail when accessed concurrently. These single-sample
     * misses are auto-recovered on the next poll — not worth ERROR level. */
    esp_log_level_set("tca95xx_16", ESP_LOG_WARN);
    esp_log_level_set("esp_io_expander", ESP_LOG_WARN);
    esp_log_level_set("touch_axs5106l", ESP_LOG_WARN);
    /* FT5x06 I2C periodic read errors are normal when touch controller has
     * no data ready — suppress to WARN level to avoid log noise. */
    esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_WARN);
    esp_log_level_set("FT5x06", ESP_LOG_WARN);

    /* Log wake cause (useful for deep sleep debugging) */
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        const char *cause = "unknown";
        switch (wakeup) {
            case ESP_SLEEP_WAKEUP_EXT0:      cause = "EXT0 (button)"; break;
            case ESP_SLEEP_WAKEUP_EXT1:      cause = "EXT1"; break;
            case ESP_SLEEP_WAKEUP_TIMER:     cause = "TIMER"; break;
            case ESP_SLEEP_WAKEUP_GPIO:      cause = "GPIO"; break;
            case ESP_SLEEP_WAKEUP_UART:      cause = "UART"; break;
            default: break;
        }
        ESP_LOGW(TAG, "Woke from deep sleep — cause: %s (%d)", cause, wakeup);
    }

    /* Init settings from NVS (must be first) */
    settings_t defaults = {0};
    strncpy(defaults.wifi_ssid,     AIDB_DEV_WIFI_SSID,     sizeof(defaults.wifi_ssid) - 1);
    strncpy(defaults.wifi_password, AIDB_DEV_WIFI_PASSWORD,  sizeof(defaults.wifi_password) - 1);
    /* openclaw/mimo secrets removed with their components (Task 1);
     * doubao api_key 已由 Task 6b 接线（settings 优先，空回落
     * SECRETS_DOUBAO_API_KEY）；voice/instructions 占位值在下方
     * doubao_init_from_settings()，Task 7 完善。 */
    defaults.volume = APP_SPEAKER_VOLUME;
    settings_init(&defaults);

    /* Auto-generate device key if empty or invalid (not 64 chars) */
    settings_t *cfg_mut = settings_get_mutable();
    if (strlen(cfg_mut->oc_device_key) != 64) {
        ESP_LOGI(TAG, "Generating new device identity (old key was '%s')...", cfg_mut->oc_device_key);
        uint8_t seed[32];
        esp_fill_random(seed, 32);
        for (int i = 0; i < 32; i++) {
            sprintf(cfg_mut->oc_device_key + i*2, "%02x", seed[i]);
        }
        settings_save();
    }

    /* Error log */
    error_log_init();

    const settings_t *cfg = settings_get();

    g_app_events = xEventGroupCreate();
    if (!g_app_events) {
        ESP_LOGE(TAG, "Failed to create event group!");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
    ESP_LOGI(TAG, "Event group created successfully");

    /* Initialize centralized state machine */
    app_state_machine_init();

    /* Pass event group to UI for button callbacks */
    ui_set_event_group(g_app_events);

    /* Init hardware */
    esp_err_t ret = board_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Board init failed: %s", esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
    ESP_LOGI(TAG, "Board: %s (%s)", board_get_name(), board_get_mcu());

    /* Configure power management — CPU frequency scaling */
#ifdef CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,  /* Don't auto-sleep — WiFi needs active CPU */
    };
    ret = esp_pm_configure(&pm_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "PM: CPU scaling %d-%dMHz", 80, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    } else {
        ESP_LOGW(TAG, "PM configure failed: %s", esp_err_to_name(ret));
    }
#endif

    board_audio_set_volume(cfg->volume);
    board_display_set_brightness(cfg->brightness);

    /* Init SD card (non-critical, continue without it) */
    ret = board_sdcard_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card not available (no MP3 playback)");
    }

    /* Init MP3 player (after audio is ready) */
    if (board_sdcard_is_inserted()) {
        ret = mp3_player_init();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "MP3 player ready");
            /* Completion callback: auto-play next song */
            mp3_player_set_completion_cb(on_mp3_complete);
        } else {
            ESP_LOGW(TAG, "MP3 player init failed");
        }
    }

    /* RGB - DISABLED: UI layer will control LEDs via ui_update_led_for_state */
    // board_rgb_task_start();
    // if (cfg->rgb_enabled) {
    // #if BOARD_RGB_LED_COUNT > 1
    //     board_rgb_animate(RGB_MODE_RAINBOW_SPIN, 20, 0, 0);
    // #else
    //     board_rgb_animate(RGB_MODE_SOLID, 16, 16, 0);
    // #endif
    // } else {
    //     board_rgb_animate(RGB_MODE_OFF, 0, 0, 0);
    // }
    
    /* Initialize RGB to off, UI will set colors based on state */
#if BOARD_HAS_RGB_RING
    board_rgb_set(0, 0, 0);
#endif

    /* Init UI */
    ret = ui_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "UI init failed (continuing)");
    /* Init tasks screen */
    ui_tasks_init();
    ui_tasks_set_event_group(g_app_events);
    /* MP3 player UI is initialized by ui_init() via ui_mp3_ui_init() */
    /* Post-init UI operations — LVGL lock required (main task != LVGL task) */
    lvgl_port_lock(0);
    app_state_request(UI_STATE_BOOT);
    ui_set_status_message("Starting...");
    lvgl_port_unlock();
    
    /* Show boot screen for at least 2 seconds */
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Init MiMo services from settings */
    // TODO(Task 7): 由 doubao 链路替换 — stt_init/tts_init deleted in Task 1 (stt/tts components)
    // /* ASR (MiMo-V2.5-ASR) */
    // stt_config_t stt_cfg = {
    //     .api_key   = cfg->mimo_api_key,
    //     .model     = cfg->asr_model,
    //     .url       = cfg->mimo_url,
    //     .sample_rate = 16000,
    //     .timeout_ms  = 60000,
    // };
    // stt_init(&stt_cfg);
    //
    // /* TTS (MiMo-V2.5-TTS) */
    // tts_config_t tts_cfg = {
    //     .api_key   = cfg->mimo_api_key,
    //     .url       = cfg->mimo_url,
    //     .model     = cfg->tts_model,
    //     .voice     = cfg->tts_voice,
    // };
    // tts_init(&tts_cfg);

    /* Connect WiFi */
    wifi_manager_init(cfg->wifi_ssid, cfg->wifi_password, on_wifi_state);

    EventBits_t bits = xEventGroupWaitBits(g_app_events, WIFI_CONNECTED_BIT,
                                            pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "WiFi timeout — continuing anyway");
    } else {
        ESP_LOGI(TAG, "WiFi ready");
    }

    /* Start web server immediately after WiFi connects — before SNTP/OpenClaw.
     * This ensures the web UI is accessible for debugging even when OpenClaw
     * fails to connect, and avoids the web server being killed by deep sleep
     * before the user can access it. */
    if (cfg->webserver_enabled) {
        xEventGroupSetBits(g_app_events, WEBSERVER_TOGGLE_BIT);
    }

    /* Wait for SNTP (skip in AP mode) */
    // Check if we're in AP mode by checking if we have a valid STA IP
    const char* ip = wifi_manager_get_ip();
    bool is_ap_mode = (ip != NULL && strcmp(ip, "192.168.4.1") == 0);
    
    if (!is_ap_mode) {
        // Only wait for SNTP in STA mode (connected to router)
        ESP_LOGI(TAG, "Waiting for SNTP time sync...");
        bool time_ok = false;
        /* Wait up to 30s (60 * 500ms) for SNTP sync */
        for (int i = 0; i < 60; i++) {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            /* 1700000000 is ~Nov 2023. If greater, time is likely synced. */
            if (tv.tv_sec > 1700000000) {
                ESP_LOGI(TAG, "Time synced (attempt %d)", i + 1);
                /* Print current date/time */
                time_t now = tv.tv_sec;
                struct tm timeinfo;
                localtime_r(&now, &timeinfo);
                char strftime_buf[64];
                strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
                ESP_LOGI(TAG, "Current time: %s (UTC)", strftime_buf);
                time_ok = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (!time_ok) {
            ESP_LOGE(TAG, "SNTP timeout! Authentication WILL fail due to invalid timestamps.");
            error_log_add(ERR_SRC_DEVICE, ERR_SEV_ERROR, "SNTP time sync failed");
#if BOARD_HAS_RGB_RING
            /* Optional: blink LED red to indicate time sync error */
            board_rgb_animate(RGB_MODE_BLINK, 255, 0, 0);
#endif
        }
    } else {
        // In AP mode, skip SNTP wait for faster captive portal
        ESP_LOGI(TAG, "AP mode detected (IP=%s) - skipping SNTP wait for fast provisioning", ip);
    }

    /* Skip OpenClaw connection in AP mode */
    // TODO(Task 8): 由 doubao 链路替换 — openclaw_client deleted in Task 1
    // if (!is_ap_mode) {
    //     /* Connect to OpenClaw */
    //     openclaw_config_t oc_config = {
    //         .host = cfg->oc_host,
    //         .port = cfg->oc_port,
    //         .token = cfg->oc_token,
    //         .device_key_hex = cfg->oc_device_key,
    //         .device_token = cfg->oc_device_token,
    //     };
    //     openclaw_init(&oc_config, on_openclaw_state);
    //     openclaw_set_notify_cb(on_openclaw_notify);
    //     openclaw_set_mp3_list_cb(app_get_sd_mp3_list_str);
    //     openclaw_set_device_cmd_cb(on_external_device_cmd);
    // } else {
    //     ESP_LOGI(TAG, "AP mode - skipping OpenClaw initialization (will restart after provisioning)");
    // }

    /* In AP mode, skip network-dependent tasks */
    if (!is_ap_mode) {
        /* Brief delay to ensure network is fully ready before first connection */
        vTaskDelay(pdMS_TO_TICKS(500));
        // TODO(Task 8): 由 doubao 链路替换 — openclaw_connect deleted in Task 1
        // openclaw_connect();

        /* Start background tasks */
        app_tasks_start();

        /* Doubao voice: api_key 走 settings（web 配置），为空回落 dev 值。
         * Task 6b 接线；自动连接策略与完整事件回调在 Task 7/8。 */
        doubao_init_from_settings();
        webserver_set_doubao_changed_cb(on_doubao_api_key_changed);

        /* Init wake word detection (non-critical — continue if fails) */
        ret = wake_word_init();
        if (ret == ESP_OK) {
            wake_word_start(g_app_events, WAKE_WORD_BIT);
            ESP_LOGI(TAG, "Wake word: \"%s\"", wake_word_get_phrase());
        } else {
            ESP_LOGW(TAG, "Wake word init failed — voice wake disabled");
        }

        /* Scan SD card for MP3 files so the AI knows what's available */
        app_sd_mp3_scan_init();
        
        /* Initialize notes manager for chat history storage */
        notes_manager_init();
    } else {
        ESP_LOGI(TAG, "AP mode - skipping OpenClaw connect, background tasks, and wake word");
    }
    
    /* Serial command task always runs (useful for debugging in AP mode) */
    serial_cmd_task_start();

    /* ── DEBUG: heap snapshot at startup ── */
    ESP_LOGI(TAG, "=== HEAP DEBUG ===");
    ESP_LOGI(TAG, "Internal free: %lu  largest: %lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "DMA free: %lu  largest: %lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    ESP_LOGI(TAG, "PSRAM free: %lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "==================");

    ESP_LOGI(TAG, "All tasks started. Ready.");

    /* ── Main event loop ── */
    int heartbeat = 0;
    /* Track last MP3 UI state to avoid redundant full redraws.
     * Calling ui_mp3_ui_show() every loop iteration forces LVGL to
     * invalidate the entire panel (labels + move_foreground), flooding
     * the already DMA-starved LCD SPI and freezing animations. */
    const char *last_mp3_file = NULL;  /* NULL → force first update via !last_mp3_file check */
    bool last_mp3_playing = false;
    while (1) {
        EventBits_t ev = xEventGroupWaitBits(g_app_events,
                                              KNOB_PRESSED_BIT |
                                              WEBSERVER_TOGGLE_BIT | DETAILS_BIT |
                                              TOUCH_BIT | TASKS_SCREEN_BIT |
                                              WAKE_WORD_BIT |
                                              MP3_PLAYER_BIT | MP3_CMD_BIT,
                                              pdTRUE, pdFALSE,
                                              pdMS_TO_TICKS(100));

        if (ev & TOUCH_BIT) {
            app_reset_activity_timer();
            if (g_recording) {
                /* Touch during recording = cancel */
                ESP_LOGI(TAG, "Touch during recording — cancelling");
                xEventGroupSetBits(g_app_events, CANCEL_BIT);
            } else if (ui_get_state() == UI_STATE_RESPONSE) {
                if (g_tts_pending) {
                    ESP_LOGD(TAG, "Touch during RESPONSE ignored — TTS pending");
                } else {
                    /* Touch during response = dismiss */
                    ESP_LOGI(TAG, "Touch dismissed response");
                    g_response_shown_at = 0;
                    lvgl_port_lock(0);
                    app_state_request(UI_STATE_IDLE);
                    lvgl_port_unlock();
                }
            } else if (!app_is_sleeping()) {
                board_play_tick();
            }
        }

        if (ev & TASKS_SCREEN_BIT) {
            app_reset_activity_timer();
            lvgl_port_lock(0);
            if (ui_tasks_is_visible()) {
                ui_tasks_hide();
            } else {
                ui_tasks_show();
            }
            lvgl_port_unlock();
        }

        /* DISABLED: Manual MP3 UI toggle - new UI auto-shows on playback */
        /* if (ev & MP3_PLAYER_BIT) {
            app_reset_activity_timer();
            if (ui_mp3_is_visible()) {
                ui_mp3_hide();
            } else {
                ui_mp3_show();
            }
        } */

        if (ev & MP3_CMD_BIT) {
            app_reset_activity_timer();
            /* Suppress TTS — MP3 command supersedes voice playback.
             * Both share the same audio HW and internal heap;
             * running them simultaneously causes heap exhaustion. */
            g_tts_pending = false;
            lvgl_port_lock(0);
            app_process_mp3_cmd();
            lvgl_port_unlock();
        }

        if (ev & KNOB_PRESSED_BIT) {
            app_reset_activity_timer();
            
            /* If on MP3 player screen in selection mode, confirm selection */
            if (ui_mp3_ui_is_visible()) {
                lvgl_port_lock(0);
                bool confirmed = ui_mp3_ui_handle_selection_input(0);  /* delta=0 means button press */
                if (confirmed) {
                    /* Selection confirmed - will be handled by knob_task */
                    lvgl_port_unlock();
                    continue;
                } else {
                    /* Not in selection mode, hide UI */
                    ui_mp3_ui_hide();
                    lvgl_port_unlock();
                    continue;
                }
            }
            
            /* If on tasks screen, go back to main */
            if (ui_tasks_is_visible()) {
                lvgl_port_lock(0);
                ui_tasks_hide();
                lvgl_port_unlock();
                continue;
            }
            /* If sleeping or just woke, don't start recording */
            if (app_is_sleeping() || app_just_woke()) {
                ESP_LOGI(TAG, "Woke from sleep via event — ignoring action");
                continue;
            }
            ui_state_t cur = ui_get_state();
            switch (cur) {
            case UI_STATE_IDLE:
            case UI_STATE_RESPONSE:
                /* State machine handles wake_word_pause */
                lvgl_port_lock(0);
                esp_err_t req_ret = app_state_request(UI_STATE_LISTENING);
                lvgl_port_unlock();
                if (req_ret == ESP_OK) {
                    // TODO(Task 10): 由 doubao 链路替换 — voice_chat.c deleted in Task 1
                    // voice_chat_start();
                }
                break;
            case UI_STATE_TTS_PLAYING:
            case UI_STATE_TTS_LOADING:
                // TODO(Task 7): 由 doubao 链路替换 — tts_stop deleted in Task 1
                // tts_stop();
                g_continue_listening = false;
                lvgl_port_lock(0);
                app_set_state(UI_STATE_IDLE);
                lvgl_port_unlock();
                break;
            case UI_STATE_THINKING:
            case UI_STATE_STREAMING:
                /* Abort the current chat operation */
                ESP_LOGI(TAG, "Aborting chat (state=%d)...", cur);
                // TODO(Task 8): 由 doubao 链路替换 — openclaw_chat_abort deleted in Task 1
                // openclaw_chat_abort();
                lvgl_port_lock(0);
                ui_set_status_message("Aborting...");
                lvgl_port_unlock();
#if BOARD_HAS_RGB_RING
                board_rgb_animate(RGB_MODE_BLINK, 32, 16, 0);
#endif
                vTaskDelay(pdMS_TO_TICKS(800));
                lvgl_port_lock(0);
                app_set_state(UI_STATE_IDLE);
                lvgl_port_unlock();
                break;
            default:
                break;
            }
        }

        if (ev & WAKE_WORD_BIT) {
            app_reset_activity_timer();
            ESP_LOGI(TAG, "Wake word detected!");
            ui_state_t cur = ui_get_state();
            /* Allow wake word from IDLE, RESPONSE, or PLAYING_MP3 states. */
            if (cur == UI_STATE_IDLE || cur == UI_STATE_RESPONSE || cur == UI_STATE_PLAYING_MP3) {
                /* Stop conflicting subsystems before requesting LISTENING state */
                // TODO(Task 7): 由 doubao 链路替换 — tts_is_playing/tts_stop deleted in Task 1
                // if (tts_is_playing()) {
                //     ESP_LOGI(TAG, "Stopping TTS due to wake word");
                //     tts_stop();
                //     vTaskDelay(pdMS_TO_TICKS(50));
                // }
                if (cur == UI_STATE_PLAYING_MP3) {
                    ESP_LOGI(TAG, "Stopping MP3 playback due to wake word");
                    mp3_player_stop();
                    vTaskDelay(pdMS_TO_TICKS(100));
                }

                /* State machine handles wake_word_pause() via
                 * on_enter_state(LISTENING), and wake_word_resume()
                 * via on_leave_state(LISTENING). */
                lvgl_port_lock(0);
                esp_err_t ww_ret = app_state_request(UI_STATE_LISTENING);
                lvgl_port_unlock();
                if (ww_ret == ESP_OK) {
                    // TODO(Task 10): 由 doubao 链路替换 — voice_chat.c deleted in Task 1
                    // voice_chat_start();
                }
            }
        }

        // TODO(Task 8): 由 doubao 链路替换 — openclaw_get_state/openclaw_chat_send_details deleted in Task 1
        // if (ev & DETAILS_BIT) {
        //     app_reset_activity_timer();
        //     if (openclaw_get_state() == OPENCLAW_STATE_CONNECTED) {
        //         ESP_LOGI(TAG, "Requesting details...");
        //         lvgl_port_lock(0);
        //         app_set_state(UI_STATE_SENDING);
        //         lvgl_port_unlock();
        //         openclaw_chat_send_details(app_on_chat_response);
        //     }
        // }

        if (ev & WEBSERVER_TOGGLE_BIT) {
            handle_webserver_toggle();
        }

        /* Periodic MP3 UI update — only redraw when track or play-state changes.
         * In steady state the pulse timer (inside LVGL context) drives the
         * breathing animation; calling ui_mp3_ui_show() every iteration
         * restarts the label scroll animation and floods the DMA-starved
         * LCD SPI, freezing the UI. */
        if (ui_mp3_ui_is_visible()) {
            mp3_playback_state_t st = mp3_player_get_state();
            if (st == MP3_STATE_PLAYING || st == MP3_STATE_PAUSED) {
                const char *cur = mp3_player_get_current_file();
                bool playing = (st == MP3_STATE_PLAYING);
                bool file_changed = (!cur || !last_mp3_file ||
                                     strcmp(cur ? cur : "", last_mp3_file) != 0);
                if (file_changed || playing != last_mp3_playing) {
                    last_mp3_file = cur ? cur : "";
                    last_mp3_playing = playing;
                    uint32_t dur = mp3_player_get_duration_sec();
                    lvgl_port_lock(0);
                    ui_mp3_ui_show(cur ? cur : "", 0, 0, dur, playing);
                    lvgl_port_unlock();
                }
                /* else: steady state — no redraw, pulse timer handles animation */
            }
        }

        /* ── Heartbeat: log every 10s (100 * 100ms) to show main loop is alive ── */
        if (++heartbeat >= 100) {
            heartbeat = 0;
            ESP_LOGI(TAG, "HEARTBEAT: internal=%lu DMA=%lu PSRAM=%lu",
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA),
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ─── Web server toggle ──────────────────────────────────────────────── */
static void handle_webserver_toggle(void)
{
    if (!webserver_is_running()) {
        const char *ip = wifi_manager_get_ip();
        if (webserver_start() == ESP_OK) {
            ESP_LOGI(TAG, "Web server started on http://%s", ip);
            lvgl_port_lock(0);
            ui_set_webserver_status(true);
            lvgl_port_unlock();
            settings_get_mutable()->webserver_enabled = true;
            settings_save();
#if BOARD_HAS_DISPLAY
            /* Show IP on screen for 5 seconds */
            char msg[80];
            snprintf(msg, sizeof(msg), "Go to\n%s", ip);
            lvgl_port_lock(0);
            ui_set_status_message(msg);
            lvgl_port_unlock();

            /* Also speak it for convenience */
            char audio_msg[128];
            snprintf(audio_msg, sizeof(audio_msg), "网页服务器已启动，请访问 %s", ip);
            speak_announcement(audio_msg);

            vTaskDelay(pdMS_TO_TICKS(5000));
            lvgl_port_lock(0);
            if (ui_get_state() == UI_STATE_IDLE || ui_get_state() == UI_STATE_BOOT) {
                app_set_state(UI_STATE_IDLE);
            }
            lvgl_port_unlock();
#else
            /* Audio-only device: speak the IP address */
            {
                char msg[128];
                snprintf(msg, sizeof(msg), "网页服务器已启动，请访问 %s", ip);
                speak_announcement(msg);
            }
#endif
        } else {
            error_log_add(ERR_SRC_DEVICE, ERR_SEV_ERROR, "Web server failed to start");
        }
    } else {
        webserver_stop();
        ESP_LOGI(TAG, "Web server stopped");
        lvgl_port_lock(0);
        ui_set_webserver_status(false);
        lvgl_port_unlock();
        settings_get_mutable()->webserver_enabled = false;
        settings_save();
    }
}
