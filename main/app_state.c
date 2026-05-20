/*
 * SPDX-FileCopyrightText: 2024-2026 AIClaw Contributors
 * SPDX-License-Identifier: MIT
 *
 * App state helpers — LED mapping, state transitions, shared globals
 */

#include "app_state.h"
#include "board.h"
#include "ui.h"
#include "ui_state_gif.h"
#include "ui_mp3_ui.h"
#include "mp3_player.h"
#include "openclaw_client.h"
#include "settings.h"
#include "webserver.h"
#include "tts_client.h"
#include "notes_manager.h"
#include "esp_lvgl_port.h"  /* For lvgl_port_lock/unlock */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static const char *TAG = "app_state";

/* Globals */
EventGroupHandle_t g_app_events = NULL;
bool g_recording = false;
int64_t g_response_shown_at = 0;
uint8_t *g_pending_jpeg = NULL;
size_t   g_pending_jpeg_size = 0;
bool g_continue_listening = false;
volatile bool g_tts_pending = false;

/* Deferred MP3 command queue (avoids stack overflow in websocket_task) */
static char s_mp3_cmd_buf[128] = {0};
static bool s_mp3_cmd_pending = false;

/* SD card MP3 file list cache — scanned at boot and refreshed on demand.
 * Included in chat system prefix so the AI knows available files.
 * Uses dynamic allocation to support unlimited MP3 files. */
static char s_sd_mp3_list[2048] = {0};  /* Increased buffer for more files */
static int  s_sd_mp3_count = 0;
static char **s_sd_mp3_names = NULL;  /* Dynamically allocated array */
static bool s_sd_scanned = false;

/* Song selection mode state */
static int s_selected_song_index = 0;

void app_queue_mp3_cmd(const char *cmd_with_args)
{
    /* Safety check: ensure event group is initialized */
    if (!g_app_events) {
        ESP_LOGW(TAG, "Event group not initialized, queuing MP3 command failed");
        return;
    }
    
    strncpy(s_mp3_cmd_buf, cmd_with_args, sizeof(s_mp3_cmd_buf) - 1);
    s_mp3_cmd_buf[sizeof(s_mp3_cmd_buf) - 1] = '\0';
    s_mp3_cmd_pending = true;
    xEventGroupSetBits(g_app_events, MP3_CMD_BIT);
}

/* Sanitize a string to ensure valid UTF-8 for JSON transport.
 * Non-UTF-8 bytes (e.g. OEM codepage chars from FAT 8.3 names) are
 * replaced with '?'. Returns the sanitized string (same buffer). */
static char *utf8_sanitize(char *s)
{
    for (char *p = s; *p; ) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x80) {
            p++;  /* ASCII — always valid */
        } else if ((c & 0xE0) == 0xC0) {
            /* 2-byte sequence */
            if (((unsigned char)p[1] & 0xC0) != 0x80) { *p = '?'; p++; }
            else p += 2;
        } else if ((c & 0xF0) == 0xE0) {
            /* 3-byte sequence */
            if (((unsigned char)p[1] & 0xC0) != 0x80 ||
                ((unsigned char)p[2] & 0xC0) != 0x80) { *p = '?'; p++; }
            else p += 3;
        } else if ((c & 0xF8) == 0xF0) {
            /* 4-byte sequence */
            if (((unsigned char)p[1] & 0xC0) != 0x80 ||
                ((unsigned char)p[2] & 0xC0) != 0x80 ||
                ((unsigned char)p[3] & 0xC0) != 0x80) { *p = '?'; p++; }
            else p += 4;
        } else {
            *p = '?'; p++;  /* Invalid lead byte */
        }
    }
    return s;
}

/* Scan SD card for MP3 files and cache the list.
 * Called at boot and on explicit [DEVICE:mp3=scan] commands. */
static void sd_mp3_cache_refresh(void)
{
    /* Free previous allocation if exists */
    if (s_sd_mp3_names) {
        for (int i = 0; i < s_sd_mp3_count; i++) {
            free(s_sd_mp3_names[i]);
        }
        free(s_sd_mp3_names);
        s_sd_mp3_names = NULL;
    }

    /* Use dynamic scanning to support unlimited files */
    esp_err_t ret = mp3_player_scan_sd_dynamic("/sdcard/mp3", &s_sd_mp3_names, (uint16_t *)&s_sd_mp3_count);
    if (ret != ESP_OK || !s_sd_mp3_names) {
        s_sd_mp3_count = 0;
        s_sd_scanned = true;
        s_sd_mp3_list[0] = '\0';
        return;
    }

    s_sd_scanned = true;

    /* Build compact list string for system prefix.
     * Sanitize each filename for display: FAT 8.3 short names may contain
     * non-UTF-8 OEM bytes (codepage 437) that cause WebSocket close 1007.
     * Original names in s_sd_mp3_names are kept intact for file operations. */
    s_sd_mp3_list[0] = '\0';
    if (s_sd_mp3_count > 0) {
        int off = 0;
        for (int i = 0; i < s_sd_mp3_count && off < (int)sizeof(s_sd_mp3_list) - 80; i++) {
            char safe_name[MP3_FILE_NAME_MAX];
            strncpy(safe_name, s_sd_mp3_names[i], sizeof(safe_name) - 1);
            safe_name[sizeof(safe_name) - 1] = '\0';
            utf8_sanitize(safe_name);
            off += snprintf(s_sd_mp3_list + off, sizeof(s_sd_mp3_list) - off,
                           "%s%d:%s", i > 0 ? ", " : "", i + 1, safe_name);
        }
    }
}

void app_sd_mp3_scan_init(void)
{
    sd_mp3_cache_refresh();
    if (s_sd_mp3_count > 0) {
        ESP_LOGI(TAG, "SD MP3 scan: %d files found on boot", s_sd_mp3_count);
    }
}

const char *app_get_sd_mp3_list_str(void)
{
    return s_sd_mp3_list;
}

int app_get_sd_mp3_count(void)
{
    return s_sd_mp3_count;
}

const char *app_get_sd_mp3_name(int index)
{
    if (index < 0 || index >= s_sd_mp3_count || !s_sd_mp3_names) return NULL;
    return s_sd_mp3_names[index];
}

void app_sd_mp3_cleanup(void)
{
    if (s_sd_mp3_names) {
        for (int i = 0; i < s_sd_mp3_count; i++) {
            free(s_sd_mp3_names[i]);
        }
        free(s_sd_mp3_names);
        s_sd_mp3_names = NULL;
        s_sd_mp3_count = 0;
        ESP_LOGI(TAG, "MP3 file list cleaned up");
    }
}

void app_process_mp3_cmd(void)
{
    if (!s_mp3_cmd_pending) return;
    s_mp3_cmd_pending = false;

    const char *val = s_mp3_cmd_buf;
    ESP_LOGI(TAG, "MP3 cmd (deferred): %s", val);

    /* Stop any ongoing TTS before MP3 operations that need audio hardware.
     * Both share the same I2S output and internal heap — running them
     * simultaneously causes heap exhaustion (Mem alloc fail). */
    if (tts_is_playing()) {
        ESP_LOGI(TAG, "Stopping TTS before MP3 command");
        tts_stop();
        g_tts_pending = false;
    }

    if (strncmp(val, "play:", 5) == 0) {
        esp_err_t ret = mp3_player_play(val + 5);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "MP3 play: %s", val + 5);
            /* Set UI state to PLAYING_MP3 so wake word detection is properly handled */
            app_set_state(UI_STATE_PLAYING_MP3);
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "MP3 file not found: %s", val + 5);
        } else {
            ESP_LOGE(TAG, "MP3 play failed: %s (err=%d)", val + 5, ret);
        }
    } else if (strcmp(val, "stop") == 0) {
        mp3_player_stop();
        app_set_state(UI_STATE_IDLE);
    } else if (strcmp(val, "pause") == 0) {
        mp3_player_pause();
    } else if (strcmp(val, "resume") == 0) {
        mp3_player_resume();
    } else if (strcmp(val, "show") == 0) {
        /* Enter song selection mode */
        if (s_sd_mp3_count > 0) {
            ui_mp3_ui_enter_selection_mode((const char **)s_sd_mp3_names, s_sd_mp3_count, &s_selected_song_index);
            ESP_LOGI(TAG, "MP3 selection mode: %d songs available", s_sd_mp3_count);
        } else {
            ESP_LOGW(TAG, "No MP3 files found on SD card");
            ui_mp3_ui_show(NULL, 0, 0, 0, false);
        }
    } else if (strncmp(val, "index:", 6) == 0) {
        /* Play by index: [DEVICE:mp3=index:N] plays the N-th file (1-based) */
        int idx = atoi(val + 6) - 1;
        if (idx >= 0 && idx < s_sd_mp3_count) {
            esp_err_t ret = mp3_player_play(s_sd_mp3_names[idx]);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "MP3 play index %d: %s", idx + 1, s_sd_mp3_names[idx]);
                /* Set UI state to PLAYING_MP3 so wake word detection is properly handled */
                app_set_state(UI_STATE_PLAYING_MP3);
            } else {
                ESP_LOGE(TAG, "MP3 play index %d failed: err=%d", idx + 1, ret);
            }
        } else {
            ESP_LOGW(TAG, "MP3 index %d out of range (have %d files)", idx + 1, s_sd_mp3_count);
        }
    } else if (strcmp(val, "scan") == 0 || strcmp(val, "list") == 0) {
        /* Refresh cache and show on screen */
        sd_mp3_cache_refresh();
        if (s_sd_mp3_count > 0) {
            /* Calculate required buffer size dynamically */
            size_t json_size = 2048 + s_sd_mp3_count * 150;  /* Base + per-file estimate */
            char *list_json = malloc(json_size);
            if (list_json) {
                int off = snprintf(list_json, json_size,
                    "{\"title\":\"SD卡MP3 (%d首)\",\"type\":\"list\",\"data\":[", s_sd_mp3_count);
                for (int i = 0; i < s_sd_mp3_count && off < (int)(json_size - 100); i++) {
                    off += snprintf(list_json + off, json_size - off,
                        "%s\"%d. %s\"", i > 0 ? "," : "", i + 1, s_sd_mp3_names[i]);
                }
                off += snprintf(list_json + off, json_size - off, "]}");
                ESP_LOGI(TAG, "MP3 scan: found %d files", s_sd_mp3_count);
                free(list_json);
            } else {
                ESP_LOGE(TAG, "Failed to allocate memory for MP3 list JSON");
            }
        } else {
            ESP_LOGW(TAG, "MP3 scan: no MP3 files found on SD card");
        }
    } else {
        ESP_LOGW(TAG, "Unknown mp3 cmd: %s", val);
    }
}

/* ── LED helper ──────────────────────────────────────────────────────── */

/* Map startup_pattern setting (0-4) to RGB mode enum */
static board_rgb_mode_t startup_mode(void)
{
    uint8_t pat = settings_get()->startup_pattern;
    switch (pat) {
    case 1:  return RGB_MODE_AURORA;
    case 2:  return RGB_MODE_STARFIELD;
    case 3:  return RGB_MODE_FIRE;
    case 4:  return RGB_MODE_OCEAN;
    default: return RGB_MODE_RAINBOW_SPIN;
    }
}

void app_led_for_state(ui_state_t st)
{
#if BOARD_RGB_LED_COUNT > 1
    /* Multi-LED ring: rich animations */
    switch (st) {
    case UI_STATE_BOOT:
    case UI_STATE_CONNECTING:
        board_rgb_animate(startup_mode(), 0, 0, 0);
        break;
    case UI_STATE_IDLE:
        board_rgb_animate(RGB_MODE_SOLID, 0, 4, 0);
        break;
    case UI_STATE_LISTENING:
        /* Red pulse for recording */
        board_rgb_animate(RGB_MODE_PULSE_WAVE, 40, 0, 0);
        break;
    case UI_STATE_SENDING:
        /* Blue chase for uploading */
        board_rgb_animate(RGB_MODE_CHASE, 0, 16, 40);
        break;
    case UI_STATE_THINKING:
        /* Orange chase for thinking */
        board_rgb_animate(RGB_MODE_CHASE, 40, 24, 0);
        break;
    case UI_STATE_STREAMING:
        /* Purple/Blue sparkle for streaming response */
        board_rgb_animate(RGB_MODE_SPARKLE, 32, 8, 48);
        break;
    case UI_STATE_RESPONSE:
        /* Solid green (slightly brighter than idle) */
        board_rgb_animate(RGB_MODE_SOLID, 0, 8, 0);
        break;
    case UI_STATE_TTS_LOADING:
        /* Cyan blink/chase */
        board_rgb_animate(RGB_MODE_CHASE, 0, 24, 24);
        break;
    case UI_STATE_TTS_PLAYING:
        /* Green/Teal breathe for speaking */
        board_rgb_animate(RGB_MODE_BREATHE, 0, 32, 16);
        break;
    case UI_STATE_NOTIFYING:
        /* Amber breathe for notifications/reminders */
        board_rgb_animate(RGB_MODE_BREATHE, 40, 32, 0);
        break;
    case UI_STATE_ERROR:
        /* Blink red for error */
        board_rgb_animate(RGB_MODE_BLINK, 32, 0, 0);
        break;
    default:
        board_rgb_animate(RGB_MODE_SOLID, 16, 16, 0);
        break;
    }
#else
    /* Single LED: simple modes */
    switch (st) {
    case UI_STATE_IDLE:
        board_rgb_animate(RGB_MODE_SOLID, 0, 4, 0);
        break;
    case UI_STATE_LISTENING:
        board_rgb_animate(RGB_MODE_SOLID, 32, 0, 0);
        break;
    case UI_STATE_SENDING:
        board_rgb_animate(RGB_MODE_SOLID, 0, 0, 32);
        break;
    case UI_STATE_THINKING:
        board_rgb_animate(RGB_MODE_BREATHE, 40, 24, 0);
        break;
    case UI_STATE_STREAMING:
        board_rgb_animate(RGB_MODE_BREATHE, 24, 0, 40);
        break;
    case UI_STATE_RESPONSE:
        board_rgb_animate(RGB_MODE_SOLID, 0, 6, 0);
        break;
    case UI_STATE_TTS_LOADING:
        board_rgb_animate(RGB_MODE_BLINK, 0, 24, 24);
        break;
    case UI_STATE_TTS_PLAYING:
        board_rgb_animate(RGB_MODE_BREATHE, 0, 32, 16);
        break;
    case UI_STATE_NOTIFYING:
        /* Amber blink for single LED */
        board_rgb_animate(RGB_MODE_BLINK, 32, 16, 0);
        break;
    case UI_STATE_ERROR:
        board_rgb_animate(RGB_MODE_BLINK, 32, 0, 0);
        break;
    default:
        board_rgb_animate(RGB_MODE_SOLID, 16, 16, 0);
        break;
    }
#endif
}

void app_set_state(ui_state_t st)
{
    ui_set_state(st);
    app_led_for_state(st);
    
    /* Show GIF animation for the current state */
#if CONFIG_LV_USE_GIF
    /* CRITICAL: Defer GIF update to avoid race condition during boot.
     * WiFi callbacks may trigger state changes before LVGL task queue
     * is fully initialized. The GIF will be shown on next state change. */
    static bool s_lvgl_ready = false;
    if (!s_lvgl_ready) {
        /* Try to lock LVGL - if it fails, LVGL is not ready yet */
        if (lvgl_port_lock(0)) {
            lvgl_port_unlock();
            s_lvgl_ready = true;
            ui_state_gif_show_for_state(st);
        } else {
            ESP_LOGD(TAG, "LVGL not ready, deferring GIF for state %d", st);
        }
    } else {
        ui_state_gif_show_for_state(st);
    }
#endif
}

/* ── Device command parser ───────────────────────────────────────────── */
/* Parses and executes [DEVICE:key=value] commands from OpenClaw response.
 * Strips processed command lines from buf in-place.
 * Returns number of commands executed. */
int parse_device_commands(char *buf)
{
    int count = 0;
    char *pos = buf;

    while ((pos = strstr(pos, "[DEVICE:")) != NULL) {
        char *start = pos;
        
        /* Find the matching closing bracket - handle nested JSON */
        char *end = NULL;
        int brace_depth = 0;
        char *scan = pos + 8;  /* skip "[DEVICE:" */
        
        while (*scan) {
            if (*scan == '{' || *scan == '[') {
                brace_depth++;
            } else if (*scan == '}' || *scan == ']') {
                if (brace_depth == 0) {
                    end = scan;
                    break;
                }
                brace_depth--;
            }
            scan++;
        }
        
        if (!end) {
            ESP_LOGW(TAG, "Device command missing closing bracket");
            break;
        }

        /* Extract key=value from [DEVICE:key=value] */
        const char *kv = pos + 8;  /* skip "[DEVICE:" */
        size_t kv_len = end - kv;
        
        /* Limit command length to prevent buffer overflow */
        if (kv_len >= 4096) {
            ESP_LOGW(TAG, "Device command too long (%d bytes), skipping", kv_len);
            pos = end + 1;
            continue;
        }
        
        char *cmd = malloc(kv_len + 1);
        if (!cmd) {
            ESP_LOGE(TAG, "Failed to allocate memory for device command");
            break;
        }
        memcpy(cmd, kv, kv_len);
        cmd[kv_len] = '\0';

        /* Split key=value */
        char *eq = strchr(cmd, '=');
        const char *key = cmd;
        const char *val = "";
        if (eq) {
            *eq = '\0';
            val = eq + 1;
        }

        ESP_LOGI(TAG, "Device cmd: %s (value len=%d)", key, strlen(val));
        settings_t *cfg = settings_get_mutable();

        if (strcmp(key, "volume") == 0) {
            int v = atoi(val);
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            cfg->volume = (uint8_t)v;
            board_audio_set_volume(v);
            settings_save();
            ESP_LOGI(TAG, "Volume set to %d%%", v);
            count++;
        } else if (strcmp(key, "brightness") == 0) {
            int v = atoi(val);
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            cfg->brightness = (uint8_t)v;
            board_display_set_brightness(v);
            settings_save();
            ESP_LOGI(TAG, "Brightness set to %d%%", v);
            count++;
        } else if (strcmp(key, "rgb") == 0) {
            if (strcmp(val, "off") == 0) {
                cfg->rgb_enabled = false;
                board_rgb_set(0, 0, 0);
                settings_save();
            } else if (strcmp(val, "on") == 0) {
                cfg->rgb_enabled = true;
                settings_save();
                app_led_for_state(ui_get_state());
            } else if (strcmp(val, "rainbow") == 0) {
                board_rgb_animate(RGB_MODE_RAINBOW_SPIN, 0, 0, 0);
            } else if (strcmp(val, "aurora") == 0) {
                board_rgb_animate(RGB_MODE_AURORA, 0, 0, 0);
            } else if (strcmp(val, "starfield") == 0) {
                board_rgb_animate(RGB_MODE_STARFIELD, 0, 0, 0);
            } else if (strcmp(val, "fire") == 0) {
                board_rgb_animate(RGB_MODE_FIRE, 0, 0, 0);
            } else if (strcmp(val, "ocean") == 0) {
                board_rgb_animate(RGB_MODE_OCEAN, 0, 0, 0);
            } else {
                /* Try as R,G,B values e.g. "255,0,128" */
                int r = 0, g = 0, b = 0;
                if (sscanf(val, "%d,%d,%d", &r, &g, &b) == 3) {
                    board_rgb_set((uint8_t)r, (uint8_t)g, (uint8_t)b);
                }
            }
            ESP_LOGI(TAG, "RGB: %s", val);
            count++;
        } else if (strcmp(key, "sleep") == 0) {
            int mins = atoi(val);
            cfg->sleep_timeout_ms = (uint32_t)(mins * 60000);
            settings_save();
            ESP_LOGI(TAG, "Sleep timeout: %d min", mins);
            count++;
        } else if (strcmp(key, "reboot") == 0) {
            ESP_LOGW(TAG, "Reboot requested via voice");
            count++;
            /* Strip command, then reboot after short delay */
            memmove(start, end + 1, strlen(end + 1) + 1);
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        } else if (strcmp(key, "webserver") == 0) {
            if (strcmp(val, "on") == 0 && !webserver_is_running()) {
                xEventGroupSetBits(g_app_events, WEBSERVER_TOGGLE_BIT);
            } else if (strcmp(val, "off") == 0 && webserver_is_running()) {
                xEventGroupSetBits(g_app_events, WEBSERVER_TOGGLE_BIT);
            }
            count++;
        } else if (strcmp(key, "auto_read") == 0) {
            cfg->auto_read_response = (strcmp(val, "on") == 0 || strcmp(val, "true") == 0);
            settings_save();
            count++;
        } else if (strcmp(key, "widget") == 0 || strcmp(key, "card") == 0) {
            /* [DEVICE:widget=JSON] or [DEVICE:card=JSON] - ignore widget/card commands */
            ESP_LOGI(TAG, "Widget/Card command ignored (len=%d)", strlen(val));
            count++;
        } else if (strcmp(key, "mp3") == 0) {
            /* Defer to main task to avoid stack overflow in websocket_task (4KB stack) */
            app_queue_mp3_cmd(val);
            count++;
        } else if (strcmp(key, "chatlog") == 0 && strncmp(val, "query:", 6) == 0) {
            /* Handle chatlog query: read SD card chat history and send to OpenClaw */
            const char *date = val + 6;
            ESP_LOGI(TAG, "Chatlog query: %s", date);
            char *log_text = notes_manager_read_date(date);
            if (openclaw_get_state() == OPENCLAW_STATE_CONNECTED) {
                if (log_text && strlen(log_text) > 0) {
                    char *prompt = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
                    if (prompt) {
                        snprintf(prompt, 4096, "以下是从SD卡读取的 %s 聊天记录，请总结要点：\n\n%s", date, log_text);
                        openclaw_chat_send(prompt, app_on_chat_response);
                        free(prompt);
                    }
                    free(log_text);
                } else {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "没有找到 %s 的聊天记录", date);
                    openclaw_chat_send(msg, app_on_chat_response);
                }
            } else {
                if (log_text) free(log_text);
                ESP_LOGW(TAG, "OpenClaw not connected, cannot process chatlog query");
            }
            count++;
        } else {
            ESP_LOGW(TAG, "Unknown device cmd: %s", key);
        }

        /* Strip the [DEVICE:...] tag from buffer (including trailing newline) */
        char *after = end + 1;
        if (*after == '\n') after++;
        memmove(start, after, strlen(after) + 1);
        pos = start;  /* Continue scanning from same position */
        
        free(cmd);
    }

    return count;
}
void app_on_chat_response(const char *text, bool is_final)
{
    if (is_final) {
        ESP_LOGI(TAG, "Response (%lu ms): %.100s%s",
                 (unsigned long)openclaw_get_thinking_time_ms(),
                 text, strlen(text) > 100 ? "..." : "");

        /* Save assistant response to notes */
        esp_err_t ret = notes_manager_save_message("assistant", text, openclaw_get_thinking_time_ms());
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save assistant message to notes: %s", esp_err_to_name(ret));
        }

        /* Continuous conversation mode: always continue after a response
         * unless explicitly stopped by a button or 15s+5s timeout in voice_chat. */
        g_continue_listening = true;
        ESP_LOGI(TAG, "Continuous conversation mode: active");

        /* Work on a mutable copy so we can strip the tag */
        static char resp_buf[2048];
        strncpy(resp_buf, text, sizeof(resp_buf) - 1);
        resp_buf[sizeof(resp_buf) - 1] = '\0';

        /* Strip [LISTEN] or [END] tag from response text */
        char *tag = strstr(resp_buf, "[LISTEN]");
        if (tag) {
            /* Trim trailing whitespace/newlines before tag */
            char *p = tag;
            while (p > resp_buf && (*(p-1) == '\n' || *(p-1) == '\r' || *(p-1) == ' ')) p--;
            *p = '\0';
        }
        tag = strstr(resp_buf, "[END]");
        if (tag) {
            char *p = tag;
            while (p > resp_buf && (*(p-1) == '\n' || *(p-1) == '\r' || *(p-1) == ' ')) p--;
            *p = '\0';
        }

        /* Parse and execute device commands ([DEVICE:key=value]) */
        int dev_cmds = parse_device_commands(resp_buf);
        if (dev_cmds > 0) {
            ESP_LOGI(TAG, "Executed %d device command(s)", dev_cmds);
        }

        /* Log raw response for debugging widget parsing issues */
        ESP_LOGI(TAG, "Raw resp (%d bytes): %.*s%s",
                 (int)strlen(resp_buf),
                 (int)(strlen(resp_buf) > 500 ? 500 : strlen(resp_buf)),
                 resp_buf,
                 strlen(resp_buf) > 500 ? "..." : "");

/* Auto-detect weather and show detailed card */
        char *temp = strstr(resp_buf, "°C");
        if (!dev_cmds && temp) {
            char location[32] = "Unknown";

            /* Try Chinese "今天XX天气" pattern: extract XX as location */
            char *jintian = strstr(resp_buf, "今天");
            if (jintian && jintian < temp) {
                char *tianqi = strstr(jintian, "天气");
                if (tianqi && tianqi < temp) {
                    char *p = jintian + strlen("今天");
                    int len = tianqi - p;
                    while (len > 0 && (*p == ' ' || *p == ',')) { p++; len--; }
                    while (len > 0 && (p[len-1] == ' ' || p[len-1] == ',')) len--;
                    if (len > 0 && len < 31) {
                        memcpy(location, p, len);
                        location[len] = '\0';
                    }
                }
            }

            /* Try "XX市" city suffix pattern before temperature */
            if (strcmp(location, "Unknown") == 0) {
                char *shi = NULL;
                char *scan = resp_buf;
                while (scan < temp && (shi = strstr(scan, "市")) != NULL && shi < temp) {
                    char *start = shi;
                    int count = 0;
                    while (start > resp_buf && count < 8) {
                        if ((unsigned char)*(start-1) < 0x80) break;
                        start--;
                        count++;
                    }
                    int len = shi - start + strlen("市");
                    if (len >= 3 && len < 31) {
                        memcpy(location, start, len);
                        location[len] = '\0';
                        break;
                    }
                    scan = shi + strlen("市");
                }
            }

            /* Try "XX天气" anywhere before temperature */
            if (strcmp(location, "Unknown") == 0) {
                char *tq = strstr(resp_buf, "天气");
                if (tq && tq < temp) {
                    char *start = tq;
                    while (start > resp_buf && start > tq - 16) {
                        unsigned char c = (unsigned char)*(start-1);
                        if (c < 0x80 && c != ' ') break;
                        start--;
                    }
                    int len = tq - start;
                    if (len > 0 && len < 31) {
                        memcpy(location, start, len);
                        location[len] = '\0';
                    }
                }
            }

            /* Try "XX今天" pattern: city before 今天 (e.g. "常州今天多云") */
            if (strcmp(location, "Unknown") == 0) {
                char *jt = strstr(resp_buf, "今天");
                if (jt && jt > resp_buf && jt < temp) {
                    /* Walk backwards through non-ASCII (CJK) characters before 今天 */
                    char *p = jt;
                    while (p > resp_buf) {
                        if (((unsigned char)*(p-1) & 0x80) == 0) break; /* ASCII stop */
                        p--;
                    }
                    /* Skip forward over any ASCII delimiters */
                    while (p < jt && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',')) p++;
                    int len = jt - p;
                    if (len > 0 && len < 31) {
                        memcpy(location, p, len);
                        location[len] = '\0';
                    }
                }
            }

            /* Try "今天XX" + weather-condition pattern (e.g. "今天常州多云") */
            if (strcmp(location, "Unknown") == 0) {
                char *jt = strstr(resp_buf, "今天");
                if (jt && jt < temp) {
                    char *p = jt + strlen("今天");
                    /* Weather condition keywords that follow location */
                    const char *conds[] = {"多云", "晴", "阴", "雨", "雪", "雾", "风",
                                            "气温", "温度", "白天", "夜间", "傍晚", NULL};
                    char *end = NULL;
                    for (int i = 0; conds[i]; i++) {
                        char *m = strstr(p, conds[i]);
                        if (m && m < temp && (!end || m < end)) end = m;
                    }
                    if (!end) {
                        /* No weather keyword, try comma as boundary */
                        char *c = strchr(p, ',');
                        if (!c || c >= temp) c = strstr(p, "\357\274\214"); /* CJK full-width comma */
                        if (c && c < temp) end = c;
                    }
                    if (end) {
                        int len = end - p;
                        /* Skip leading spaces and CJK full-width comma (UTF-8: EF BC 8C) */
                        while (len > 0 && (*p == ' ' || *p == ',' ||
                               ((unsigned char)*p == 0xEF && (unsigned char)*(p+1) == 0xBC && (unsigned char)*(p+2) == 0x8C))) {
                            if ((unsigned char)*p == 0xEF) { p += 3; len -= 3; }
                            else { p++; len--; }
                        }
                        while (len > 0 && (p[len-1] == ' ' || p[len-1] == ',' ||
                               (len >= 3 && (unsigned char)p[len-3] == 0xEF && (unsigned char)p[len-2] == 0xBC && (unsigned char)p[len-1] == 0x8C))) {
                            if (len >= 3 && (unsigned char)p[len-3] == 0xEF) len -= 3;
                            else len--;
                        }
                        if (len > 0 && len < 31) {
                            memcpy(location, p, len);
                            location[len] = '\0';
                        }
                    }
                }
            }

            // Try format "new york: +21°C" or "tianjin: +17°C"
            char *colon = strstr(resp_buf, ": ");
            if (!colon) colon = strstr(resp_buf, ":");
            if (colon && colon < temp && colon - resp_buf < 35) {
                int len = colon - resp_buf;
                if (len > 0 && len < 31) {
                    memcpy(location, resp_buf, len);
                    location[len] = '\0';
                    // Capitalize first letter of each word, preserving spaces
                    int j = 0;
                    for (int i = 0; location[i]; i++) {
                        if (j == 0 || (i > 0 && location[i-1] == ' ')) {
                            if (location[i] >= 'a' && location[i] <= 'z') {
                                location[j++] = (char)(location[i] - 'a' + 'A');
                            } else {
                                location[j++] = location[i];
                            }
                        } else {
                            location[j++] = location[i];
                        }
                    }
                    location[j] = '\0';
                }
            }
            // Try "in XX" pattern: "in New York it's 21°C"
            if (strcmp(location, "Unknown") == 0) {
                char *in_pos = resp_buf;
                while ((in_pos = strstr(in_pos, "in ")) != NULL) {
                    if (in_pos == resp_buf || *(in_pos-1) == ' ' || *(in_pos-1) == '\n' || *(in_pos-1) == ',' || *(in_pos-1) == '.') {
                        break;
                    }
                    in_pos++;
                }
                if (in_pos) {
                    char *p = in_pos + 3;
                    char *end = strstr(p, "it's");
                    if (!end) end = strstr(p, "is ");
                    if (!end) end = strstr(p, "temperature");
                    if (!end) end = strstr(p, "temp");
                    if (!end) end = strstr(p, "weather");
                    if (!end) end = strstr(p, "气温");
                    if (!end) end = strstr(p, "天气");
                    if (!end) end = strstr(p, "目前");
                    if (!end) end = strstr(p, "当前");
                    if (!end) end = strstr(p, ",");
                    if (!end) end = temp;
                    int len = end - p;
                    // Trim trailing spaces
                    while (len > 0 && (p[len-1] == ' ' || p[len-1] == ',')) len--;
                    if (len > 1 && len < 31) {
                        memcpy(location, p, len);
                        location[len] = '\0';
                    }
                }
            }
            // Try "at XX" pattern: "at New York"
            if (strcmp(location, "Unknown") == 0) {
                char *at_pos = resp_buf;
                while ((at_pos = strstr(at_pos, "at ")) != NULL) {
                    if (at_pos == resp_buf || *(at_pos-1) == ' ' || *(at_pos-1) == '\n' || *(at_pos-1) == ',' || *(at_pos-1) == '.') {
                        break;
                    }
                    at_pos++;
                }
                if (at_pos) {
                    char *p = at_pos + 3;
                    char *end = strstr(p, "it's");
                    if (!end) end = strstr(p, "is ");
                    if (!end) end = strstr(p, "temperature");
                    if (!end) end = strstr(p, "temp");
                    if (!end) end = strstr(p, "weather");
                    if (!end) end = strstr(p, "气温");
                    if (!end) end = strstr(p, "天气");
                    if (!end) end = strstr(p, "目前");
                    if (!end) end = strstr(p, "当前");
                    if (!end) end = strstr(p, ",");
                    if (!end) end = temp;
                    int len = end - p;
                    while (len > 0 && (p[len-1] == ' ' || p[len-1] == ',')) len--;
                    if (len > 1 && len < 31) {
                        memcpy(location, p, len);
                        location[len] = '\0';
                    }
                }
            }

            /* Temperature: extract number before °C */
            char temp_str[16] = "N/A";
            char *t_start = temp;
            while (t_start > resp_buf && (isdigit((unsigned char)*(t_start-1)) || *(t_start-1) == '+' || *(t_start-1) == '-' || *(t_start-1) == '~' || *(t_start-1) == '.')) {
                t_start--;
            }
            int t_len = temp - t_start;
            if (t_len > 0 && t_len < (int)sizeof(temp_str)) {
                memcpy(temp_str, t_start, t_len);
                temp_str[t_len] = '\0';
            }

            /* Humidity - find percentage after 湿度 */
            char hum_str[16] = {0};
            char *hum = strstr(resp_buf, "湿度");
            if (hum) {
                char *p = hum + strlen("湿度");
                while (*p && !isdigit((unsigned char)*p)) p++;
                int i = 0;
                while (*p && isdigit((unsigned char)*p) && i < 10) {
                    hum_str[i++] = *p++;
                }
                if (i > 0) {
                    hum_str[i] = '%';
                    hum_str[i+1] = '\0';
                }
            }

            /* Wind - find after 风速 or 风力 */
            char wind_str[24] = {0};
            char *wind = strstr(resp_buf, "风速");
            if (!wind) wind = strstr(resp_buf, "风力");
            if (wind) {
                const char *kw = (wind == strstr(resp_buf, "风力")) ? "风力" : "风速";
                char *p = wind + strlen(kw);
                while (*p && !isdigit((unsigned char)*p)) p++;
                int i = 0;
                while (*p && (isdigit((unsigned char)*p) || *p == '-' || *p == '~') && i < 8) {
                    wind_str[i++] = *p++;
                }
                if (i > 0) {
                    memcpy(wind_str + i, "级", 4);
                }
            }

            /* Build JSON - only include fields that have real data */
            char json[384];
            char data_part[320];
            int d_off = 0;

            d_off += snprintf(data_part + d_off, sizeof(data_part) - d_off,
                "地区:%s\\n温度:%s°C", location,
                strcmp(temp_str, "N/A") == 0 ? "N/A" : temp_str);
            if (hum_str[0]) {
                d_off += snprintf(data_part + d_off, sizeof(data_part) - d_off,
                    "\\n湿度:%s", hum_str);
            }
            if (wind_str[0]) {
                d_off += snprintf(data_part + d_off, sizeof(data_part) - d_off,
                    "\\n风速:%s", wind_str);
            }

            snprintf(json, sizeof(json),
                "{\"title\":\"天气\",\"data\":\"%s\"}", data_part);
            ESP_LOGI(TAG, "Auto weather: %s", json);
}

        /* Auto-detect todo/task list and show card */
        if (strstr(resp_buf, "待办") || strstr(resp_buf, "任务") || strstr(resp_buf, "todo") || strstr(resp_buf, "task")
            || strstr(resp_buf, "记好") || strstr(resp_buf, "已记") || strstr(resp_buf, "提醒")
            || strstr(resp_buf, "reminder") || strstr(resp_buf, "日程") || strstr(resp_buf, "事项")) {
            char data_buf[512] = {0};
            int offset = 0;
            int has_task = 0;

            char *p = resp_buf;
            char *todo_pos = NULL;
            if (strstr(p, "没有待办") || strstr(p, "暂无待办") || strstr(p, "no todo") || strstr(p, "no task")) {
                snprintf(data_buf, sizeof(data_buf), "暂无待办");
                has_task = 1;
            } else {
                // Many patterns to find task content
                todo_pos = strstr(p, "记好了");
                if (!todo_pos) todo_pos = strstr(p, "已记下");
                if (!todo_pos) todo_pos = strstr(p, "已记录");
                if (!todo_pos) todo_pos = strstr(p, "已添加");
                if (!todo_pos) todo_pos = strstr(p, "待办事项是");
                if (!todo_pos) todo_pos = strstr(p, "待办事项有");
                if (!todo_pos) todo_pos = strstr(p, "待办是");
                if (!todo_pos) todo_pos = strstr(p, "待办：");
                if (!todo_pos) todo_pos = strstr(p, "待办:");
                if (!todo_pos) todo_pos = strstr(p, "待办事项");
                if (!todo_pos) todo_pos = strstr(p, "您的待办");
                if (!todo_pos) todo_pos = strstr(p, "your todo");
                if (!todo_pos) todo_pos = strstr(p, "task is");
                if (!todo_pos) todo_pos = strstr(p, "tasks:");
                if (!todo_pos) todo_pos = strstr(p, "提醒：");
                if (!todo_pos) todo_pos = strstr(p, "提醒:");
                if (!todo_pos) todo_pos = strstr(p, "reminder:");
                if (!todo_pos) todo_pos = strstr(p, "日程：");
                if (!todo_pos) todo_pos = strstr(p, "日程:");
                if (!todo_pos) todo_pos = strstr(p, "1.");
                if (!todo_pos) todo_pos = strstr(p, "1、");
                // First line might be task
                if (!todo_pos) {
                    const char *nl = strchr(p, '\n');
                    if (nl && nl - p < 200 && nl - p > 3) todo_pos = (char*)p;
                }

if (todo_pos) {
                    p = todo_pos;

                    // If we matched a label prefix, skip past it to the actual content
                    const char *labels[] = {
                        "记好了", "已记下", "已记录", "已添加",
                        "待办事项是", "待办事项有", "待办是", "待办：", "待办:",
                        "待办事项", "您的待办", "your todo", "task is", "tasks:",
                        "任务列表：", "任务列表:", "今日待办：", "今日待办:",
                        "提醒：", "提醒:", "reminder:", "日程：", "日程:",
                        NULL
                    };
                    for (int i = 0; labels[i]; i++) {
                        if (strstr(p, labels[i]) == p) {
                            p += strlen(labels[i]);
                            while (*p == ' ' || *p == ':' || *p == '\n' || *p == '\r') p++;
                            // Also skip full-width CJK colon (UTF-8: EF BC 9A)
                            if ((unsigned char)*p == 0xEF && (unsigned char)*(p+1) == 0xBC && (unsigned char)*(p+2) == 0x9A)
                                p += 3;
                            break;
                        }
                    }

                    // Skip list-number prefix if present (e.g., "1.", "1、", "1)")
                    unsigned char *up = (unsigned char*)p;
                    while (*up && (isdigit(*up) || *up == '.' || *up == 0xE2 || *up == 0xE3)) up++;
                    p = (char*)up;

                    if (*p && *p != '\n' && *p != '\r') {
                        has_task = 1;
                        while (*p && offset < 400) {
                            if (*p == '?') break;
                            if (*p == '\n' || *p == '\r') {
                                // Find start of next non-empty line
                                char *next = p + 1;
                                while (*next == ' ' || *next == '\r' || *next == '\n') next++;
                                if (*next == '\0') break;
                                // Numbered, bullet, or CJK list continuations
                                if (!isdigit((unsigned char)*next) &&
                                    *next != '-' && *next != '*' && (unsigned char)*next != 0xe2) break;
                            }
                            if (*p != '\r') data_buf[offset++] = *p;
                            p++;
                        }
                        // Trim trailing whitespace/punctuation/newlines
                        while (offset > 0 && (data_buf[offset-1] == ' ' ||
                               data_buf[offset-1] == '.' || data_buf[offset-1] == ',' ||
                               data_buf[offset-1] == '\n')) offset--;
                        data_buf[offset] = '\0';
                    }
                }
                            }

            if (has_task && offset > 0) {
                /* Strip markdown bold/italic markers from extracted text */
                char clean[512];
                int ci = 0;
                for (int i = 0; data_buf[i] && ci < (int)sizeof(clean) - 1; i++) {
                    if (data_buf[i] == '*' && data_buf[i+1] == '*') { i++; continue; }
                    if (data_buf[i] == '*' || data_buf[i] == '_') continue;
                    clean[ci++] = data_buf[i];
                }
                clean[ci] = '\0';
                if (ci > 0) {
                    char json[600];
                    snprintf(json, sizeof(json),
                        "{\"title\":\"待办事项\",\"data\":\"%s\"}", clean);
                    ESP_LOGI(TAG, "Auto todo: data='%s'", clean);
                }
            }
        }

        /* Parse dual response: first line = short label, rest = spoken response */
        static char short_buf[128];
        const char *long_text = resp_buf;
        const char *nl = strchr(resp_buf, '\n');
        if (nl && (nl - resp_buf) < (int)sizeof(short_buf) - 1) {
            size_t short_len = nl - resp_buf;
            memcpy(short_buf, resp_buf, short_len);
            short_buf[short_len] = '\0';
            long_text = nl + 1;
            while (*long_text == '\n' || *long_text == '\r') long_text++;
            if (*long_text == '\0') long_text = short_buf;
            ESP_LOGI(TAG, "Short: '%s' | Long: '%.80s%s'", short_buf, long_text,
                     strlen(long_text) > 80 ? "..." : "");
        } else {
            strncpy(short_buf, resp_buf, sizeof(short_buf) - 1);
            short_buf[sizeof(short_buf) - 1] = '\0';
        }

        app_set_state(UI_STATE_RESPONSE);
        g_response_shown_at = esp_timer_get_time();
        ui_set_response(short_buf, long_text);

        /* DON'T clear widget - keep it visible until next conversation starts */

        /* Auto-TTS: skip if response contained an MP3 command.
         * MP3 and TTS share I2S audio hardware — running both
         * simultaneously exhausts internal heap and causes crashes. */
        bool has_mp3_cmd = (strstr(text, "[DEVICE:mp3=") != NULL);

        /* Auto-TTS: always on screenless boards, otherwise respect setting */
#if BOARD_HAS_DISPLAY
        const settings_t *cfg = settings_get();
        if (cfg->auto_read_response && !has_mp3_cmd) {
            g_tts_pending = true;
            xEventGroupSetBits(g_app_events, TTS_PLAY_BIT);
        }
#else
        /* No display — always speak the response */
        if (!has_mp3_cmd) {
            g_tts_pending = true;
            xEventGroupSetBits(g_app_events, TTS_PLAY_BIT);
        }
#endif
    }
}

/* ── MP3 list getters for UI selection mode ──────────────────────────── */
const char **sd_mp3_get_list(void)
{
    return (const char **)s_sd_mp3_names;
}

int sd_mp3_get_count(void)
{
    return s_sd_mp3_count;
}

int *sd_mp3_get_selected_index(void)
{
    return &s_selected_song_index;
}
