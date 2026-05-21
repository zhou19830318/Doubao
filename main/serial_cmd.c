/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
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
 *   AIWearable> help             # at the prompt (logs scroll alongside)
 *   AIWearable> quiet            # suppress ESP_LOG* noise
 *   AIWearable> logs             # re-enable ESP_LOG* output
 *   AIWearable> status           # show device state
 *   AIWearable> exit             # (not needed, just press Ctrl+D or reboot)
 */

#include "serial_cmd.h"
#include "app_state.h"
#include "settings.h"

#include "board.h"
#include "openclaw_client.h"
#include "wifi_manager.h"
#include "ui.h"
#include "mp3_player.h"
#include "tts_client.h"
#include "notes_manager.h"
#include "app_tasks.h"

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
    printf("UI=%d  OC=%d  WiFi=%s  Heap=%lu  Quiet=%s\n",
           ui_get_state(), openclaw_get_state(),
           wifi_manager_get_ip(),
           (unsigned long)esp_get_free_heap_size(),
           s_quiet ? "ON" : "OFF");
    const openclaw_info_t *info = openclaw_get_info();
    if (info && info->has_tasks) {
        printf("Tasks: %s\n", info->task_summary);
        for (int i = 0; i < info->task_count; i++) {
            printf("  [%d] %s  id=%s  enabled=%d  running=%d  last=%s  err=%s\n",
                   i, info->tasks[i].name, info->tasks[i].id,
                   info->tasks[i].enabled, info->tasks[i].running,
                   info->tasks[i].last_status, info->tasks[i].last_error);
        }
    }
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
    printf("║            AIWearable Serial Commands            ║\n");
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
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  DISPLAY:                                   ║\n");
    printf("║    wake / w        — Wake up display        ║\n");
    printf("║    deepsleep       — Enter deep sleep       ║\n");
    printf("║    reboot          — Restart device         ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  NETWORK:                                   ║\n");
    printf("║    wifi <ssid> <pwd> — Set & connect WiFi   ║\n");
    printf("║    web             — Toggle webserver       ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  SYSTEM:                                    ║\n");
    printf("║    status / s      — Show device status     ║\n");
    printf("║    tasks           — Open tasks screen      ║\n");
    printf("║    camera / cam    — Capture & send image   ║\n");
    printf("║    quiet / q       — Toggle log suppression ║\n");
    printf("║    cron-add-test   — Add test cron job      ║\n");
    printf("║    cron-remove <id> — Remove cron job       ║\n");
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
    openclaw_chat_abort();
    app_set_state(UI_STATE_IDLE);
    return 0;
}

static int cmd_camera(int argc, char **argv)
{
    printf("Capturing camera...\n");
    xEventGroupSetBits(g_app_events, CAMERA_BIT);
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

static int cmd_say(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: say <message>\n");
        return 1;
    }
    if (openclaw_get_state() != OPENCLAW_STATE_CONNECTED) {
        printf("Error: OpenClaw not connected.\n");
        return 1;
    }
    app_set_state(UI_STATE_SENDING);
    printf("Sending: %s\n", argv[1]);

    esp_err_t ret = notes_manager_save_message("user", argv[1], 0);
    if (ret != ESP_OK) {
        printf("Warning: failed to save to notes (%s)\n", esp_err_to_name(ret));
    }

    if (g_pending_jpeg && g_pending_jpeg_size > 0) {
        printf("(with image %d bytes)\n", (int)g_pending_jpeg_size);
        openclaw_chat_send_with_image(argv[1], g_pending_jpeg, g_pending_jpeg_size, app_on_chat_response);
        free(g_pending_jpeg);
        g_pending_jpeg = NULL;
        g_pending_jpeg_size = 0;
    } else {
        openclaw_chat_send(argv[1], app_on_chat_response);
    }
    return 0;
}

static int cmd_cron_add_test(int argc, char **argv)
{
    printf("Creating test cron job...\n");
    openclaw_cron_add("AIWearable Test",
                      "10000",
                      "Check the current time and say it.");
    vTaskDelay(pdMS_TO_TICKS(1000));
    openclaw_request_tasks();
    return 0;
}

static int cmd_cron_remove(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: cron-remove <id>\n");
        return 1;
    }
    printf("Removing cron job: %s\n", argv[1]);
    openclaw_cron_remove(argv[1]);
    vTaskDelay(pdMS_TO_TICKS(1000));
    openclaw_request_tasks();
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

    /* Set prompt — "AIWearable> " */
    repl_config.prompt = "AIWearable> ";
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

    /* Register all AIWearable commands (handles help, h, ? internally) */
    const esp_console_cmd_t cmds[] = {
        { .command = "help",    .help = "Show AIWearable command reference",   .func = &cmd_help },
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
        { .command = "camera",  .help = "Capture & send camera image",     .func = &cmd_camera },
        { .command = "cam",     .help = NULL,                               .func = &cmd_camera },
        { .command = "cron-add-test", .help = "Add a test cron job",       .func = &cmd_cron_add_test },
        { .command = "cron-remove",   .help = "Remove cron: cron-remove <id>", .func = &cmd_cron_remove },
        { .command = "mp3",     .help = "Open MP3 player UI",              .func = &cmd_mp3 },
        { .command = "mp3list", .help = "List MP3 files on SD card",       .func = &cmd_mp3list },
        { .command = "mp3play", .help = "Play MP3: mp3play <filename>",    .func = &cmd_mp3play },
        { .command = "mp3stop", .help = "Stop MP3 playback",               .func = &cmd_mp3stop },
        { .command = "mp3pause", .help = "Pause MP3 playback",             .func = &cmd_mp3pause },
        { .command = "mp3resume", .help = "Resume MP3 playback",           .func = &cmd_mp3resume },
    };

    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_console_cmd_register(&cmds[i]);
    }

    /* Start REPL task — this handles stdin, line editing, and dispatch */
    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    ESP_LOGI(TAG, "Serial CLI started — type 'help' or press Enter");
}
