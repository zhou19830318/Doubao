/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * App state helpers — LED mapping, state transitions, shared globals
 */

#include "app_state.h"
#include "app_state_machine.h"
#include "board.h"
#include "ui.h"
#include "ui_mp3_ui.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_lvgl_port.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

static const char *TAG = "app_state";

/* Globals */
EventGroupHandle_t g_app_events = NULL;
bool g_recording = false;
int64_t g_response_shown_at = 0;
bool g_continue_listening = false;
volatile bool g_tts_pending = false;

/* ── LED helper ──────────────────────────────────────────────────────── */

#if BOARD_HAS_RGB_RING
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
#endif

void app_led_for_state(ui_state_t st)
{
#if BOARD_HAS_RGB_RING
#if BOARD_RGB_LED_COUNT > 1
    switch (st) {
    case UI_STATE_BOOT:
    case UI_STATE_CONNECTING:
        board_rgb_animate(startup_mode(), 0, 0, 0);
        break;
    case UI_STATE_IDLE:
        board_rgb_animate(RGB_MODE_SOLID, 0, 4, 0);
        break;
    case UI_STATE_LISTENING:
        board_rgb_animate(RGB_MODE_PULSE_WAVE, 40, 0, 0);
        break;
    case UI_STATE_SENDING:
        board_rgb_animate(RGB_MODE_CHASE, 0, 16, 40);
        break;
    case UI_STATE_THINKING:
        board_rgb_animate(RGB_MODE_CHASE, 40, 24, 0);
        break;
    case UI_STATE_STREAMING:
        board_rgb_animate(RGB_MODE_SPARKLE, 32, 8, 48);
        break;
    case UI_STATE_RESPONSE:
        board_rgb_animate(RGB_MODE_SOLID, 0, 8, 0);
        break;
    case UI_STATE_TTS_LOADING:
        board_rgb_animate(RGB_MODE_CHASE, 0, 24, 24);
        break;
    case UI_STATE_TTS_PLAYING:
        board_rgb_animate(RGB_MODE_BREATHE, 0, 32, 16);
        break;
    case UI_STATE_NOTIFYING:
        board_rgb_animate(RGB_MODE_BREATHE, 40, 32, 0);
        break;
    case UI_STATE_ERROR:
        board_rgb_animate(RGB_MODE_BLINK, 32, 0, 0);
        break;
    default:
        board_rgb_animate(RGB_MODE_SOLID, 16, 16, 0);
        break;
    }
#else
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
#else
    (void)st;
#endif
}

void app_set_state(ui_state_t st)
{
    app_state_machine_force_current(st);
    ui_set_state(st);
    app_led_for_state(st);
}

/* ── Device command parser ───────────────────────────────────────────── */
int parse_device_commands(char *buf)
{
    int count = 0;
    char *pos = buf;

    while ((pos = strstr(pos, "[DEVICE:")) != NULL) {
        char *start = pos;

        char *end = NULL;
        int brace_depth = 0;
        char *scan = pos + 8;

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

        const char *kv = pos + 8;
        size_t kv_len = end - kv;

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
#if BOARD_HAS_RGB_RING
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
            } else if (strcmp(val, "fire") == 0) {
                board_rgb_animate(RGB_MODE_FIRE, 0, 0, 0);
            } else if (strcmp(val, "ocean") == 0) {
                board_rgb_animate(RGB_MODE_OCEAN, 0, 0, 0);
            } else {
                int r = 0, g = 0, b = 0;
                if (sscanf(val, "%d,%d,%d", &r, &g, &b) == 3) {
                    board_rgb_set((uint8_t)r, (uint8_t)g, (uint8_t)b);
                }
            }
#endif
            ESP_LOGI(TAG, "RGB: %s", val);
            count++;
        } else if (strcmp(key, "reboot") == 0) {
            ESP_LOGW(TAG, "Reboot requested via voice");
            count++;
            memmove(start, end + 1, strlen(end + 1) + 1);
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        } else if (strcmp(key, "sleep") == 0) {
            int mins = atoi(val);
            cfg->sleep_timeout_ms = (uint32_t)(mins * 60000);
            settings_save();
            ESP_LOGI(TAG, "Sleep timeout: %d min", mins);
            count++;
        } else {
            ESP_LOGW(TAG, "Unknown device cmd: %s", key);
        }

        char *after = end + 1;
        if (*after == '\n') after++;
        memmove(start, after, strlen(after) + 1);
        pos = start;

        free(cmd);
    }

    return count;
}

/* ── Local voice control ─────────────────────────────────────────────── */
/* O2.0 synthesizes the model's reply server-side, so any [DEVICE:...]
 * tag in that reply would be spoken aloud. Volume/brightness are
 * therefore driven from the user's own ASR transcript, parsed here. */

enum { VC_TARGET_NONE, VC_TARGET_VOLUME, VC_TARGET_BRIGHTNESS };

static const char *vc_find_first(const char *text,
                                 const char *const *words, size_t n)
{
    const char *best = NULL;
    for (size_t i = 0; i < n; i++) {
        const char *p = strstr(text, words[i]);
        if (p && (!best || p < best)) best = p;
    }
    return best;
}

#define VC_HAS(text, list) \
    vc_find_first((text), (list), sizeof(list) / sizeof((list)[0]))

static int vc_utf8_len(const char *p)
{
    unsigned char c = (unsigned char)*p;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static int vc_cn_digit_at(const char *p)
{
    static const char *const digits[] = {
        "零", "一", "二", "两", "三", "四", "五", "六", "七", "八", "九"
    };
    if ((unsigned char)p[0] < 0x80) return -1;
    for (int i = 0; i < 10; i++) {
        if (strncmp(p, digits[i], 3) == 0) return i;
    }
    return -1;
}

/* Parse one Chinese numeral run at `s` ("五十", "一百二十", ...).
 * Requires 十/百 so 一点/一下 are never mistaken for a level. */
static int vc_parse_cn_level(const char *s, const char **endp)
{
    int total = 0;
    int current = -1;
    bool has_unit = false;
    const char *p = s;

    for (;;) {
        if (strncmp(p, "十", 3) == 0) {
            if (current < 0) current = 1;
            total += current * 10;
            current = -1;
            has_unit = true;
        } else if (strncmp(p, "百", 3) == 0) {
            if (current < 0) current = 1;
            total = (total + current) * 100;
            current = -1;
            has_unit = true;
        } else {
            int d = vc_cn_digit_at(p);
            if (d < 0) break;
            current = d;
        }
        p += 3;
    }

    *endp = p;
    if (!has_unit) return -1;
    if (current >= 0) total += current;
    return total;
}

static int vc_find_cn_level(const char *text)
{
    /* 百分之五十: start scanning after the prefix, otherwise the 百 in
     * 百分之 itself would be parsed as 100. */
    const char *base = text;
    const char *pct = strstr(text, "百分之");
    if (pct) base = pct + 9;   /* strlen("百分之") == 9 */

    for (const char *p = base; *p; ) {
        const char *end = NULL;
        int v = vc_parse_cn_level(p, &end);
        if (v >= 0) {
            /* 十分/十点 are adverbs or clock times, not a level. */
            if (strncmp(end, "分", 3) != 0 && strncmp(end, "点", 3) != 0) {
                return v;
            }
            p = end;
            continue;
        }
        p += vc_utf8_len(p);
    }
    return -1;
}

static int vc_find_ascii_level(const char *text)
{
    for (const char *p = text; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            long v = strtol(p, NULL, 10);
            return (v > 1000) ? -1 : (int)v;
        }
    }
    return -1;
}

int handle_local_voice_control(const char *text)
{
    if (!text || !text[0]) return 0;

    static const char *const vol_words[] = {
        "音量", "声音", "喇叭", "扬声器", "小声", "大声", "响",
        "吵", "静音", "听不清"
    };
    static const char *const bri_words[] = {
        "亮度", "屏幕", "亮", "暗", "刺眼", "看不清", "太黑"
    };
    static const char *const vol_up[] = {
        "太小", "听不清", "大一点", "大一些", "大些", "大点", "大声",
        "调大", "增大", "加大", "提高", "调高", "响一点", "最响", "最大", "最高"
    };
    static const char *const vol_down[] = {
        "太大", "太响", "太吵", "太大声", "小一点", "小一些", "小些", "小点",
        "小声", "调小", "减小", "降低", "调低", "轻一点", "最小", "最低"
    };
    static const char *const bri_up[] = {
        "太暗", "太黑", "看不清", "暗了", "亮一点", "亮一些", "亮些",
        "调亮", "加亮", "提高", "调高", "最亮", "最大", "最高"
    };
    static const char *const bri_down[] = {
        "太亮", "刺眼", "调暗", "暗一点", "暗一些", "暗些", "低一点",
        "调低", "降低", "最暗", "最小", "最低"
    };

    const char *pv = VC_HAS(text, vol_words);
    const char *pb = VC_HAS(text, bri_words);
    int target = VC_TARGET_NONE;
    if (pv && pb)      target = (pv <= pb) ? VC_TARGET_VOLUME : VC_TARGET_BRIGHTNESS;
    else if (pv)       target = VC_TARGET_VOLUME;
    else if (pb)       target = VC_TARGET_BRIGHTNESS;

    int abs_v = vc_find_ascii_level(text);
    if (abs_v < 0) abs_v = vc_find_cn_level(text);

    /* "音量是多少" / "亮度调到几" are questions, not commands. */
    if (abs_v < 0 && (strstr(text, "多少") || strstr(text, "几"))) return 0;

    if (target == VC_TARGET_NONE) {
        if (abs_v >= 0) return 0;      /* a bare number says nothing */
        target = VC_TARGET_VOLUME;     /* "大一点" defaults to volume */
    }

    settings_t *cfg = settings_get_mutable();
    int cur = (target == VC_TARGET_VOLUME) ? cfg->volume : cfg->brightness;
    int lo  = (target == VC_TARGET_VOLUME) ? 0 : 5;
    int new_val;

    if (abs_v >= 0) {
        new_val = abs_v;
    } else if (target == VC_TARGET_VOLUME &&
               (strstr(text, "静音") || strstr(text, "关掉声音") ||
                strstr(text, "关闭声音") || strstr(text, "别出声"))) {
        new_val = 0;
    } else {
        int step = (strstr(text, "一点") || strstr(text, "一些") ||
                    strstr(text, "点") || strstr(text, "些")) ? 10 : 20;
        bool is_vol = (target == VC_TARGET_VOLUME);
        const char *const *down = is_vol ? vol_down : bri_down;
        const char *const *up   = is_vol ? vol_up : bri_up;
        size_t down_n = is_vol ? sizeof(vol_down) / sizeof(vol_down[0])
                               : sizeof(bri_down) / sizeof(bri_down[0]);
        size_t up_n   = is_vol ? sizeof(vol_up) / sizeof(vol_up[0])
                               : sizeof(bri_up) / sizeof(bri_up[0]);
        if (vc_find_first(text, down, down_n)) {
            new_val = cur - step;
        } else if (vc_find_first(text, up, up_n)) {
            new_val = cur + step;
        } else {
            return 0;
        }
    }

    if (new_val < lo) new_val = lo;
    if (new_val > 100) new_val = 100;

    if (target == VC_TARGET_VOLUME) {
        cfg->volume = (uint8_t)new_val;
        board_audio_set_volume(new_val);
        ESP_LOGI(TAG, "Voice control: volume %d%% -> %d%%", cur, new_val);
    } else {
        cfg->brightness = (uint8_t)new_val;
        board_display_set_brightness(new_val);
        ESP_LOGI(TAG, "Voice control: brightness %d%% -> %d%%", cur, new_val);
    }
    settings_save();
    return 1;
}

void app_on_chat_response(const char *text, bool is_final)
{
    if (is_final) {
        ESP_LOGI(TAG, "Response: %.100s%s", text, strlen(text) > 100 ? "..." : "");

        g_continue_listening = true;

        static char resp_buf[2048];
        strncpy(resp_buf, text, sizeof(resp_buf) - 1);
        resp_buf[sizeof(resp_buf) - 1] = '\0';

        char *tag = strstr(resp_buf, "[LISTEN]");
        if (tag) {
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

        int dev_cmds = parse_device_commands(resp_buf);
        if (dev_cmds > 0) {
            ESP_LOGI(TAG, "Executed %d device command(s)", dev_cmds);
        }

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
        } else {
            strncpy(short_buf, resp_buf, sizeof(short_buf) - 1);
            short_buf[sizeof(short_buf) - 1] = '\0';
        }

        lvgl_port_lock(0);
        app_set_state(UI_STATE_RESPONSE);
        ui_set_response(short_buf, long_text);
        lvgl_port_unlock();
        g_response_shown_at = esp_timer_get_time();
    }
}
