/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * AIWearable — OpenClaw ESP32 Interface Device
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
#include "voice_chat.h"
#include "serial_cmd.h"
#include "app_tasks.h"
#include "settings.h"
#include "error_log.h"
#include "secrets.h"
#include "ui_provisioning.h"

#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

#include "wifi_manager.h"
#include "openclaw_client.h"
#include "tts_client.h"
#include "stt_client.h"
#include "notes_manager.h"
#include "ui.h"
#include "ui_tasks.h"
#include "ui_mp3_ui.h"
#include "ui_state_gif.h"
#include "mp3_player.h"
#include "webserver.h"
#include "camera.h"
#include "wake_word.h"

static const char *TAG = "aiwearable";

/* ─── WiFi callback ──────────────────────────────────────────────────── */
/* ─── MP3 player completion callback ──────────────────────────────────── */
static void on_mp3_complete(void)
{
    ESP_LOGI(TAG, "MP3 playback finished");
    
    /* CRITICAL: Reset continuous conversation mode after MP3 playback.
     * MP3 playback is a deliberate user action that should break the
     * continuous conversation flow. If we don't reset this flag, the
     * system will stay in IDLE forever waiting for auto-listen, but
     * wake word detection might be disabled or in wrong state. */
    extern bool g_continue_listening;
    if (g_continue_listening) {
        ESP_LOGI(TAG, "Resetting continuous conversation mode after MP3");
        g_continue_listening = false;
    }
    
    /* CRITICAL: Switch UI state back to IDLE so wake word can trigger voice chat.
     * Without this, the system stays in PLAYING_MP3 state and wake word events
     * are ignored because app_main.c only responds in IDLE or RESPONSE states. */
    app_set_state(UI_STATE_IDLE);

    /* Reset activity timer so sleep countdown starts fresh after playback */
    app_reset_activity_timer();
    
    /* UI update is handled inside mp3_player event callback via state_cb */
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
        ui_set_wifi_status(true, wifi_manager_get_rssi());
        break;
    case WIFI_STATE_CONNECTING:
        ui_set_wifi_status(false, 0);
        app_set_state(UI_STATE_CONNECTING);
        ui_set_status_message("Connecting WiFi...");
        break;
    case WIFI_STATE_FAILED:
        ui_set_wifi_status(false, 0);
        app_set_state(UI_STATE_ERROR);
        ui_set_status_message("WiFi failed!\nStarting AP...");
        error_log_add(ERR_SRC_WIFI, ERR_SEV_ERROR, "WiFi connection failed");
        ESP_LOGI(TAG, "WiFi failed, starting AP for config...");
        wifi_manager_start_ap(NULL, NULL);
        
        // Immediately start web server for captive portal
        xEventGroupSetBits(g_app_events, WEBSERVER_TOGGLE_BIT);
        
        // Show provisioning instructions on screen (no delay needed)
#if BOARD_HAS_DISPLAY
        ui_show_provisioning_qr(NULL);  // Will show http://192.168.4.1
#endif
        
        break;
    default:
        ui_set_wifi_status(false, 0);
        break;
    }
}

/* ─── OpenClaw state callback ────────────────────────────────────────── */

/* Notification callback: incoming message not initiated by this device */
static void on_openclaw_notify(const char *text, const char *source)
{
    const settings_t *cfg = settings_get();
    if (!cfg->auto_notify) {
        ESP_LOGI(TAG, "Notification suppressed (auto_notify=off): %.40s", text);
        return;
    }

    ESP_LOGI(TAG, "Notification received from %s: %.40s", source ? source : "unknown", text);

    /* Allow notifications unless we are currently transcribing or speaking.
     * LISTENING (waiting for user) or RESPONSE (showing text) should not block reminders. */
    ui_state_t st = ui_get_state();
    if (st == UI_STATE_SENDING || st == UI_STATE_TTS_LOADING || st == UI_STATE_TTS_PLAYING) {
        ESP_LOGI(TAG, "Notification skipped (device busy in state %d)", st);
        return;
    }

    /* Wake device if sleeping */
    app_reset_activity_timer();

    /* Parse DEVICE commands first (e.g., [DEVICE:widget=...]) */
    char *text_copy = strdup(text);
    if (text_copy) {
        int cmd_count = parse_device_commands(text_copy);
        if (cmd_count > 0) {
            ESP_LOGI(TAG, "Parsed %d DEVICE command(s) from notification", cmd_count);
            /* If widget was shown, don't show as regular notification */
            free(text_copy);
            return;
        }
        free(text_copy);
    }

    /* Show on display if available */
#if BOARD_HAS_DISPLAY
    app_set_state(UI_STATE_RESPONSE);
    ui_set_response("Notification", text);
#endif

    /* Always TTS the notification */
    xEventGroupSetBits(g_app_events, TTS_PLAY_BIT);
    /* Store text for TTS playback — reuse g_tts_text buffer */
    extern char g_tts_text[1024];
    /* Append to buffer if it already contains text (multi-notification handling) */
    size_t cur_len = strlen(g_tts_text);
    if (cur_len > 0 && cur_len < sizeof(g_tts_text) - 10) {
        strncat(g_tts_text, "。 ", sizeof(g_tts_text) - cur_len - 1);
        cur_len = strlen(g_tts_text);
    }
    strncpy(g_tts_text + cur_len, text, sizeof(g_tts_text) - cur_len - 1);
    g_tts_text[sizeof(g_tts_text) - 1] = '\0';

    /* RGB notification pattern (amber pulse) */
    board_rgb_animate(RGB_MODE_BREATHE, 20, 16, 0);  /* amber */
}

static void on_openclaw_state(openclaw_state_t state)
{
    switch (state) {
    case OPENCLAW_STATE_CONNECTED: {
        ui_state_t cur = ui_get_state();
        ESP_LOGI(TAG, "OpenClaw connected");
        xEventGroupSetBits(g_app_events, OC_CONNECTED_BIT);
        ui_set_openclaw_connected(true);
        openclaw_request_health();
        openclaw_request_usage();
        if (cur <= UI_STATE_CONNECTING || cur == UI_STATE_ERROR) {
            app_set_state(UI_STATE_IDLE);
        }
        break;
    }
    case OPENCLAW_STATE_CONNECTING:
    case OPENCLAW_STATE_AUTHENTICATING:
        ui_set_openclaw_connected(false);
        app_set_state(UI_STATE_CONNECTING);
        ui_set_status_message("Connecting OpenClaw...");
        break;
    case OPENCLAW_STATE_CHAT_THINKING:
        app_set_state(UI_STATE_THINKING);
        break;
    case OPENCLAW_STATE_CHAT_STREAMING:
        app_set_state(UI_STATE_STREAMING);
        break;
    case OPENCLAW_STATE_DISCONNECTED:
        ESP_LOGW(TAG, "OpenClaw disconnected — will auto-reconnect");
        xEventGroupClearBits(g_app_events, OC_CONNECTED_BIT);
        ui_set_openclaw_connected(false);
        {
            ui_state_t cur = ui_get_state();
            if (cur == UI_STATE_IDLE || cur == UI_STATE_BOOT || cur == UI_STATE_CONNECTING) {
                app_set_state(UI_STATE_CONNECTING);
                ui_set_status_message("Reconnecting...");
            }
        }
        error_log_add(ERR_SRC_OPENCLAW, ERR_SEV_WARNING, "WebSocket disconnected");
        break;
    case OPENCLAW_STATE_ERROR:
        ui_set_openclaw_connected(false);
        app_set_state(UI_STATE_ERROR);
        ui_set_status_message("OpenClaw error");
        error_log_add(ERR_SRC_OPENCLAW, ERR_SEV_ERROR, "OpenClaw connection error");
        break;
    default:
        break;
    }
}

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
    esp_err_t err = tts_speak(text);
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

/* ─── Main ───────────────────────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  AIWearable v%s", APP_VERSION_STRING);
    ESP_LOGI(TAG, "  OpenClaw ESP32 Interface Device");
    ESP_LOGI(TAG, "========================================");

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
    strncpy(defaults.wifi_ssid,     SECRETS_WIFI_SSID,     sizeof(defaults.wifi_ssid) - 1);
    strncpy(defaults.wifi_password, SECRETS_WIFI_PASSWORD,  sizeof(defaults.wifi_password) - 1);
    strncpy(defaults.oc_host,       SECRETS_OPENCLAW_HOST,  sizeof(defaults.oc_host) - 1);
    defaults.oc_port = SECRETS_OPENCLAW_PORT;
    strncpy(defaults.oc_token,      SECRETS_OPENCLAW_TOKEN, sizeof(defaults.oc_token) - 1);
    strncpy(defaults.oc_device_key, SECRETS_DEVICE_KEY_HEX, sizeof(defaults.oc_device_key) - 1);
    strncpy(defaults.dashscope_api_key, SECRETS_DASHSCOPE_API_KEY, sizeof(defaults.dashscope_api_key) - 1);
    strncpy(defaults.stt_model,        SECRETS_STT_MODEL,         sizeof(defaults.stt_model) - 1);
    strncpy(defaults.stt_endpoint,      SECRETS_STT_ENDPOINT,      sizeof(defaults.stt_endpoint) - 1);
    strncpy(defaults.mimo_api_key,      SECRETS_MIMO_API_KEY,      sizeof(defaults.mimo_api_key) - 1);
    strncpy(defaults.mimo_url,          SECRETS_MIMO_ENDPOINT,     sizeof(defaults.mimo_url) - 1);
    strncpy(defaults.mimo_model,        SECRETS_MIMO_MODEL,        sizeof(defaults.mimo_model) - 1);
    strncpy(defaults.mimo_voice,        SECRETS_MIMO_VOICE,        sizeof(defaults.mimo_voice) - 1);
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

    /* Camera init deferred to first use (saves ~5-10mA from SSCMA background tasks) */
    ESP_LOGI(TAG, "Camera: deferred to first use (power saving)");
    (void)ret;  /* camera_init() will be called on first CAMERA_BIT */

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
    board_rgb_set(0, 0, 0);

    /* Init UI */
    ret = ui_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "UI init failed (continuing)");
    /* Init tasks screen */
    ui_tasks_init();
    ui_tasks_set_event_group(g_app_events);
    /* MP3 player UI is initialized by ui_init() via ui_mp3_ui_init() */
    app_set_state(UI_STATE_BOOT);
    ui_set_status_message("Starting...");
    
    /* Show boot screen for at least 2 seconds */
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Init TTS from settings (MiMo) */
    tts_config_t tts_cfg = {
        .mimo_api_key = cfg->mimo_api_key,
        .mimo_url     = cfg->mimo_url,
        .mimo_model   = cfg->mimo_model,
        .mimo_voice   = cfg->mimo_voice,
    };
    tts_init(&tts_cfg);

    /* Init STT from settings (DashScope) */
    stt_config_t stt_cfg = {
        .sample_rate = 16000,
        .timeout_ms  = 5000,
        .silence_ms  = 1300,
    };
    strlcpy(stt_cfg.api_key,  cfg->dashscope_api_key, sizeof(stt_cfg.api_key));
    strlcpy(stt_cfg.model,    cfg->stt_model,         sizeof(stt_cfg.model));
    strlcpy(stt_cfg.endpoint, cfg->stt_endpoint,       sizeof(stt_cfg.endpoint));
    stt_init(&stt_cfg);

    /* Show boot GIF for at least 1.5 seconds before connecting WiFi */
    ESP_LOGI(TAG, "Showing boot animation...");
    vTaskDelay(pdMS_TO_TICKS(1500));

    /* Connect WiFi */
    wifi_manager_init(cfg->wifi_ssid, cfg->wifi_password, on_wifi_state);

    EventBits_t bits = xEventGroupWaitBits(g_app_events, WIFI_CONNECTED_BIT,
                                            pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "WiFi timeout — continuing anyway");
    } else {
        ESP_LOGI(TAG, "WiFi ready");
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
            /* Optional: blink LED red to indicate time sync error */
            board_rgb_animate(RGB_MODE_BLINK, 255, 0, 0);
        }
    } else {
        // In AP mode, skip SNTP wait for faster captive portal
        ESP_LOGI(TAG, "AP mode detected (IP=%s) - skipping SNTP wait for fast provisioning", ip);
    }

    /* Skip OpenClaw connection in AP mode */
    if (!is_ap_mode) {
        /* Connect to OpenClaw */
        openclaw_config_t oc_config = {
            .host = cfg->oc_host,
            .port = cfg->oc_port,
            .token = cfg->oc_token,
            .device_key_hex = cfg->oc_device_key,
            .device_token = cfg->oc_device_token,
        };
        openclaw_init(&oc_config, on_openclaw_state);
        openclaw_set_notify_cb(on_openclaw_notify);
        openclaw_set_mp3_list_cb(app_get_sd_mp3_list_str);
    } else {
        ESP_LOGI(TAG, "AP mode - skipping OpenClaw initialization (will restart after provisioning)");
    }

    /* Start web server if enabled in settings */
    if (cfg->webserver_enabled) {
        xEventGroupSetBits(g_app_events, WEBSERVER_TOGGLE_BIT);
    }

    /* In AP mode, skip network-dependent tasks */
    if (!is_ap_mode) {
        /* Brief delay to ensure network is fully ready before first connection */
        vTaskDelay(pdMS_TO_TICKS(500));
        openclaw_connect();

        /* Start background tasks */
        app_tasks_start();
        
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
        
        /* Initialize GIF state manager */
#if CONFIG_LV_USE_GIF
        ui_state_gif_init();
#endif
    } else {
        ESP_LOGI(TAG, "AP mode - skipping OpenClaw connect, background tasks, and wake word");
    }
    
    /* Serial command task always runs (useful for debugging in AP mode) */
    serial_cmd_task_start();

    ESP_LOGI(TAG, "All tasks started. Ready.");

    /* ── Main event loop ── */
    while (1) {
        EventBits_t ev = xEventGroupWaitBits(g_app_events,
                                              KNOB_PRESSED_BIT |
                                              WEBSERVER_TOGGLE_BIT | DETAILS_BIT |
                                              TOUCH_BIT | TASKS_SCREEN_BIT |
                                              CAMERA_BIT | WAKE_WORD_BIT |
                                              MP3_PLAYER_BIT,
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
                    app_set_state(UI_STATE_IDLE);
                }
            } else if (!app_is_sleeping()) {
                board_play_tick();
            }
        }

        if (ev & TASKS_SCREEN_BIT) {
            app_reset_activity_timer();
            if (ui_tasks_is_visible()) {
                ui_tasks_hide();
            } else {
                ui_tasks_show();
            }
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
            app_process_mp3_cmd();
        }

        if (ev & KNOB_PRESSED_BIT) {
            app_reset_activity_timer();
            
            /* If on MP3 player screen in selection mode, confirm selection */
            if (ui_mp3_ui_is_visible()) {
                bool confirmed = ui_mp3_ui_handle_selection_input(0);  /* delta=0 means button press */
                if (confirmed) {
                    /* Selection confirmed - will be handled by knob_task */
                    continue;
                } else {
                    /* Not in selection mode, hide UI */
                    ui_mp3_ui_hide();
                    continue;
                }
            }
            
            /* If on tasks screen, go back to main */
            if (ui_tasks_is_visible()) {
                ui_tasks_hide();
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
                /* Both idle and response → start new recording */
                wake_word_pause();
                voice_chat_start();
                wake_word_resume();
                break;
            case UI_STATE_TTS_PLAYING:
            case UI_STATE_TTS_LOADING:
                tts_stop();
                g_continue_listening = false;
                app_set_state(UI_STATE_IDLE);
                break;
            case UI_STATE_THINKING:
            case UI_STATE_STREAMING:
                /* Abort the current chat operation */
                ESP_LOGI(TAG, "Aborting chat (state=%d)...", cur);
                openclaw_chat_abort();
                ui_set_status_message("Aborting...");
                board_rgb_animate(RGB_MODE_BLINK, 32, 16, 0);
                vTaskDelay(pdMS_TO_TICKS(800));
                app_set_state(UI_STATE_IDLE);
                break;
            default:
                break;
            }
        }

        if (ev & WAKE_WORD_BIT) {
            app_reset_activity_timer();
            ESP_LOGI(TAG, "Wake word detected!");
            ui_state_t cur = ui_get_state();
            /* Allow wake word from IDLE, RESPONSE, or PLAYING_MP3 states.
             * MP3 playback should be interruptible by wake word for voice control. */
            if (cur == UI_STATE_IDLE || cur == UI_STATE_RESPONSE || cur == UI_STATE_PLAYING_MP3) {
                /* CRITICAL: Stop TTS immediately to free internal RAM for STT */
                if (tts_is_playing()) {
                    ESP_LOGI(TAG, "Stopping TTS due to wake word");
                    tts_stop();
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                
                /* If in MP3 playback mode, stop it first */
                if (cur == UI_STATE_PLAYING_MP3) {
                    ESP_LOGI(TAG, "Stopping MP3 playback due to wake word");
                    mp3_player_stop();
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                
                wake_word_pause();
                voice_chat_start();
                wake_word_resume();
            }
        }

        if (ev & DETAILS_BIT) {
            app_reset_activity_timer();
            if (openclaw_get_state() == OPENCLAW_STATE_CONNECTED) {
                ESP_LOGI(TAG, "Requesting details...");
                app_set_state(UI_STATE_SENDING);
                openclaw_chat_send_details(app_on_chat_response);
            }
        }

        if (ev & WEBSERVER_TOGGLE_BIT) {
            handle_webserver_toggle();
        }

        if (ev & CAMERA_BIT) {
            app_reset_activity_timer();
            ESP_LOGI(TAG, "Camera capture requested");
            /* Lazy init: initialize camera on first use */
            if (!camera_is_ready()) {
                ui_set_status_message("Starting camera...");
                ret = camera_init();
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(ret));
                    ui_set_status_message("Camera not available");
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    if (ui_get_state() == UI_STATE_BOOT) app_set_state(UI_STATE_IDLE);
                    continue;
                }
            }
            if (!camera_is_ready()) {
                ui_set_status_message("Camera not ready");
                vTaskDelay(pdMS_TO_TICKS(1500));
            } else {
                ui_set_status_message("Capturing...");
                uint8_t *jpeg = NULL;
                size_t jpeg_sz = 0;
                esp_err_t ret = camera_capture_jpeg(&jpeg, &jpeg_sz);
                if (ret == ESP_OK && jpeg && jpeg_sz > 0) {
                    // Store for next voice chat
                    if (g_pending_jpeg) free(g_pending_jpeg);
                    g_pending_jpeg = jpeg;
                    g_pending_jpeg_size = jpeg_sz;
                    ESP_LOGI(TAG, "Image captured: %d bytes, pending for next voice chat", (int)jpeg_sz);

                    // Camera image stored for next voice chat
                    ui_set_status_message("Ready (talk to send)");
                } else {
                    ESP_LOGE(TAG, "Camera capture failed: %s", esp_err_to_name(ret));
                    ui_set_status_message("Capture failed");
                    vTaskDelay(pdMS_TO_TICKS(1500));
                }
            }
            if (ui_get_state() == UI_STATE_BOOT) {
                app_set_state(UI_STATE_IDLE);
            }
        }

        /* Periodic MP3 UI update (when visible and playing) */
        if (ui_mp3_ui_is_visible()) {
            mp3_playback_state_t st = mp3_player_get_state();
            if (st == MP3_STATE_PLAYING || st == MP3_STATE_PAUSED) {
                const char *cur = mp3_player_get_current_file();
                uint32_t pos = mp3_player_get_position_sec();
                uint32_t dur = mp3_player_get_duration_sec();
                int progress = (dur > 0) ? (int)(pos * 100 / dur) : 0;
                ui_mp3_ui_show(cur ? cur : "", progress, pos, dur, 
                              (st == MP3_STATE_PLAYING));
            }
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
            ui_set_webserver_status(true);
            settings_get_mutable()->webserver_enabled = true;
            settings_save();
#if BOARD_HAS_DISPLAY
            /* Show IP on screen for 5 seconds */
            char msg[80];
            snprintf(msg, sizeof(msg), "Go to\n%s", ip);
            ui_set_status_message(msg);

            /* Also speak it for convenience */
            char audio_msg[128];
            snprintf(audio_msg, sizeof(audio_msg), "网页服务器已启动，请访问 %s", ip);
            speak_announcement(audio_msg);

            vTaskDelay(pdMS_TO_TICKS(5000));
            if (ui_get_state() == UI_STATE_IDLE || ui_get_state() == UI_STATE_BOOT) {
                app_set_state(UI_STATE_IDLE);
            }
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
        ui_set_webserver_status(false);
        settings_get_mutable()->webserver_enabled = false;
        settings_save();
    }
}
