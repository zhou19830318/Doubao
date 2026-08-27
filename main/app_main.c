/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * Doubao Voice Robot (ESP32-S3)
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
#include "cJSON.h"
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
#include "notes_manager.h"
#include "ui.h"
#include "ui_tasks.h"
#include "ui_mp3_ui.h"
#include "mp3_player.h"
#include "webserver.h"
#include "wake_word.h"
#include "app_state_machine.h"
#include "doubao_voice.h"
#include "doubao_chat.h"

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

/* Forward-declare webserver toggle handler */
static void handle_webserver_toggle(void);

/* ── TTS announcement (inline, no one-shot task) ──────────────────────── */
/* Previous design: xTaskCreateStaticPinnedToCore + vTaskDelete(NULL)
 * caused LoadProhibited in prvSelectHighestPriorityTaskSMP — the idle
 * task on Core 1 never cleaned up the static TCB before the scheduler
 * re-encountered the dangling list entry.  Calling inline eliminates
 * the race entirely (doubao_push_text is non-blocking; it just queues
 * JSON into the WS TX buffer). */
static void speak_announcement(const char *text)
{
    /* Announcements are best-effort with the doubao link: speech_text
     * requires an OPEN session, and sessions only exist during a voice
     * turn (per-conversation model). At boot / config time there is no
     * session (and often no connection yet) — skip quietly instead of
     * raising an error that feeds the consecutive-error deep-sleep
     * counter. The wake_word_pause/resume pair is gone too: push only
     * queues JSON, and an unconditional resume here would hand the mic
     * back to the wake-word task mid-conversation. */
    esp_err_t err = doubao_push_text(text);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "TTS announce sent: %s", text);
    } else {
        ESP_LOGI(TAG, "TTS announce skipped (%s): %s",
                 esp_err_to_name(err), text);
    }
}

/* ─── Doubao voice wiring (Task 6b: api_key web 配置) ─────────────────── */

/* cfg 指向 settings 静态字符串即可（doubao_init 内部深拷贝，无需 malloc） */
static void doubao_init_from_settings(void)
{
    const settings_t *cfg = settings_get();
    const char *key = cfg->api_key[0] ? cfg->api_key : AIDB_DEV_API_KEY;
    if (!key[0]) {
        ESP_LOGW(TAG, "Doubao API key 未配置（settings 为空且无 secrets.h dev 值）— 连接将报 AUTH 错误");
    }

    /* Voice must be a voice name ("zh_female_vv_jupiter_bigtts"), NOT a
     * URL. A past config entry stuffed the old MiMo endpoint URL into the
     * voice field (NVS "db_voice"), and the server accepted the session
     * while never producing a single ASR/response event — silently
     * swallowing a bad required field. Guard here and on every
     * re-init (this fn also runs on web config change). */
    static const char *default_voice = "zh_female_vv_jupiter_bigtts";
    const char *voice = (cfg->voice[0] && strstr(cfg->voice, "://") == NULL)
                        ? cfg->voice : NULL;
    if (voice == NULL) {
        if (cfg->voice[0]) {
            ESP_LOGW(TAG, "voice='%s' is not a voice name — falling back to "
                          "zh_female_vv_jupiter_bigtts", cfg->voice);
        }
        voice = default_voice;
        /* Write the correct default back to NVS so the warning doesn't
         * repeat on every boot. Without this, a cleared field stays empty
         * and triggers the fallback log on every power cycle. */
        settings_t *mutable_cfg = settings_get_mutable();
        strncpy(mutable_cfg->voice, default_voice, sizeof(mutable_cfg->voice) - 1);
        mutable_cfg->voice[sizeof(mutable_cfg->voice) - 1] = '\0';
        settings_save();
    }

    /* Build instructions: user prompt + device command guide.
     * The device command guide is ALWAYS appended so that even old NVS
     * prompts (written before device commands were added) get the
     * [DEVICE:...] capability. Without this, existing devices that
     * upgraded firmware keep their old prompt and the LLM never
     * outputs device commands. */
    static const char *dev_cmd_guide =
        "\n"
        "## 设备控制指令\n"
        "当用户要求调节音量时，回复中必须包含 [DEVICE:volume=0-100]\n"
        "当用户要求调节亮度时，回复中必须包含 [DEVICE:brightness=0-100]\n"
        "当用户要求停止音乐时，回复中必须包含 [DEVICE:mp3=stop]\n"
        "当用户要求暂停音乐时，回复中必须包含 [DEVICE:mp3=pause]\n"
        "当用户要求调节灯光时，回复中必须包含 [DEVICE:rgb=rainbow/aurora/fire/ocean/off/on]\n"
        "示例：用户说'音量调到50'，你回复'好的，音量已调到50%'并包含 [DEVICE:volume=50]\n"
        "注意：[DEVICE:...]指令单独一行，不要放在句子中间。"
        "\n"
        "## 音乐播放指令（重要！）\n"
        "下方有SD卡中的MP3曲目列表，格式为 序号:歌名。\n"
        "当用户要求播放音乐时：\n"
        "1. 如果用户指定了歌名或歌手，从列表中找到最匹配的歌曲，用 [DEVICE:mp3=index:N] 播放（N是序号）\n"
        "2. 如果用户没有指定具体歌曲（如'播放音乐'），用 [DEVICE:mp3=show] 显示曲目列表让用户选择\n"
        "3. 如果找不到匹配的歌曲，用 [DEVICE:mp3=show] 显示列表并告知用户\n"
        "示例：用户说'播放邓紫棋的歌'，你在列表中找到邓紫棋的歌曲，回复'正在播放邓紫棋的《喜欢你》'并包含 [DEVICE:mp3=index:15]\n"
        "示例：用户说'播放第一首'，回复'正在播放'并包含 [DEVICE:mp3=index:1]\n"
        "示例：用户说'播放音乐'，回复'已为你调出曲目列表'并包含 [DEVICE:mp3=show]\n";
    char instructions_buf[4096];
    if (cfg->system_prompt[0]) {
        snprintf(instructions_buf, sizeof(instructions_buf), "%s%s",
                 cfg->system_prompt, dev_cmd_guide);
    } else {
        snprintf(instructions_buf, sizeof(instructions_buf),
                 "你是一个桌面语音助手，简洁中文回答。%s", dev_cmd_guide);
    }
    /* Append SD card MP3 song list so the AI knows exact indices.
     * Without this, the AI cannot map user requests like "播放邓紫棋的歌"
     * to the correct [DEVICE:mp3=index:N] command. */
    const char *mp3_list = app_get_sd_mp3_list_str();
    if (mp3_list && mp3_list[0]) {
        size_t cur_len = strlen(instructions_buf);
        size_t remaining = sizeof(instructions_buf) - cur_len - 2;
        if (remaining > 80) {
            snprintf(instructions_buf + cur_len, remaining,
                     "\n\n## SD卡曲目列表（序号:歌名）\n%s", mp3_list);
        }
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
    /* Task 7: 事件回调 → doubao_chat（气泡/状态机/指令/落盘）；WSS 任务上下文 */
    esp_err_t ret = doubao_init(&dc, doubao_chat_on_event);
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

/* ─── Boot-time internal DRAM attribution ─────────────────────────────
 * Internal DRAM is the scarce resource on this board (PSRAM sits ~4.5MB
 * free while internal runs out). Absolute numbers at the end of boot say
 * nothing about *who* ate it, so each probe reports the delta since the
 * previous one — the stage with the big negative delta is the culprit.
 * Logged at WARN so it survives a raised log level. */
static size_t s_heap_prev = 0;

static void heap_probe(const char *stage)
{
    size_t now     = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t dma     = heap_caps_get_free_size(MALLOC_CAP_DMA);
    long   delta   = (s_heap_prev == 0) ? 0 : (long)now - (long)s_heap_prev;
    ESP_LOGW(TAG, "HEAP[%-12s] internal=%6u largest=%6u dma=%6u delta=%+ld",
             stage, (unsigned)now, (unsigned)largest, (unsigned)dma, delta);
    s_heap_prev = now;
}

/* ── cJSON PSRAM hooks (set early, before webserver can serve requests) */
static void *cjson_psram_malloc(size_t sz) {
    return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void cjson_psram_free(void *p) {
    heap_caps_free(p);
}

/* ─── Main ───────────────────────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Doubao v%s", APP_VERSION_STRING);
    ESP_LOGI(TAG, "  Doubao Voice Robot");
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
    /* SPI DMA buffer allocation errors during LCD refresh are expected
     * when internal RAM is tight — suppress to avoid log flood. */
    esp_log_level_set("spi_master", ESP_LOG_NONE);      /* suppress SPI DMA alloc errors */
    esp_log_level_set("lcd_panel.io.spi", ESP_LOG_NONE); /* suppress LCD SPI errors */

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
    /* doubao api_key: settings 优先，空回落 SECRETS_DOUBAO_API_KEY */
    defaults.volume = APP_SPEAKER_VOLUME;
    settings_init(&defaults);

    /* Set cJSON global hooks to use PSRAM early — before the webserver
     * can handle any requests. Without this, the first notes/files page
     * load uses default internal RAM malloc for cJSON nodes, which
     * exhausts internal DRAM → "setup_dma_priv_buffer: Failed to allocate
     * priv TX buffer" ×8 (SPI LCD DMA stall). protocol.c also sets these
     * hooks, but only after doubao_init() which runs later. */
    {
        cJSON_Hooks hooks = { .malloc_fn = cjson_psram_malloc,
                              .free_fn = cjson_psram_free };
        cJSON_InitHooks(&hooks);
    }

    heap_probe("baseline");

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
    heap_probe("board_init");

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
    heap_probe("sdcard");

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
    heap_probe("mp3_player");

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
    heap_probe("ui_init");
    /* MP3 player UI is initialized by ui_init() via ui_mp3_ui_init() */
    /* Post-init UI operations — LVGL lock required (main task != LVGL task) */
    lvgl_port_lock(0);
    app_state_request(UI_STATE_BOOT);
    ui_set_status_message("Starting...");
    lvgl_port_unlock();
    
    /* Show boot screen for at least 2 seconds */
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
    heap_probe("wifi");

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
        heap_probe("app_tasks");

        /* Doubao voice: api_key 走 settings（web 配置），为空回落 dev 值。
         * Task 6b 接线；自动连接策略与完整事件回调在 Task 7/8。 */
        doubao_init_from_settings();
        webserver_set_doubao_changed_cb(on_doubao_api_key_changed);
        heap_probe("doubao_init");

        /* Init wake word detection (non-critical — continue if fails) */
        ret = wake_word_init();
        if (ret == ESP_OK) {
            wake_word_start(g_app_events, WAKE_WORD_BIT);
            ESP_LOGI(TAG, "Wake word: \"%s\"", wake_word_get_phrase());
        } else {
            ESP_LOGW(TAG, "Wake word init failed — voice wake disabled");
        }
        heap_probe("wake_word");

        /* Scan SD card for MP3 files so the AI knows what's available */
        app_sd_mp3_scan_init();
        heap_probe("mp3_scan");

        /* Initialize notes manager for chat history storage */
        notes_manager_init();
        heap_probe("notes");
    } else {
        ESP_LOGI(TAG, "AP mode - skipping OpenClaw connect, background tasks, and wake word");
    }
    
    /* Serial command task always runs (useful for debugging in AP mode) */
    serial_cmd_task_start();
    heap_probe("serial_cli");

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
                                              MP3_PLAYER_BIT | MP3_CMD_BIT |
                                              SETTINGS_LONG_PRESS_BIT | DOUBLE_TAP_BIT |
                                              DOUBAO_START_BIT,
                                              pdTRUE, pdFALSE,
                                              pdMS_TO_TICKS(100));

        if (ev & DOUBLE_TAP_BIT) {
            /* Double tap: interrupt playback / dismiss response → idle.
             * SKIP when MP3 selection was just confirmed — the panel's
             * double-tap handler already queued the play command. The
             * global gesture detector also fires on the same touch; without
             * this check it would interrupt and force IDLE, racing the play. */
            if (ui_mp3_ui_consume_selection_confirmed()) {
                ESP_LOGD(TAG, "Double tap suppressed (MP3 selection confirmed)");
            } else {
                app_reset_activity_timer();
                ESP_LOGI(TAG, "Double tap — interrupt/stop");
                doubao_interrupt();
                g_continue_listening = false;
                lvgl_port_lock(0);
                app_state_request(UI_STATE_IDLE);
                lvgl_port_unlock();
            }
        }

        if (ev & SETTINGS_LONG_PRESS_BIT) {
            /* Long press: open light settings page */
            app_reset_activity_timer();
            ESP_LOGI(TAG, "Long press — opening settings");
            /* Force interrupt if in dialogue */
            ui_state_t cur = ui_get_state();
            if (cur == UI_STATE_TTS_PLAYING || cur == UI_STATE_THINKING ||
                cur == UI_STATE_STREAMING || cur == UI_STATE_LISTENING) {
                doubao_interrupt();
            }
            lvgl_port_lock(0);
            ui_settings_show();
            lvgl_port_unlock();
        }

        if (ev & TOUCH_BIT) {
            app_reset_activity_timer();
            /* Single tap from gesture detector */
            if (ui_settings_is_visible()) {
                /* Tap on settings page → ignore (use back button) */
            } else if (g_recording) {
                ESP_LOGI(TAG, "Touch during recording — cancelling");
                xEventGroupSetBits(g_app_events, CANCEL_BIT);
            } else if (ui_get_state() == UI_STATE_RESPONSE) {
                if (g_tts_pending) {
                    ESP_LOGD(TAG, "Touch during RESPONSE ignored — TTS pending");
                } else {
                    ESP_LOGI(TAG, "Touch dismissed response");
                    g_response_shown_at = 0;
                    lvgl_port_lock(0);
                    app_state_request(UI_STATE_IDLE);
                    lvgl_port_unlock();
                }
            } else if (ui_get_state() == UI_STATE_IDLE && !app_is_sleeping()) {
                /* Single tap during IDLE → start dialogue (spec §5.2) */
                ESP_LOGI(TAG, "Touch start dialogue");
                board_play_tick();
                /* 叮咚：播放触摸确认音，然后进入 LISTENING。 */
                doubao_chat_play_wake_feedback();
                /* app_set_state, NOT app_state_request: the doubao path
                 * bypasses the legacy machine (its s_current_state goes
                 * stale), so a request would be rejected ("7 -> 5 not
                 * allowed") and the turn never starts. start_listening
                 * pauses the wake word itself. */
                lvgl_port_lock(0);
                app_set_state(UI_STATE_LISTENING);
                lvgl_port_unlock();
                doubao_chat_start();
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
                /* 叮咚：播放按键确认音，然后进入 LISTENING。 */
                doubao_chat_play_wake_feedback();
                /* app_set_state (not app_state_request): the legacy machine
                 * state is stale on the doubao path and rejects the
                 * request ("7 -> 5 not allowed") — start_listening pauses
                 * the wake word itself. */
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
                /* 聆听中单击 BOOT → 取消本轮回 IDLE（咚叮提示，
                 * go_idle 恢复唤醒词；Ver2.0 同款交互） */
                doubao_chat_cancel();
                break;
            case UI_STATE_THINKING:
            case UI_STATE_STREAMING:
                /* Abort the current chat operation — full cleanup via the
                 * same cancel path (capture/session/wake word + 咚叮).
                 * The old path (interrupt + raw app_set_state(IDLE)) left
                 * the wake word paused — device went deaf. */
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
            /* Allow wake word from IDLE, RESPONSE, or PLAYING_MP3 states. */
            /* Allow wake word from IDLE, RESPONSE, PLAYING_MP3, and TTS states.
             * During TTS: interrupt and go to LISTENING (barge-in). */
            if (cur == UI_STATE_IDLE || cur == UI_STATE_RESPONSE ||
                cur == UI_STATE_PLAYING_MP3 ||
                cur == UI_STATE_TTS_PLAYING || cur == UI_STATE_TTS_LOADING) {
                /* Stop conflicting subsystems before requesting LISTENING state */
                if (cur == UI_STATE_TTS_PLAYING || cur == UI_STATE_TTS_LOADING) {
                    ESP_LOGI(TAG, "Stopping TTS due to wake word (barge-in)");
                    doubao_interrupt();
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                if (cur == UI_STATE_PLAYING_MP3) {
                    ESP_LOGI(TAG, "Stopping MP3 playback due to wake word");
                    mp3_player_stop();
                    vTaskDelay(pdMS_TO_TICKS(100));
                }

                /* 叮咚：播放唤醒确认音，然后进入 LISTENING。
                 * play_wake_feedback 阻塞 ~200ms；start_listening() 检测
                 * 到标志后跳过重复播放。 */
                doubao_chat_play_wake_feedback();

                /* app_set_state (not app_state_request): the legacy machine
                 * state is stale on the doubao path and rejects the
                 * request ("7 -> 5 not allowed") — the wake-word barge-in
                 * must always reach start_listening, which pauses the
                 * wake word itself. */
                lvgl_port_lock(0);
                app_set_state(UI_STATE_LISTENING);
                lvgl_port_unlock();
                doubao_chat_start();
            }
        }

        if (ev & DOUBAO_START_BIT) {
            app_reset_activity_timer();
            /* doubao_chat_start() sets DOUBAO_START_BIT;
             * call start_listening() to begin mic capture + VAD */
            extern void doubao_chat_start_listening(void);
            doubao_chat_start_listening();
        }

        if (ev & WEBSERVER_TOGGLE_BIT) {
            handle_webserver_toggle();
        }

        /* ── doubao_chat periodic tick (VAD, timeouts, interrupt detect) ── */
        doubao_chat_tick();

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
