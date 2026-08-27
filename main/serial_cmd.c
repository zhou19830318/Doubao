/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * Serial CLI — ESP-IDF esp_console-based interactive command line.
 *
 * Uses the official esp_console REPL (linenoise line editor) which correctly
 * handles stdin, echo, backspace, history, and command dispatch on all
 * console backends (UART, USB-SERIAL-JTAG, USB-CDC).
 *
 * To use:
 *   idf.py monitor               # or picocom / minicom at 115200 baud
 *   Doubao> help              # at the prompt (logs scroll alongside)
 *   Doubao> quiet             # suppress ESP_LOG* noise
 *   Doubao> logs              # re-enable ESP_LOG* output
 *   Doubao> status            # show device state
 *   Doubao> exit              # (not needed, just press Ctrl+D or reboot)
 */

#include "serial_cmd.h"
#include "app_state.h"
#include "settings.h"

#include "board.h"
#include "board_loopback_test.h"   // 临时测试模块，Task 12 Step 5 删除
#include "proto_test.h"            // 临时测试模块，Task 5 的 proto test 命令
#include "doubao_voice.h"          // Task 6 测试命令: doubao connect/disconnect/status
/* 本地密钥（git 忽略）— 仅 CLI 测试注入；Task 7 正式接线。
 * fresh checkout 可能没有 secrets.h，必须编译守卫（同 app_main.c）。 */
#if __has_include("secrets.h")
#include "secrets.h"
#ifdef SECRETS_DOUBAO_API_KEY
#define AIDB_DEV_API_KEY SECRETS_DOUBAO_API_KEY
#else
#define AIDB_DEV_API_KEY ""
#endif
#else
#define AIDB_DEV_API_KEY ""
#endif
// TODO(Task 7/8): 由 doubao 链路替换 — removed openclaw_client.h/tts_client.h (components deleted in Task 1)
#include "wifi_manager.h"
#include "ui.h"
#include "mp3_player.h"
#include "notes_manager.h"
#include "app_tasks.h"
#include "esp_lvgl_port.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "linenoise/linenoise.h"

static const char *TAG = "cli";

/* ── Log suppression ────────────────────────────────────────────────── */
static bool          s_quiet          = false;
static vprintf_like_t s_orig_vprintf  = NULL;

static int custom_vprintf(const char *fmt, va_list args)
{
    if (s_quiet) return 0;
    return s_orig_vprintf(fmt, args);
}

/* ── Command handlers ───────────────────────────────────────────────── */

static int cmd_status(int argc, char **argv)
{
    // TODO(Task 8): 由 doubao 链路替换 — openclaw_get_state/openclaw_get_info deleted in Task 1
    // printf("UI=%d  OC=%d  WiFi=%s  Heap=%lu  Quiet=%s\n",
    //        ui_get_state(), openclaw_get_state(),
    //        wifi_manager_get_ip(),
    //        (unsigned long)esp_get_free_heap_size(),
    //        s_quiet ? "ON" : "OFF");
    printf("UI=%d  WiFi=%s  Heap=%lu  Quiet=%s\n",
           ui_get_state(),
           wifi_manager_get_ip(),
           (unsigned long)esp_get_free_heap_size(),
           s_quiet ? "ON" : "OFF");
    // const openclaw_info_t *info = openclaw_get_info();
    // if (info && info->has_tasks) {
    //     printf("Tasks: %s\n", info->task_summary);
    //     for (int i = 0; i < info->task_count; i++) {
    //         printf("  [%d] %s  id=%s  enabled=%d  running=%d  last=%s  err=%s\n",
    //                i, info->tasks[i].name, info->tasks[i].id,
    //                info->tasks[i].enabled, info->tasks[i].running,
    //                info->tasks[i].last_status, info->tasks[i].last_error);
    //     }
    // }
    return 0;
}

static int cmd_quiet(int argc, char **argv)
{
    s_quiet = !s_quiet;
    printf("Log output %s\n", s_quiet ? "SUPPRESSED" : "ENABLED");
    return 0;
}

static int cmd_help(int argc, char **argv)
{
    /* esp_console already has a built-in "help" command, but we override
       to show our custom list alongside the registered commands. */
    printf("\n");
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║            Doubao Serial Commands             ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  CHAT:                                      ║\n");
    printf("║    talk / t        — Start voice chat       ║\n");
    printf("║    say <msg>       — Send text to AI        ║\n");
    printf("║    abort           — Abort current chat     ║\n");
    printf("║    details / d     — Request full details   ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  AUDIO:                                     ║\n");
    printf("║    play / p        — Read last response     ║\n");
    printf("║    mp3             — Open MP3 player UI     ║\n");
    printf("║    mp3list         — List MP3 files on SD   ║\n");
    printf("║    mp3play <file>  — Play file from /mp3/   ║\n");
    printf("║    mp3stop         — Stop playback          ║\n");
    printf("║    mp3pause        — Pause playback         ║\n");
    printf("║    mp3resume       — Resume paused playback ║\n");
    printf("║    audio loop      — Full-duplex loopback   ║\n");
    printf("║    audio rec       — Record 100ms, mic RMS  ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  DISPLAY:                                   ║\n");
    printf("║    wake / w        — Wake up display        ║\n");
    printf("║    deepsleep       — Enter deep sleep       ║\n");
    printf("║    reboot          — Restart device         ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  NETWORK:                                   ║\n");
    printf("║    wifi <ssid> <pwd> — Set & connect WiFi   ║\n");
    printf("║    web             — Toggle webserver       ║\n");
    printf("║    doubao connect  — Connect Doubao voice   ║\n");
    printf("║    doubao status   — Show WSS/session state ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  SYSTEM:                                    ║\n");
    printf("║    status / s      — Show device status     ║\n");
    printf("║    tasks           — Open tasks screen      ║\n");
    printf("║    quiet / q       — Toggle log suppression ║\n");
    printf("║    cron-add-test   — Add test cron job      ║\n");
    printf("║    cron-remove <id> — Remove cron job       ║\n");
    printf("║    proto test      — Doubao codec self-test ║\n");
    printf("║    help / h / ?    — Show this help         ║\n");
    printf("╚══════════════════════════════════════════════╝\n");
    printf("\nFor built-in help (all registered commands):  help\n");
    return 0;
}

static int cmd_talk(int argc, char **argv)
{
    printf("OK, starting voice chat...\n");
    xEventGroupSetBits(g_app_events, KNOB_PRESSED_BIT);
    return 0;
}

static int cmd_play(int argc, char **argv)
{
    printf("Playing last response...\n");
    xEventGroupSetBits(g_app_events, TTS_PLAY_BIT);
    return 0;
}

static int cmd_details(int argc, char **argv)
{
    printf("Requesting full details...\n");
    xEventGroupSetBits(g_app_events, DETAILS_BIT);
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: wifi <ssid> <password>\n");
        return 1;
    }
    settings_t *cfg = settings_get_mutable();
    strncpy(cfg->wifi_ssid, argv[1], sizeof(cfg->wifi_ssid) - 1);
    cfg->wifi_ssid[sizeof(cfg->wifi_ssid) - 1] = '\0';
    strncpy(cfg->wifi_password, argv[2], sizeof(cfg->wifi_password) - 1);
    cfg->wifi_password[sizeof(cfg->wifi_password) - 1] = '\0';
    settings_save();
    printf("WiFi SSID='%s', attempting reconnect...\n", cfg->wifi_ssid);
    wifi_manager_reconnect(cfg->wifi_ssid, cfg->wifi_password);
    return 0;
}

static int cmd_web(int argc, char **argv)
{
    printf("Toggling webserver...\n");
    xEventGroupSetBits(g_app_events, WEBSERVER_TOGGLE_BIT);
    return 0;
}

static int cmd_tasks(int argc, char **argv)
{
    printf("Opening tasks screen...\n");
    xEventGroupSetBits(g_app_events, TASKS_SCREEN_BIT);
    return 0;
}

static int cmd_abort(int argc, char **argv)
{
    printf("Aborting chat...\n");
    // TODO(Task 8): 由 doubao 链路替换 — openclaw_chat_abort deleted in Task 1
    // openclaw_chat_abort();
    app_set_state(UI_STATE_IDLE);
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    printf("Rebooting now...\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0; /* never reached */
}

static int cmd_deepsleep(int argc, char **argv)
{
    printf("Entering deep sleep...\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    app_enter_deep_sleep();
    return 0; /* never reached */
}

static int cmd_wake(int argc, char **argv)
{
    app_reset_activity_timer();
    printf("Display woken.\n");
    return 0;
}

static int cmd_mp3(int argc, char **argv)
{
    printf("Opening MP3 player...\n");
    xEventGroupSetBits(g_app_events, MP3_PLAYER_BIT);
    return 0;
}

static int cmd_mp3list(int argc, char **argv)
{
    char **files = NULL;
    uint16_t count = 0;
    esp_err_t ret = mp3_player_scan_sd_dynamic("/sdcard/mp3", &files, &count);
    if (ret != ESP_OK || !files) {
        printf("No MP3 files found on SD card.\n");
    } else {
        printf("--- MP3 Files (%d) ---\n", count);
        for (int i = 0; i < count; i++) {
            printf("  [%d] %s\n", i, files[i]);
            free(files[i]);
        }
        free(files);
    }
    return 0;
}

static int cmd_mp3play(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: mp3play <filename>\n");
        return 1;
    }
    printf("Playing: %s\n", argv[1]);
    mp3_player_play(argv[1]);
    return 0;
}

static int cmd_mp3stop(int argc, char **argv)
{
    mp3_player_stop();
    printf("MP3 stopped.\n");
    return 0;
}

static int cmd_mp3pause(int argc, char **argv)
{
    mp3_player_pause();
    printf("MP3 paused.\n");
    return 0;
}

static int cmd_mp3resume(int argc, char **argv)
{
    mp3_player_resume();
    printf("MP3 resumed.\n");
    return 0;
}

/* ── Temporary M1 full-duplex audio test (Task 12 Step 5 删除) ──────────── */

static int cmd_audio(int argc, char **argv)
{
    const char *sub = (argc >= 2) ? argv[1] : "help";

    if (strcmp(sub, "loop") == 0) {
        const char *act = (argc >= 3) ? argv[2] : NULL;
        if (act && strcmp(act, "start") == 0) {
            esp_err_t ret = board_loopback_start();
            printf("Audio loopback %s (440Hz sine + mic record)\n",
                   ret == ESP_OK ? "STARTED" : "FAILED to start");
            return ret == ESP_OK ? 0 : 1;
        }
        if (act && strcmp(act, "stop") == 0) {
            board_loopback_stop();
            printf("Audio loopback stopped.\n");
            return 0;
        }
        if (!act) {   /* no sub-action → toggle */
            if (board_loopback_is_running()) {
                board_loopback_stop();
                printf("Audio loopback stopped.\n");
            } else {
                esp_err_t ret = board_loopback_start();
                printf("Audio loopback %s (440Hz sine + mic record)\n",
                       ret == ESP_OK ? "STARTED" : "FAILED to start");
                return ret == ESP_OK ? 0 : 1;
            }
            return 0;
        }
        printf("Usage: audio loop [start|stop]\n");
        return 1;
    }

    if (strcmp(sub, "rec") == 0) {
        esp_err_t ret = board_loopback_record_rms();
        if (ret != ESP_OK) printf("audio rec failed: %s\n", esp_err_to_name(ret));
        return ret == ESP_OK ? 0 : 1;
    }

    printf("Usage: audio loop [start|stop]  |  audio rec\n");
    return 1;
}

static int cmd_say(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: say <message>\n");
        return 1;
    }

    /* Task 7 文本链路：say <文本> → speech_text_buffer 直推。
     * WS 未连接时先异步 connect（文本丢弃，提示稍后重试）。 */
    if (!doubao_is_connected()) {
        printf("Doubao not connected — connecting... (retry the message in a moment)\n");
        doubao_connect();
        return 0;
    }

    /* 用户气泡 + 状态（REPL 任务线程调 UI 必须持 LVGL 锁） */
    lvgl_port_lock(0);
    ui_add_user_bubble(argv[1]);
    app_set_state(UI_STATE_SENDING);
    lvgl_port_unlock();

    /* 对话记录落盘（SD 慢操作，不持锁） */
    esp_err_t ret = notes_manager_save_message("user", argv[1], 0);
    if (ret != ESP_OK) {
        printf("Warning: failed to save to notes (%s)\n", esp_err_to_name(ret));
    }

    ret = doubao_push_text(argv[1]);
    if (ret == ESP_OK) {
        printf("Sent: %s\n", argv[1]);
        return 0;
    }
    printf("Push failed: %s\n", esp_err_to_name(ret));
    return 1;
}

static int cmd_cron_add_test(int argc, char **argv)
{
    printf("Creating test cron job...\n");
    // TODO(Task 8): 由 doubao 链路替换 — openclaw_cron_add/openclaw_request_tasks deleted in Task 1
    // openclaw_cron_add("AIWatch Test",
    //                   "10000",
    //                   "Check the current time and say it.");
    // vTaskDelay(pdMS_TO_TICKS(1000));
    // openclaw_request_tasks();
    return 0;
}

/* ── Doubao WSS test commands (Task 6; 真机验收入口) ───────────────────── */

static int cmd_doubao(int argc, char **argv)
{
    const char *sub = (argc >= 2) ? argv[1] : "help";

    if (strcmp(sub, "connect") == 0) {
        doubao_cfg_t cfg = {
            .api_key = AIDB_DEV_API_KEY,
            .voice = "zh_female_vv_jupiter_bigtts",
            .instructions = "你是一个桌面上放置的语音助手，用简洁的中文回答。",
            .speed = 0,
            .loudness = 0,
        };
        esp_err_t ret = doubao_init(&cfg, NULL);
        if (ret != ESP_OK) {
            printf("doubao init failed: %s\n", esp_err_to_name(ret));
            return 1;
        }
        ret = doubao_connect();
        printf("doubao connect: %s — 等待日志出现 session.created 且 session.id=<id>\n",
               ret == ESP_OK ? "OK" : esp_err_to_name(ret));
        return ret == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "disconnect") == 0) {
        esp_err_t ret = doubao_disconnect();
        printf("doubao disconnect: %s\n", ret == ESP_OK ? "OK" : esp_err_to_name(ret));
        return ret == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "status") == 0) {
        const char *sid = doubao_get_session_id();
        printf("doubao: %s  session.id=%s\n",
               doubao_is_connected() ? "CONNECTED" : "DISCONNECTED",
               sid ? sid : "(none)");
        return 0;
    }
    printf("Usage: doubao connect | disconnect | status\n");
    return 1;
}

static int cmd_cron_remove(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: cron-remove <id>\n");
        return 1;
    }
    printf("Removing cron job: %s\n", argv[1]);
    // TODO(Task 8): 由 doubao 链路替换 — openclaw_cron_remove/openclaw_request_tasks deleted in Task 1
    // openclaw_cron_remove(argv[1]);
    // vTaskDelay(pdMS_TO_TICKS(1000));
    // openclaw_request_tasks();
    return 0;
}

/* ── Initialization ─────────────────────────────────────────────────── */

void serial_cmd_task_start(void)
{
    /* Install custom vprintf hook so "quiet" can suppress ESP_LOG* */
    s_orig_vprintf = esp_log_set_vprintf(custom_vprintf);

    /* Minimal console init: the UART VFS was set up by the IDF boot code,
       but we also install the esp_console REPL for proper line editing. */

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();

    /* Set prompt — "Doubao> " */
    repl_config.prompt = "Doubao> ";
    repl_config.max_cmdline_length = 256;
    repl_config.task_stack_size = 4096;
    repl_config.task_priority = 3;

    /* Use whichever console backend is configured */
#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl));
#else
#error "No supported console backend is enabled (CONFIG_ESP_CONSOLE_UART_DEFAULT / USB_SERIAL_JTAG / USB_CDC)"
#endif

    /* Register all Doubao commands (handles help, h, ? internally) */
    const esp_console_cmd_t cmds[] = {
        { .command = "help",    .help = "Show Doubao command reference",    .func = &cmd_help },
        { .command = "h",       .help = NULL,                               .func = &cmd_help },
        { .command = "?",       .help = NULL,                               .func = &cmd_help },
        { .command = "status",  .help = "Show device status",              .func = &cmd_status },
        { .command = "s",       .help = NULL,                               .func = &cmd_status },
        { .command = "quiet",   .help = "Toggle ESP_LOG* suppression",     .func = &cmd_quiet },
        { .command = "q",       .help = NULL,                               .func = &cmd_quiet },
        { .command = "talk",    .help = "Start voice chat",                .func = &cmd_talk },
        { .command = "t",       .help = NULL,                               .func = &cmd_talk },
        { .command = "say",     .help = "Send text to AI: say <message>",  .func = &cmd_say },
        { .command = "abort",   .help = "Abort current chat",              .func = &cmd_abort },
        { .command = "details", .help = "Request full details",            .func = &cmd_details },
        { .command = "d",       .help = NULL,                               .func = &cmd_details },
        { .command = "play",    .help = "Read last response aloud",        .func = &cmd_play },
        { .command = "p",       .help = NULL,                               .func = &cmd_play },
        { .command = "wake",    .help = "Wake up display",                 .func = &cmd_wake },
        { .command = "w",       .help = NULL,                               .func = &cmd_wake },
        { .command = "deepsleep", .help = "Enter deep sleep",              .func = &cmd_deepsleep },
        { .command = "reboot",  .help = "Restart the device",              .func = &cmd_reboot },
        { .command = "restart", .help = NULL,                               .func = &cmd_reboot },
        { .command = "wifi",    .help = "Set WiFi: wifi <ssid> <pass>",    .func = &cmd_wifi },
        { .command = "web",     .help = "Toggle webserver",                .func = &cmd_web },
        { .command = "tasks",   .help = "Open tasks screen",               .func = &cmd_tasks },
        { .command = "cron-add-test", .help = "Add a test cron job",       .func = &cmd_cron_add_test },
        { .command = "cron-remove",   .help = "Remove cron: cron-remove <id>", .func = &cmd_cron_remove },
        { .command = "mp3",     .help = "Open MP3 player UI",              .func = &cmd_mp3 },
        { .command = "mp3list", .help = "List MP3 files on SD card",       .func = &cmd_mp3list },
        { .command = "mp3play", .help = "Play MP3: mp3play <filename>",    .func = &cmd_mp3play },
        { .command = "mp3stop", .help = "Stop MP3 playback",               .func = &cmd_mp3stop },
        { .command = "mp3pause", .help = "Pause MP3 playback",             .func = &cmd_mp3pause },
        { .command = "mp3resume", .help = "Resume MP3 playback",           .func = &cmd_mp3resume },
        { .command = "audio", .help = "Audio test (M1 temp): audio loop [start|stop], audio rec", .func = &cmd_audio },
        { .command = "doubao", .help = "Doubao WSS test: doubao connect|disconnect|status", .func = &cmd_doubao },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_console_cmd_register(&cmds[i]);
    }

    /* Doubao protocol codec self-test (Task 5, temp) */
    proto_test_register();

    /* Start REPL task — this handles stdin, line editing, and dispatch */
    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    ESP_LOGI(TAG, "Serial CLI started — type 'help' or press Enter");
}
