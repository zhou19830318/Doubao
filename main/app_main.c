/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * Doubao Voice Robot (ESP32-S3)
 *
 * Minimal initialization + main event loop.
 * Core chat only: WiFi → Doubao voice → wake word → button.
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
#include "app_tasks.h"
#include "settings.h"
#include "error_log.h"
#include "cJSON.h"

/* Credentials come from secrets.h (gitignored) or the settings page (NVS).
 * Never hard-code them here: this repository is public. */
#if defined(__has_include)
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#endif

#ifndef SECRETS_WIFI_SSID
#define SECRETS_WIFI_SSID ""
#endif
#ifndef SECRETS_WIFI_PASSWORD
#define SECRETS_WIFI_PASSWORD ""
#endif
#ifndef SECRETS_DOUBAO_API_KEY
#define SECRETS_DOUBAO_API_KEY ""
#endif
#ifndef SECRETS_O20_APP_ID
#define SECRETS_O20_APP_ID ""
#endif
#ifndef SECRETS_O20_ACCESS_KEY
#define SECRETS_O20_ACCESS_KEY ""
#endif

#define AIDB_DEV_WIFI_SSID      SECRETS_WIFI_SSID
#define AIDB_DEV_WIFI_PASSWORD  SECRETS_WIFI_PASSWORD
#define AIDB_DEV_API_KEY        SECRETS_DOUBAO_API_KEY
#define AIDB_DEV_O20_APP_ID     SECRETS_O20_APP_ID
#define AIDB_DEV_O20_ACCESS_KEY SECRETS_O20_ACCESS_KEY

#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

#include "wifi_manager.h"
#include "ui.h"
#include "wake_word.h"
#include "app_state_machine.h"
#include "doubao_voice.h"
#include "doubao_chat.h"
#include "doubao_ws_client.h"

static const char *TAG = "Doubao";

/* ─── WiFi callback ──────────────────────────────────────────────── */
static void on_wifi_state(wifi_state_t state)
{
    if (!g_app_events) return;

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
        ESP_LOGI(TAG, "WiFi failed");
        lvgl_port_lock(0);
        ui_set_wifi_status(false, 0);
        app_set_state(UI_STATE_IDLE);
        ui_set_status_message("WiFi failed!");
        lvgl_port_unlock();
        break;
    default:
        lvgl_port_lock(0);
        ui_set_wifi_status(false, 0);
        lvgl_port_unlock();
        break;
    }
}

/* ─── Doubao voice wiring ─────────────────────────────────────────── */
static void doubao_init_from_settings(void)
{
    const settings_t *cfg = settings_get();
    const char *key = cfg->api_key[0] ? cfg->api_key : AIDB_DEV_API_KEY;
    if (!key[0]) {
        ESP_LOGW(TAG, "Doubao API key not configured");
    }

    static const char *default_voice = "zh_female_xiaohe_uranus_bigtts";
    const char *voice = (cfg->voice[0] && strstr(cfg->voice, "://") == NULL)
                        ? cfg->voice : NULL;
    if (voice == NULL) {
        if (cfg->voice[0]) {
            ESP_LOGW(TAG, "voice='%s' is not a voice name — falling back to default", cfg->voice);
        }
        voice = default_voice;
        settings_t *mutable_cfg = settings_get_mutable();
        strncpy(mutable_cfg->voice, default_voice, sizeof(mutable_cfg->voice) - 1);
        mutable_cfg->voice[sizeof(mutable_cfg->voice) - 1] = '\0';
        settings_save();
    }

    static const char *dev_cmd_guide =
        "\n"
        "## 设备控制\n"
        "音量、亮度由设备端直接识别并执行，你不需要输出任何 [DEVICE:...] 指令标签；"
        "标签会被语音合成念出来，绝对不要输出。\n"
        "当用户要求调节音量或亮度时，用一句简短自然的话确认即可"
        "（例如'好嘞，已经帮你调好啦'），不要追问具体数值。\n";
    char instructions_buf[4096];
    if (cfg->system_prompt[0]) {
        snprintf(instructions_buf, sizeof(instructions_buf), "%s%s",
                 cfg->system_prompt, dev_cmd_guide);
    } else {
        snprintf(instructions_buf, sizeof(instructions_buf),
                 "你是一个机车幽默、说话甜美的台湾女孩。你青春有活力，冰雪聪明，会撒娇发嗲，也会粘人，但该理性时依然理性。你说话带点台湾腔，语气活泼可爱，用词俏皮生动。回答简洁自然，像朋友聊天一样轻松。%s", dev_cmd_guide);
    }

    doubao_cfg_t dc = {
        .api_key = key,
        .voice = voice,
        .instructions = instructions_buf,
        .speed = cfg->speed,
        .loudness = cfg->loudness,
        .enable_search = cfg->enable_search,
        .enable_music = cfg->enable_music,
    };
    ESP_LOGI(TAG, "doubao_init: search=%d music=%d api_key=%s",
             dc.enable_search, dc.enable_music,
             key[0] ? "set" : "EMPTY");

    /* Enable O2.0 binary protocol mode for singing support */
    if (AIDB_DEV_O20_APP_ID[0]) {
        dbws_set_o20_mode(true, AIDB_DEV_O20_APP_ID, AIDB_DEV_O20_ACCESS_KEY);
        ESP_LOGI(TAG, "O2.0 mode enabled: app_id=%s", AIDB_DEV_O20_APP_ID);
    } else {
        ESP_LOGW(TAG, "O2.0 credentials missing — singing disabled");
    }

    esp_err_t ret = doubao_init(&dc, doubao_chat_on_event);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "doubao_init failed: %s", esp_err_to_name(ret));
    }
}

/* ─── cJSON PSRAM hooks ──────────────────────────────────────────── */
static void *cjson_psram_malloc(size_t sz) {
    return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void cjson_psram_free(void *p) {
    heap_caps_free(p);
}

/* ─── Main ───────────────────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Doubao Voice Robot v%s", APP_VERSION_STRING);
    ESP_LOGI(TAG, "  Build: %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, "========================================");

    esp_log_level_set("tca95xx_16", ESP_LOG_WARN);
    esp_log_level_set("esp_io_expander", ESP_LOG_WARN);
    esp_log_level_set("touch_axs5106l", ESP_LOG_WARN);
    esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_WARN);
    esp_log_level_set("FT5x06", ESP_LOG_WARN);
    esp_log_level_set("spi_master", ESP_LOG_NONE);
    esp_log_level_set("lcd_panel.io.spi", ESP_LOG_NONE);

    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGW(TAG, "Woke from deep sleep — cause: %d", wakeup);
    }

    /* Init settings from NVS */
    settings_t defaults = {0};
    strncpy(defaults.wifi_ssid,     AIDB_DEV_WIFI_SSID,     sizeof(defaults.wifi_ssid) - 1);
    strncpy(defaults.wifi_password, AIDB_DEV_WIFI_PASSWORD,  sizeof(defaults.wifi_password) - 1);
    defaults.volume = APP_SPEAKER_VOLUME;
    settings_init(&defaults);

    /* cJSON PSRAM hooks */
    {
        cJSON_Hooks hooks = { .malloc_fn = cjson_psram_malloc,
                              .free_fn = cjson_psram_free };
        cJSON_InitHooks(&hooks);
    }

    /* Auto-generate device key if empty */
    settings_t *cfg_mut = settings_get_mutable();
    if (strlen(cfg_mut->oc_device_key) != 64) {
        ESP_LOGI(TAG, "Generating new device identity...");
        uint8_t seed[32];
        esp_fill_random(seed, 32);
        for (int i = 0; i < 32; i++) {
            sprintf(cfg_mut->oc_device_key + i*2, "%02x", seed[i]);
        }
        settings_save();
    }

    error_log_init();

    const settings_t *cfg = settings_get();

    g_app_events = xEventGroupCreate();
    if (!g_app_events) {
        ESP_LOGE(TAG, "Failed to create event group!");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    app_state_machine_init();
    ui_set_event_group(g_app_events);

    /* Init hardware */
    esp_err_t ret = board_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Board init failed: %s", esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }
    ESP_LOGI(TAG, "Board: %s (%s)", board_get_name(), board_get_mcu());

#ifdef CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,
    };
    esp_pm_configure(&pm_config);
#endif

    board_audio_set_volume(cfg->volume);
    board_display_set_brightness(cfg->brightness);

    /* Init UI */
    ret = ui_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "UI init failed (continuing)");

    lvgl_port_lock(0);
    app_state_request(UI_STATE_BOOT);
    ui_set_status_message("Starting...");
    lvgl_port_unlock();

    vTaskDelay(pdMS_TO_TICKS(2000));

    /* Connect WiFi */
    wifi_manager_init(cfg->wifi_ssid, cfg->wifi_password, on_wifi_state);

    EventBits_t bits = xEventGroupWaitBits(g_app_events, WIFI_CONNECTED_BIT,
                                            pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "WiFi timeout — continuing anyway");
    } else {
        ESP_LOGI(TAG, "WiFi ready");
    }

    /* Wait for SNTP */
    const char* ip = wifi_manager_get_ip();
    bool is_ap_mode = (ip != NULL && strcmp(ip, "192.168.4.1") == 0);

    if (!is_ap_mode) {
        ESP_LOGI(TAG, "Waiting for SNTP...");
        for (int i = 0; i < 60; i++) {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            if (tv.tv_sec > 1700000000) {
                ESP_LOGI(TAG, "Time synced");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    if (!is_ap_mode) {
        vTaskDelay(pdMS_TO_TICKS(500));

        /* Start background tasks */
        app_tasks_start();

        /* Doubao voice init */
        doubao_init_from_settings();

        /* Init wake word detection */
        ret = wake_word_init();
        if (ret == ESP_OK) {
            wake_word_start(g_app_events, WAKE_WORD_BIT);
            ESP_LOGI(TAG, "Wake word: \"%s\"", wake_word_get_phrase());
        } else {
            ESP_LOGW(TAG, "Wake word init failed — voice wake disabled");
        }
    } else {
        ESP_LOGI(TAG, "AP mode - skipping network tasks");
    }

    /* Serial command task removed — chat-only mode */

    ESP_LOGI(TAG, "All tasks started. Ready.");

    /* Transition UI to ready state */
    lvgl_port_lock(0);
    app_set_state(UI_STATE_IDLE);
    lvgl_port_unlock();

    /* ── Main event loop ── */
    int heartbeat = 0;
    while (1) {
        EventBits_t ev = xEventGroupWaitBits(g_app_events,
                                              KNOB_PRESSED_BIT |
                                              WAKE_WORD_BIT |
                                              SETTINGS_LONG_PRESS_BIT | DOUBLE_TAP_BIT |
                                              DOUBAO_START_BIT,
                                              pdTRUE, pdFALSE,
                                              pdMS_TO_TICKS(100));

        if (ev & DOUBLE_TAP_BIT) {
            app_reset_activity_timer();
            ESP_LOGI(TAG, "Double tap — interrupt/stop");
            doubao_interrupt();
            g_continue_listening = false;
            lvgl_port_lock(0);
            app_state_request(UI_STATE_IDLE);
            lvgl_port_unlock();
        }

        if (ev & SETTINGS_LONG_PRESS_BIT) {
            app_reset_activity_timer();
            ESP_LOGI(TAG, "Long press — opening settings");
            ui_state_t cur = ui_get_state();
            if (cur == UI_STATE_TTS_PLAYING || cur == UI_STATE_THINKING ||
                cur == UI_STATE_STREAMING || cur == UI_STATE_LISTENING) {
                doubao_interrupt();
            }
            lvgl_port_lock(0);
            ui_settings_show();
            lvgl_port_unlock();
        }

        if (ev & KNOB_PRESSED_BIT) {
            app_reset_activity_timer();

            if (app_is_sleeping() || app_just_woke()) {
                ESP_LOGI(TAG, "Woke from sleep via event — ignoring action");
                continue;
            }
            ui_state_t cur = ui_get_state();
            switch (cur) {
            case UI_STATE_IDLE:
            case UI_STATE_RESPONSE:
                doubao_chat_play_wake_feedback();
                lvgl_port_lock(0);
                app_set_state(UI_STATE_LISTENING);
                lvgl_port_unlock();
                doubao_chat_start();
                break;
            case UI_STATE_TTS_PLAYING:
            case UI_STATE_TTS_LOADING:
                doubao_interrupt();
                g_continue_listening = false;
                lvgl_port_lock(0);
                app_set_state(UI_STATE_IDLE);
                lvgl_port_unlock();
                break;
            case UI_STATE_LISTENING:
                doubao_chat_cancel();
                break;
            case UI_STATE_THINKING:
            case UI_STATE_STREAMING:
                ESP_LOGI(TAG, "Aborting chat (state=%d)...", cur);
                doubao_chat_cancel();
                break;
            default:
                break;
            }
        }

        if (ev & WAKE_WORD_BIT) {
            app_reset_activity_timer();
            ESP_LOGI(TAG, "Wake word detected!");
            ui_state_t cur = ui_get_state();
            if (cur == UI_STATE_IDLE || cur == UI_STATE_RESPONSE ||
                cur == UI_STATE_TTS_PLAYING || cur == UI_STATE_TTS_LOADING) {
                if (cur == UI_STATE_TTS_PLAYING || cur == UI_STATE_TTS_LOADING) {
                    ESP_LOGI(TAG, "Stopping TTS due to wake word (barge-in)");
                    doubao_interrupt();
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                doubao_chat_play_wake_feedback();
                lvgl_port_lock(0);
                app_set_state(UI_STATE_LISTENING);
                lvgl_port_unlock();
                doubao_chat_start();
            }
        }

        if (ev & DOUBAO_START_BIT) {
            app_reset_activity_timer();
            extern void doubao_chat_start_listening(void);
            doubao_chat_start_listening();
        }

        /* doubao_chat periodic tick */
        doubao_chat_tick();

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
