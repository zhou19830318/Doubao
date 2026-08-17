/*
 * SPDX-FileCopyrightText: 2024-2026 AIWatch Contri
 tors
 * SPDX-License-Identifier: MIT
 *
 * Main UI module — Portrait (BOARD_LCD_H_RES×BOARD_LCD_V_RES) with Dynamic Island style
 *
 * Layout (top to bottom):
 *   Dynamic Island  →  top pill: WiFi · State · Clock · OC dot
 *   Chinese subtitle →  below island
 *   Main content     →  state-specific centerpiece
 *
 * State content:
 *   BOOT        →  "Booting" large text
 *   CONNECTING  →  "Connecting WiFi..." / "Connecting OpenClaw..."
 *   IDLE        →  Big clock + Chinese date
 *   LISTENING   →  Audio waveform animation + "Listening" text
 *   SENDING     →  "Sending..."
 *   THINKING    →  "AI Thinking..." + detail
 *   STREAMING   →  "Receiving..."
 *   RESPONSE    →  AI response text
 *   TTS_PLAYING →  "Speaking" + animation
 *   ERROR       →  Error message
 *   SLEEP/ARMED →  Minimal / dim
 *   PLAYING_MP3 →  Handled by mp3_ui overlay
 *   NOTIFYING   →  Notification text
 */

#include "ui.h"
#include "ui_colors.h"
#include "ui_state_chip.h"
#include "ui_mp3_ui.h"
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
#include <math.h>

/* Chinese font for date display */
extern const lv_font_t SourceHanSansCN_Medium_16;
/* Custom fonts for IDLE page (copied from Ver2.1) */
extern const lv_font_t ui_font_ClockNum;   /* 120px Akrobat-ExtraBold for clock */
extern const lv_font_t ui_font_DateNum;    /* 24px 思源柔黑体 for date */

static const char *TAG = "ui_main";

/* ══════════════════════════════════════════════════════════════════════════
 * Constants
 * ══════════════════════════════════════════════════════════════════════════ */

#define SCREEN_W            BOARD_LCD_H_RES   /* Board-native portrait width */
#define SCREEN_H            BOARD_LCD_V_RES   /* Board-native portrait height */

/* Layout zones (y coordinates, center-based from LV_ALIGN_CENTER):
 *   Dynamic Island:   y=4..50  (status bar + island pill + Chinese subtitle)
 *   Content top:      y≈68     (end of Dynamic Island area)
 *   Content bottom:   y≈SCREEN_H-12
 *   Content center:   y≈SCREEN_H/2
 */

/* 410x502 screen: 16 bars, 10px wide, 6px gap, max 80px tall.
 * Container = 16*(10+6)-6 = 250 wide, 80+8 = 88 tall. */
#define WAVE_BARS           16
#define WAVE_BAR_W          10
#define WAVE_BAR_GAP        6
#define WAVE_BAR_MAX_H      80
#define WAVE_CONTAINER_W    ((WAVE_BARS * (WAVE_BAR_W + WAVE_BAR_GAP)) - WAVE_BAR_GAP)
#define WAVE_CONTAINER_H    (WAVE_BAR_MAX_H + 8)
#define WAVE_ANIM_PERIOD_MS 40

/* Marquee scroll speed (px/s) for LLM response / STT / TTS text.
 * SLOW by design: 16px CJK glyphs at 10px/s ≈ 0.6 字/s, comfortable to read.
 * Increase to ~20 for faster but still legible, decrease for slower. */
#define SCROLL_SPEED_PX_PER_SEC 10

/* ══════════════════════════════════════════════════════════════════════════
 * Global state
 * ══════════════════════════════════════════════════════════════════════════ */

static lv_obj_t *s_screen = NULL;
static ui_state_t s_state = UI_STATE_BOOT;
static EventGroupHandle_t s_events = NULL;

/* Response storage */
static char s_full_response[4096];

/* Clock update timer */
static esp_timer_handle_t s_clock_timer = NULL;

/* Wave animation timer (only runs during LISTENING state) */
static lv_timer_t *s_wave_timer = NULL;
static float s_wave_phase = 0.0f;

/* ══════════════════════════════════════════════════════════════════════════
 * LVGL widgets — state-specific content
 * ══════════════════════════════════════════════════════════════════════════ */

/* Large centered label for generic states (BOOT, CONNECTING, SENDING, etc.) */
static lv_obj_t *s_center_label = NULL;

/* Detail label below center — secondary info in lower zone of screen */
static lv_obj_t *s_detail_label = NULL;

/* IDLE: big clock + Chinese date */
static lv_obj_t *s_idle_time = NULL;
static lv_obj_t *s_idle_date = NULL;

/* LISTENING: waveform bars */
static lv_obj_t *s_wave_container = NULL;
static lv_obj_t *s_wave_bars[WAVE_BARS];

/* Unified single-line left-scrolling marquee — one physical position shared
 * by LLM response (RESPONSE), STT transcription (SENDING), TTS caption
 * (TTS_PLAYING). States are mutually exclusive, so no overlap is possible. */
static lv_obj_t *s_scroll_label = NULL;

/* ── Chat bubbles (Task 7 minimal text chat) ──────────────────────────── */
/* 滚动容器 + lv_label（LONG_WRAP）；左（机器人/灰）/右（用户/蓝）对齐。
 * 上限 50 条，超出删最旧（铁律 19 防碎片化）。流式优化在 Task 13。 */
static lv_obj_t  *s_bubble_container = NULL;
static lv_obj_t  *s_bot_bubble_label = NULL;   /* 当前机器人气泡的 label */
static char       s_bot_text[4096];            /* 累积回复文本（整段重设） */
static bool       s_last_bubble_is_user = false; /* 最近气泡是否为用户气泡（transcript 更新用） */
static uint32_t   s_bubble_count = 0;
#define BUBBLE_MAX_COUNT 50
#define BUBBLE_PCT_W     70                     /* 气泡宽 = 容器内容宽 70% */

/* ══════════════════════════════════════════════════════════════════════════
 * State labels and Chinese descriptions
 * ══════════════════════════════════════════════════════════════════════════ */

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

/* ── State colors ───────────────────────────────────────────────────────── */
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

/* ══════════════════════════════════════════════════════════════════════════
 * Forward declarations
 * ══════════════════════════════════════════════════════════════════════════ */

static void     update_state_display(ui_state_t state);
static void     handle_state_transition(ui_state_t old_state, ui_state_t new_state);
static esp_err_t start_clock_timer(void);
static void     stop_clock_timer(void);
static void     show_content_for_state(ui_state_t state);
static void     hide_all_content(void);
static void     create_state_widgets(void);
static void     wave_timer_cb(lv_timer_t *timer);
static void     update_idle_clock(void);
static void     show_title(const char *text, const lv_font_t *font);
static void     show_subtitle(const char *text);
static void     normalize_marquee_text(char *buf, size_t buf_size);
static void     set_scroll_text(const char *text, lv_color_t color);

/* ══════════════════════════════════════════════════════════════════════════
 * Waveform Animation
 * ══════════════════════════════════════════════════════════════════════════ */

static void wave_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_wave_container) return;

    s_wave_phase += 0.12f;
    if (s_wave_phase > 6.2832f) s_wave_phase -= 6.2832f;

    for (int i = 0; i < WAVE_BARS; i++) {
        if (!s_wave_bars[i]) continue;

        /* Each bar uses a sine wave with slightly different phase/freq
         * to create a natural audio wave effect. Center bars are taller. */
        float t = s_wave_phase + (float)i * 0.9f;
        float center_bias = 1.0f - 0.3f * fabsf((float)i - (WAVE_BARS - 1) / 2.0f) / ((WAVE_BARS - 1) / 2.0f);
        float height = (sinf(t) * 0.5f + 0.5f) * WAVE_BAR_MAX_H * center_bias;

        if (height < 4.0f) height = 4.0f;
        uint32_t h = (uint32_t)height;

        lv_obj_set_height(s_wave_bars[i], h);
    }
}

static void start_wave_animation(void)
{
    if (s_wave_timer) return;
    s_wave_phase = 0.0f;
    /* Timer repeats indefinitely by default (repeat_count = -1) */
    s_wave_timer = lv_timer_create(wave_timer_cb, WAVE_ANIM_PERIOD_MS, NULL);
}

static void stop_wave_animation(void)
{
    if (s_wave_timer) {
        lv_timer_del(s_wave_timer);
        s_wave_timer = NULL;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Widget Creation
 * ══════════════════════════════════════════════════════════════════════════ */

/* ══════════════════════════════════════════════════════════════════════════
 * Marquee text normalization
 * ══════════════════════════════════════════════════════════════════════════ */

/* Replace newlines with spaces for horizontal scroll (newlines break marquee) */
static void normalize_marquee_text(char *buf, size_t buf_size)
{
    if (!buf) return;
    for (size_t i = 0; i < strlen(buf) && i < buf_size; i++) {
        if (buf[i] == '\n' || buf[i] == '\r') {
            buf[i] = ' ';
        }
    }
    /* Trim trailing spaces */
    size_t len = strlen(buf);
    while (len > 0 && buf[len - 1] == ' ') {
        buf[--len] = '\0';
    }
}

/* Set marquee text with a constant, readable scroll speed.
 *
 * LVGL 9.5 SCROLL_CIRCULAR reads the ANIM_DURATION style as the duration of
 * one full scroll loop in ms (unless it carries the speed-encoding mask from
 * lv_anim_speed(), whose granularity is 10px/s). To get an exact speed we
 * compute the duration from the actual text width: lv_label_set_text()
 * synchronously refreshes the label's self size (lv_label.c set_text_internal
 * → lv_obj_refresh_self_size), so lv_obj_get_self_width() right after is the
 * new text width. duration_ms = text_w / speed. */
static void set_scroll_text(const char *text, lv_color_t color)
{
    static char scroll_buf[4096];

    if (!text || !text[0]) {
        lv_obj_add_flag(s_scroll_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    strncpy(scroll_buf, text, sizeof(scroll_buf) - 1);
    scroll_buf[sizeof(scroll_buf) - 1] = '\0';
    normalize_marquee_text(scroll_buf, sizeof(scroll_buf));

    lv_label_set_text(s_scroll_label, scroll_buf);
    lv_obj_set_style_text_color(s_scroll_label, color, 0);

    int32_t w = lv_obj_get_self_width(s_scroll_label);  /* text width, synced */
    if (w > 0) {
        uint32_t duration_ms = (uint32_t)((uint64_t)w * 1000 / SCROLL_SPEED_PX_PER_SEC);
        lv_obj_set_style_anim_duration(s_scroll_label, duration_ms, 0);
    }

    lv_obj_remove_flag(s_scroll_label, LV_OBJ_FLAG_HIDDEN);
}

static void create_state_widgets(void)
{
    /* ══════════════════════════════════════════════════════════════════
     * Layout zones on 410×502 screen (center y=251, LV_ALIGN_CENTER).
     * GIF removed — content owns the full height, zones recentered on
     * the middle-lower half so the screen reads balanced below the
     * Dynamic Island (top y=0..92):
     *   主标题/时钟/波形      y=-66  → 中心 y≈185
     *   副标题/日期           y=-6   → 中心 y≈245
     *   滚动文本 s_scroll_label y=+54 → 中心 y≈305, 高64 (底 y≈337)
     * ══════════════════════════════════════════════════════════════════ */

    /* ── 主标题 s_center_label — 替换旧 y=-40 ────────────────────────── */
    s_center_label = lv_label_create(s_screen);
    lv_label_set_text(s_center_label, "");
    lv_obj_set_style_text_font(s_center_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_center_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(s_center_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_center_label, SCREEN_W - 64);  /* 32px L + 32px R */
    lv_label_set_long_mode(s_center_label, LV_LABEL_LONG_DOT);
    /* Y=-66: 中心 y≈185, height ~28 (171..199) */
    lv_obj_align(s_center_label, LV_ALIGN_CENTER, 0, -66);
    lv_obj_add_flag(s_center_label, LV_OBJ_FLAG_HIDDEN);

    /* ── 副标题 s_detail_label — 替换旧 y=+52 ────────────────────────── */
    s_detail_label = lv_label_create(s_screen);
    lv_label_set_text(s_detail_label, "");
    lv_obj_set_style_text_font(s_detail_label, &SourceHanSansCN_Medium_16, 0);
    lv_obj_set_style_text_color(s_detail_label, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_text_align(s_detail_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_detail_label, SCREEN_W - 64);
    lv_label_set_long_mode(s_detail_label, LV_LABEL_LONG_DOT);
    /* Y=-6: 中心 y≈245, height ~20 (235..255) */
    lv_obj_align(s_detail_label, LV_ALIGN_CENTER, 0, -6);
    lv_obj_add_flag(s_detail_label, LV_OBJ_FLAG_HIDDEN);

    /* ── 统一滚动文本 s_scroll_label — 替换旧 stt/tts/response_area ─── */
    /* Single-line, left-scrolling marquee. Replaces separate s_stt_label,
     * s_tts_label, and s_response_area. One physical position = zero overlap.
     *
     * Speed is set per-text in set_scroll_text() (exact px/s from text width,
     * see SCROLL_SPEED_PX_PER_SEC). Never set ANIM_DURATION to a plain ms
     * value here: SCROLL_CIRCULAR treats it as ms per full loop — e.g. a
     * plain 25 = 25ms per loop, invisibly fast "non-scroll" flicker. */
    s_scroll_label = lv_label_create(s_screen);
    lv_label_set_text(s_scroll_label, "");
    lv_obj_set_style_text_font(s_scroll_label, &SourceHanSansCN_Medium_16, 0);
    lv_obj_set_style_text_color(s_scroll_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(s_scroll_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(s_scroll_label, SCREEN_W - 64);
    lv_obj_set_height(s_scroll_label, 64);
    lv_label_set_long_mode(s_scroll_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    /* Y=+54: 中心 y≈305, height 64, bottom at y ≈ 337 */
    lv_obj_align(s_scroll_label, LV_ALIGN_CENTER, 0, 54);
    lv_obj_add_flag(s_scroll_label, LV_OBJ_FLAG_HIDDEN);

    /* ── IDLE 大时钟 s_idle_time — 120px ClockNum 字体 ──────────────── */
    /* Ver2.1 同款 120px Akrobat-ExtraBold 时钟字体（行高 122）。
     * Y=-51: 中心 y≈200 (139..261)，下方为日期，与顶部 island (0..92)
     * 及底部留白均衡。IDLE 时主标题隐藏，时钟代替。 */
    s_idle_time = lv_label_create(s_screen);
    lv_label_set_text(s_idle_time, "00:00");
    lv_obj_set_style_text_font(s_idle_time, &ui_font_ClockNum, 0);
    lv_obj_set_style_text_color(s_idle_time, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(s_idle_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_idle_time, SCREEN_W - 64);
    lv_label_set_long_mode(s_idle_time, LV_LABEL_LONG_DOT);
    lv_obj_align(s_idle_time, LV_ALIGN_CENTER, 0, -51);
    lv_obj_add_flag(s_idle_time, LV_OBJ_FLAG_HIDDEN);

    /* ── IDLE 日期 s_idle_date — 24px DateNum 字体 ──────────────────── */
    /* Ver2.1 同款 24px 思源柔黑体（行高 28，符号集含"星期"）。
     * Y=+49: 中心 y≈300 (286..314)，时钟底 261 → 25px 间隙。
     * IDLE 时副标题隐藏，日期代替。 */
    s_idle_date = lv_label_create(s_screen);
    lv_label_set_text(s_idle_date, "");
    lv_obj_set_style_text_font(s_idle_date, &ui_font_DateNum, 0);
    lv_obj_set_style_text_color(s_idle_date, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_text_align(s_idle_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_idle_date, SCREEN_W - 64);
    lv_label_set_long_mode(s_idle_date, LV_LABEL_LONG_DOT);
    lv_obj_align(s_idle_date, LV_ALIGN_CENTER, 0, 49);
    lv_obj_add_flag(s_idle_date, LV_OBJ_FLAG_HIDDEN);

    /* ── LISTENING 波形 s_wave_container — 更新位置 ─────────────────── */
    /* 居中，y=-66 与主标题同位（LISTENING 时主标题隐藏，波形代替）。
     * 容器高 88 → 141..229，与下方副标题 (235..255) 留 10px 间隙 */
    s_wave_container = lv_obj_create(s_screen);
    lv_obj_set_size(s_wave_container, WAVE_CONTAINER_W, WAVE_CONTAINER_H);
    lv_obj_align(s_wave_container, LV_ALIGN_CENTER, 0, -66);
    lv_obj_set_style_bg_opa(s_wave_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wave_container, 0, 0);
    lv_obj_set_style_pad_all(s_wave_container, 0, 0);
    lv_obj_add_flag(s_wave_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_wave_container, LV_OBJ_FLAG_SCROLLABLE);

    /* Flex layout for wave bars */
    lv_obj_set_flex_flow(s_wave_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_wave_container,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < WAVE_BARS; i++) {
        s_wave_bars[i] = lv_obj_create(s_wave_container);
        lv_obj_set_size(s_wave_bars[i], WAVE_BAR_W, 8);
        lv_obj_set_style_radius(s_wave_bars[i], 3, 0);
        lv_obj_set_style_bg_color(s_wave_bars[i], lv_color_hex(0xff453a), 0);
        lv_obj_set_style_bg_opa(s_wave_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_wave_bars[i], 0, 0);
        lv_obj_remove_flag(s_wave_bars[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    /* ── s_listening_label: 不再单独创建 ─────────────────────────────── */
    /* 旧 s_listening_label 已删除，LISTENING 状态的提示文字统一用
     * s_detail_label (show_subtitle("说点什么吧")) 显示，避免与波形重叠。 */

    /* ── 对话气泡容器 s_bubble_container（Task 7 最小实现） ─────────── */
    /* Dynamic Island 下方 y≈96 到底部；flex 列；默认隐藏。
     * 进入聊天状态或 IDLE 有历史时显示（show_content_for_state）。 */
    s_bubble_container = lv_obj_create(s_screen);
    lv_obj_set_pos(s_bubble_container, 4, 96);
    lv_obj_set_size(s_bubble_container, SCREEN_W - 8, SCREEN_H - 96 - 4);
    lv_obj_set_style_bg_opa(s_bubble_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bubble_container, 0, 0);
    lv_obj_set_style_pad_all(s_bubble_container, 4, 0);
    lv_obj_set_flex_flow(s_bubble_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_bubble_container, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(s_bubble_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_bubble_container, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_bubble_container, LV_OBJ_FLAG_HIDDEN);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Content visibility management
 * ══════════════════════════════════════════════════════════════════════════ */

static void hide_all_content(void)
{
    if (s_center_label)    lv_obj_add_flag(s_center_label, LV_OBJ_FLAG_HIDDEN);
    if (s_detail_label)    lv_obj_add_flag(s_detail_label, LV_OBJ_FLAG_HIDDEN);
    if (s_idle_time)       lv_obj_add_flag(s_idle_time, LV_OBJ_FLAG_HIDDEN);
    if (s_idle_date)       lv_obj_add_flag(s_idle_date, LV_OBJ_FLAG_HIDDEN);
    if (s_wave_container)  lv_obj_add_flag(s_wave_container, LV_OBJ_FLAG_HIDDEN);
    if (s_scroll_label)    lv_obj_add_flag(s_scroll_label, LV_OBJ_FLAG_HIDDEN);
    if (s_bubble_container) lv_obj_add_flag(s_bubble_container, LV_OBJ_FLAG_HIDDEN);

    stop_wave_animation();
}

/* Show title in the main title zone (y≈185) */
static void show_title(const char *text, const lv_font_t *font)
{
    if (!s_center_label) return;
    lv_label_set_text(s_center_label, text);
    lv_obj_set_style_text_font(s_center_label, font, 0);
    lv_obj_remove_flag(s_center_label, LV_OBJ_FLAG_HIDDEN);
}

/* Show subtitle in the subtitle zone (y≈245) */
static void show_subtitle(const char *text)
{
    if (!s_detail_label) return;
    lv_label_set_text(s_detail_label, text);
    lv_obj_remove_flag(s_detail_label, LV_OBJ_FLAG_HIDDEN);
}

/* 对话气泡区可见性：聊天状态或 IDLE 有对话历史时显示（替代该状态的
 * 中央文本/时钟）。返回是否已显示。 */
static bool show_bubble_area(void)
{
    if (s_bubble_container && s_bubble_count > 0) {
        lv_obj_remove_flag(s_bubble_container, LV_OBJ_FLAG_HIDDEN);
        return true;
    }
    return false;
}

static void show_content_for_state(ui_state_t state)
{
    /* Every state shows at most 3 controls: title + subtitle + scroll text.
     * hide_all_content() hides everything before enabling state-specific ones. */
    hide_all_content();

    switch (state) {

    case UI_STATE_BOOT:
        show_title("Booting", &lv_font_montserrat_28);
        show_subtitle("AIWatch v0.1.0");
        break;

    case UI_STATE_CONNECTING:
        show_title("Connecting...", &lv_font_montserrat_20);
        show_subtitle("正在连接网络");
        break;

    case UI_STATE_IDLE:
        /* 有对话历史 → 显示气泡（保留聊天现场）；否则大时钟 + 日期 */
        if (show_bubble_area()) break;
        if (s_idle_time)  lv_obj_remove_flag(s_idle_time, LV_OBJ_FLAG_HIDDEN);
        if (s_idle_date)  lv_obj_remove_flag(s_idle_date, LV_OBJ_FLAG_HIDDEN);
        update_idle_clock();
        break;

    case UI_STATE_LISTENING:
        /* Waveform in title zone, hint text in subtitle zone.
         * s_listening_label removed — unified into s_detail_label. */
        if (s_wave_container)  lv_obj_remove_flag(s_wave_container, LV_OBJ_FLAG_HIDDEN);
        show_subtitle("说点什么吧");
        start_wave_animation();
        break;

    case UI_STATE_SENDING:
        /* 对话中显示气泡（Task 7）；无气泡时回退原 "Sending..." 文本 */
        if (show_bubble_area()) break;
        show_title("Sending...", &lv_font_montserrat_20);
        show_subtitle("语音识别中");
        /* STT text shown via ui_set_stt_text() — state-guarded */
        break;

    case UI_STATE_THINKING:
        if (show_bubble_area()) break;
        show_title("Thinking...", &lv_font_montserrat_20);
        show_subtitle("AI思考中");
        break;

    case UI_STATE_STREAMING:
        if (show_bubble_area()) break;
        show_title("Receiving...", &lv_font_montserrat_20);
        show_subtitle("接收回复中");
        break;

    case UI_STATE_RESPONSE:
        if (show_bubble_area()) break;
        show_title("AI Response", &lv_font_montserrat_20);
        /* LLM response shown via ui_set_response() — state-guarded */
        break;

    case UI_STATE_TTS_LOADING:
        show_title("Loading Audio...", &lv_font_montserrat_20);
        show_subtitle("准备播放");
        break;

    case UI_STATE_TTS_PLAYING:
        show_title("Speaking", &lv_font_montserrat_28);
        show_subtitle("语音播放中");
        /* TTS text shown via ui_set_tts_text() — state-guarded */
        break;

    case UI_STATE_PLAYING_MP3:
        /* Handled entirely by ui_mp3_ui overlay */
        break;

    case UI_STATE_NOTIFYING:
        show_title("Notification", &lv_font_montserrat_20);
        show_subtitle("新通知");
        break;

    case UI_STATE_ERROR:
        show_title("Error", &lv_font_montserrat_20);
        show_subtitle("发生错误");
        break;

    case UI_STATE_SLEEP:
    case UI_STATE_ARMED:
        /* Minimal — show nothing */
        break;

    default:
        break;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * UI Initialization
 * ══════════════════════════════════════════════════════════════════════════ */

/* Task 2 (M1) touch-mapping debug: logs every tap on the main screen
 * background in LVGL logical coordinates. LVGL9 core rotates raw panel
 * coords via lv_display_rotate_point() using the display's rotation, so
 * this doubles as rotation verification.
 * Expected corner mapping on the rotated 502x410 landscape display
 * (physical panel 410x502, LV_DISPLAY_ROTATION_90):
 *   raw(x,y) → logical(502-y-1, x); corners: TL≈(0,0) TR≈(501,0)
 *   BL≈(0,409) BR≈(501,409) — where the corner is as SEEN on screen.
 * Temporary instrumentation — remove when Task 14 rebuilds the UI. */
static void ui_touch_debug_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    ESP_LOGI(TAG, "TOUCH debug: logical (%d,%d)", pt.x, pt.y);
}

esp_err_t ui_init(void)
{
    ESP_LOGI(TAG, "Initializing UI (%dx%d, Dynamic Island style)...", SCREEN_W, SCREEN_H);

    /* Set timezone to China Standard Time (UTC+8) */
    setenv("TZ", "CST-8", 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to CST-8 (China Standard Time, UTC+8)");

    /* CRITICAL: Hold LVGL lock for the entire init sequence.
     * lv_screen_load() triggers the LVGL refresh task to start rendering,
     * but child widgets (create_state_widgets, ui_state_chip_init, etc.)
     * are not yet created — the refresh task would access uninitialized
     * style properties and crash (Core 1 panic: LoadProhibited). */
    lvgl_port_lock(0);

    /* Create main screen — white background (iOS style, per ui_colors.h) */
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(UI_COLOR_BG_MAIN), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    /* Task 2 (M1): touch-mapping debug log on background taps (see
     * ui_touch_debug_cb). Temporary — remove with Task 14 UI rebuild. */
    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_screen, ui_touch_debug_cb, LV_EVENT_CLICKED, NULL);

    /* Load screen first so all child objects are created on correct screen */
    lv_screen_load(s_screen);

    /* ── Initialize Dynamic Island (top pill: WiFi · State · Clock · OC) ── */
    /* This replaces the old status bar + bottom state chip */
    ui_state_chip_init(s_screen);

    /* ── Create state-specific content widgets ──────────────────────────── */
    create_state_widgets();

    /* ── Initialize sub-modules ─────────────────────────────────────────── */
    /* Status bar: NO LONGER used — Dynamic Island handles its functions */
    /* ui_status_bar_init(s_screen); */

    /* MP3 UI overlay */
    ui_mp3_ui_init(s_screen);

    /* Set initial state — ui_set_state() only touches LVGL objects and takes
     * no lock, so it is safe to call while we hold lvgl_port_lock(0). */
    ui_set_state(UI_STATE_BOOT);

    lvgl_port_unlock();

    /* Start clock update timer (outside lock — uses its own periodic lock) */
    start_clock_timer();

    ESP_LOGI(TAG, "UI initialized successfully — Dynamic Island layout");
    return ESP_OK;
}

void ui_destroy(void)
{
    /* Stop clock update timer */
    stop_clock_timer();

    /* Stop wave animation */
    stop_wave_animation();

    /* Stop LED animation task */
    ui_led_anim_stop();

    if (s_screen) {
        lv_obj_del(s_screen);
        s_screen = NULL;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * State Management
 * ══════════════════════════════════════════════════════════════════════════ */

void ui_set_state(ui_state_t state)
{
    if (state == s_state) return;  /* No change — skip redundant work */

    ui_state_t old_state = s_state;
    s_state = state;

    ESP_LOGI(TAG, "State transition: %s → %s",
             state_labels[old_state], state_labels[state]);

    /* Update RGB LED for new state */
    ui_update_led_for_state(state);

    /* Handle state-specific transitions */
    handle_state_transition(old_state, state);

    /* Update display — show state-specific content */
    update_state_display(state);

    /* Update Dynamic Island (top pill indicator) */
    if (state <= UI_STATE_ERROR) {
        ui_state_chip_update(state_labels[state], state_chinese[state], get_state_color(state));
    }
}

ui_state_t ui_get_state(void)
{
    return s_state;
}

/* ══════════════════════════════════════════════════════════════════════════
 * State Transition Handler
 * ══════════════════════════════════════════════════════════════════════════ */

static void handle_state_transition(ui_state_t old_state, ui_state_t new_state)
{
    /* Hide MP3 UI when leaving MP3 state */
    if (old_state == UI_STATE_PLAYING_MP3 && new_state != UI_STATE_PLAYING_MP3) {
        ui_mp3_ui_hide();
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * State Display Update
 * ══════════════════════════════════════════════════════════════════════════ */

static void update_state_display(ui_state_t state)
{
    /* Show the appropriate content for the current state */
    /* show_content_for_state handles visibility of all content widgets */

    switch (state) {
    case UI_STATE_BOOT:
        show_content_for_state(UI_STATE_BOOT);
        break;

    case UI_STATE_CONNECTING:
        show_content_for_state(UI_STATE_CONNECTING);
        break;

    case UI_STATE_IDLE:
        show_content_for_state(UI_STATE_IDLE);
        break;

    case UI_STATE_LISTENING:
        show_content_for_state(UI_STATE_LISTENING);
        break;

    case UI_STATE_SENDING:
        show_content_for_state(UI_STATE_SENDING);
        break;

    case UI_STATE_THINKING:
        show_content_for_state(UI_STATE_THINKING);
        break;

    case UI_STATE_STREAMING:
        show_content_for_state(UI_STATE_STREAMING);
        break;

    case UI_STATE_RESPONSE:
        show_content_for_state(UI_STATE_RESPONSE);
        break;

    case UI_STATE_TTS_LOADING:
        show_content_for_state(UI_STATE_TTS_LOADING);
        break;

    case UI_STATE_TTS_PLAYING:
        show_content_for_state(UI_STATE_TTS_PLAYING);
        break;

    case UI_STATE_PLAYING_MP3:
        show_content_for_state(UI_STATE_PLAYING_MP3);
        break;

    case UI_STATE_NOTIFYING:
        show_content_for_state(UI_STATE_NOTIFYING);
        break;

    case UI_STATE_ERROR:
        show_content_for_state(UI_STATE_ERROR);
        break;

    case UI_STATE_SLEEP:
    case UI_STATE_ARMED:
        show_content_for_state(state);
        break;

    default:
        hide_all_content();
        break;
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * API Implementations
 * ══════════════════════════════════════════════════════════════════════════ */

void ui_set_event_group(void *event_group)
{
    s_events = (EventGroupHandle_t)event_group;
}

void ui_set_wifi_status(bool connected, int rssi)
{
    /* Update Dynamic Island WiFi icon */
    ui_state_chip_set_wifi(connected);
    (void)rssi;
}

void ui_set_battery_status(int percent, bool charging)
{
    /* TODO: Add battery indicator to Dynamic Island if needed */
    (void)percent;
    (void)charging;
}

void ui_set_openclaw_connected(bool connected)
{
    /* Update Dynamic Island OC dot */
    ui_state_chip_set_oc(connected);
}

void ui_set_response(const char *short_text, const char *full_text)
{
    if (!short_text) return;

    /* Store full response for TTS */
    if (full_text) {
        strncpy(s_full_response, full_text, sizeof(s_full_response) - 1);
        s_full_response[sizeof(s_full_response) - 1] = '\0';
    }

    /* State guard: only show in RESPONSE state */
    if (s_state != UI_STATE_RESPONSE) return;

    if (s_center_label) {
        lv_label_set_text(s_center_label, "AI Response");
        lv_obj_set_style_text_font(s_center_label, &lv_font_montserrat_20, 0);
        lv_obj_remove_flag(s_center_label, LV_OBJ_FLAG_HIDDEN);
    }

    /* Hide subtitle — RESPONSE only shows title + scroll text */
    if (s_detail_label) lv_obj_add_flag(s_detail_label, LV_OBJ_FLAG_HIDDEN);

    /* Show LLM response as left-scrolling marquee (slow, readable speed) */
    if (s_scroll_label) {
        set_scroll_text(full_text ? full_text : "", lv_color_hex(0xFFFFFF));
    }

    ESP_LOGI(TAG, "Response: %.100s", short_text);
}

const char *ui_get_full_response(void)
{
    return s_full_response;
}

void ui_set_thinking_time(uint32_t elapsed_ms)
{
    /* s_center_label: "Thinking...", s_detail_label: elapsed time */
    if (s_state == UI_STATE_THINKING) {
        if (s_center_label) {
            lv_label_set_text(s_center_label, "Thinking...");
            lv_obj_set_style_text_font(s_center_label, &lv_font_montserrat_20, 0);
        }
        if (s_detail_label) {
            char text[48];
            uint32_t sec = elapsed_ms / 1000;
            if (sec < 60) {
                snprintf(text, sizeof(text), "(%lus)", (unsigned long)sec);
            } else {
                snprintf(text, sizeof(text), "(%lum%lus)",
                         (unsigned long)(sec / 60), (unsigned long)(sec % 60));
            }
            lv_label_set_text(s_detail_label, text);
        }
    }
}

void ui_set_thinking_detail(const char *detail, uint32_t elapsed_ms)
{
    /* s_center_label: "Thinking...", s_detail_label: detail + time */
    if (s_state == UI_STATE_THINKING) {
        if (s_center_label) {
            lv_label_set_text(s_center_label, "Thinking...");
            lv_obj_set_style_text_font(s_center_label, &lv_font_montserrat_20, 0);
        }
        if (s_detail_label && detail) {
            char text[64];
            uint32_t sec = elapsed_ms / 1000;
            snprintf(text, sizeof(text), "%s (%lus)", detail, (unsigned long)sec);
            lv_label_set_text(s_detail_label, text);
        }
    }
}

void ui_set_cost(const char *cost_str)
{
    /* TODO: Show cost in Dynamic Island if needed */
    (void)cost_str;
}

// TODO(Task 8): 由 doubao 链路替换 — openclaw_info_t deleted in Task 1
// void ui_set_server_info(const openclaw_info_t *info)
// {
//     (void)info;
// }

void ui_set_status_message(const char *msg)
{
    if (!msg) return;

    /* During states with dual labels, put status in the detail (lower) label */
    if (s_detail_label && (s_state == UI_STATE_CONNECTING || s_state == UI_STATE_THINKING ||
                           s_state == UI_STATE_STREAMING || s_state == UI_STATE_SENDING ||
                           s_state == UI_STATE_TTS_PLAYING)) {
        lv_label_set_text(s_detail_label, msg);
        lv_obj_remove_flag(s_detail_label, LV_OBJ_FLAG_HIDDEN);
    } else if (s_center_label) {
        /* ── Show a status message in the center label area ──
         * CRITICAL: hide state-specific widgets that occupy the same screen
         * region (y≈185, LV_ALIGN_CENTER offset -66) to prevent overlap.
         * The next show_content_for_state() call restores them cleanly. */
        switch (s_state) {
        case UI_STATE_IDLE:
            if (s_idle_time) lv_obj_add_flag(s_idle_time, LV_OBJ_FLAG_HIDDEN);
            if (s_idle_date) lv_obj_add_flag(s_idle_date, LV_OBJ_FLAG_HIDDEN);
            break;
        case UI_STATE_LISTENING:
            if (s_wave_container)  lv_obj_add_flag(s_wave_container, LV_OBJ_FLAG_HIDDEN);
            if (s_detail_label)    lv_obj_add_flag(s_detail_label, LV_OBJ_FLAG_HIDDEN);
            break;
        case UI_STATE_BOOT:
            if (s_detail_label) lv_obj_add_flag(s_detail_label, LV_OBJ_FLAG_HIDDEN);
            break;
        default:
            break;
        }
        lv_label_set_text(s_center_label, msg);
        lv_obj_set_style_text_font(s_center_label, &lv_font_montserrat_20, 0);
        lv_obj_remove_flag(s_center_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_set_stt_text(const char *text)
{
    if (!s_scroll_label) return;

    /* State guard: only show in SENDING state */
    if (s_state != UI_STATE_SENDING) {
        lv_obj_add_flag(s_scroll_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* Yellow STT transcription marquee */
    set_scroll_text(text, lv_color_hex(0xFF9F0A));
}

void ui_set_tts_text(const char *text)
{
    if (!s_scroll_label) return;

    /* State guard: only show in TTS_PLAYING state */
    if (s_state != UI_STATE_TTS_PLAYING) {
        lv_obj_add_flag(s_scroll_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* Blue TTS caption marquee */
    set_scroll_text(text, lv_color_hex(0x0A84FF));
}

/* ══════════════════════════════════════════════════════════════════════════
 * Chat bubbles (Task 7 minimal implementation)
 * ══════════════════════════════════════════════════════════════════════════ */

/* 新建一条气泡：is_user=true 右侧蓝底，false 左侧灰底；文本 WRAP。
 * 超过 BUBBLE_MAX_COUNT 删最旧（铁律 19 防碎片化）。 */
static void bubble_new(bool is_user, const char *text)
{
    if (!s_bubble_container) return;

    if (s_bubble_count >= BUBBLE_MAX_COUNT) {
        lv_obj_t *oldest = lv_obj_get_child(s_bubble_container, 0);
        if (oldest) lv_obj_del(oldest);
    } else {
        s_bubble_count++;
    }

    lv_obj_t *bubble = lv_obj_create(s_bubble_container);
    lv_obj_set_width(bubble, lv_pct(BUBBLE_PCT_W));
    lv_obj_set_style_radius(bubble, 10, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, 8, 0);
    lv_obj_set_style_bg_color(bubble,
                              is_user ? lv_color_hex(0x0A84FF) : lv_color_hex(0xE5E5EA), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_remove_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_margin_top(bubble, 6, 0);

    /* 左右对齐（LVGL9 flex 无子项对齐，用 margin 顶位）：
     * 用户气泡 margin_left = 容器内容宽 - 气泡宽 */
    int32_t content_w = (SCREEN_W - 8) - 2 * 4;
    int32_t bubble_w  = content_w * BUBBLE_PCT_W / 100;
    lv_obj_set_style_margin_left(bubble, is_user ? (content_w - bubble_w) : 0, 0);

    lv_obj_t *label = lv_label_create(bubble);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, bubble_w - 16);
    lv_obj_set_style_text_color(label,
                                is_user ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x1C1C1E), 0);
    lv_label_set_text(label, text ? text : "");

    s_last_bubble_is_user = is_user;
    /* LVGL 9 无 lv_obj_scroll_to_bottom（8.x 亦无）；新气泡进视野 =
     * lv_obj_scroll_to_view（底部列表时即滚到底）。 */
    lv_obj_scroll_to_view(bubble, LV_ANIM_OFF);
}

void ui_add_user_bubble(const char *text)
{
    if (!s_bubble_container || !text) return;

    /* transcript 流式更新：最近一条仍是用户气泡 → 整体替换其文本
     * （TRANSCRIPT_DONE 也是整体替换）；否则新建用户气泡，
     * 并作废当前机器人气泡（下一轮回复进新气泡）。 */
    if (s_last_bubble_is_user && s_bubble_count > 0) {
        lv_obj_t *last = lv_obj_get_child(s_bubble_container, s_bubble_count - 1);
        lv_obj_t *label = last ? lv_obj_get_child(last, 0) : NULL;
        if (label) {
            lv_label_set_text(label, text);
            lv_obj_scroll_to_view(last, LV_ANIM_OFF);
            return;
        }
    }
    bubble_new(true, text);
    s_bot_bubble_label = NULL;
    s_bot_text[0] = '\0';
}

void ui_add_bot_bubble(void)
{
    if (!s_bubble_container) return;

    bubble_new(false, "");
    s_bot_bubble_label = lv_obj_get_child(
        s_bubble_container, lv_obj_get_child_count(s_bubble_container) - 1);
    s_bot_bubble_label = s_bot_bubble_label ? lv_obj_get_child(s_bot_bubble_label, 0) : NULL;
    s_bot_text[0] = '\0';
}

void ui_bot_bubble_append(const char *delta)
{
    if (!s_bubble_container || !delta) return;

    if (!s_bot_bubble_label) {
        ui_add_bot_bubble();
    }
    if (!s_bot_bubble_label) return;

    /* Task 7: 整段重设（lv_label_set_text）；流式优化（append 渲染）Task 13 */
    size_t cur  = strlen(s_bot_text);
    size_t room = sizeof(s_bot_text) - cur - 1;
    if (room > 0) {
        strncat(s_bot_text, delta, room);
        s_bot_text[sizeof(s_bot_text) - 1] = '\0';
    }
    lv_label_set_text(s_bot_bubble_label, s_bot_text);
    lv_obj_scroll_to_view(s_bot_bubble_label, LV_ANIM_OFF);
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
    (void)running;
}

void ui_set_task_info(const char *task_text)
{
    (void)task_text;
}

// TODO(Task 8): 由 doubao 链路替换 — openclaw_info_t deleted in Task 1
// void ui_set_task_info_detailed(const openclaw_info_t *info)
// {
//     (void)info;
// }

void ui_set_external_activity(bool active, const char *detail, uint32_t elapsed_ms)
{
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
    ESP_LOGI(TAG, "Widget command received but display disabled");
}

void ui_widget_clear(void)
{
    /* No-op */
}

/* Screen transition (kept for compatibility) */
void ui_screen_load_anim(lv_obj_t *scr, lv_screen_load_anim_t anim, uint32_t time, uint32_t delay)
{
    lv_screen_load_anim(scr, anim, time, delay, false);
}

/* ══════════════════════════════════════════════════════════════════════════
 * IDLE Clock helpers
 * ══════════════════════════════════════════════════════════════════════════ */

static void update_idle_clock(void)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    /* Big clock: "HH:MM" */
    if (s_idle_time) {
        char time_str[16];
        snprintf(time_str, sizeof(time_str), "%02d:%02d",
                 timeinfo.tm_hour, timeinfo.tm_min);
        lv_label_set_text(s_idle_time, time_str);
    }

    /* Chinese date: "2024年12月25日 星期三" */
    if (s_idle_date) {
        const char *weekday_cn[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
        int wday = timeinfo.tm_wday;
        if (wday < 0 || wday > 6) wday = 0;

        char date_str[48];
        snprintf(date_str, sizeof(date_str), "%d年%d月%d日 %s",
                 1900 + timeinfo.tm_year,
                 timeinfo.tm_mon + 1,
                 timeinfo.tm_mday,
                 weekday_cn[wday]);
        lv_label_set_text(s_idle_date, date_str);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Clock update timer
 * ══════════════════════════════════════════════════════════════════════════ */

static void clock_update_timer_cb(void *arg)
{
    (void)arg;

    /* Get current time */
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    if (!lvgl_port_lock(100)) return;

    /* Update Dynamic Island clock */
    ui_state_chip_set_clock(timeinfo.tm_hour, timeinfo.tm_min);

    /* Update IDLE big clock + date if visible */
    if (s_state == UI_STATE_IDLE) {
        update_idle_clock();
    }

    lvgl_port_unlock();
}

static esp_err_t start_clock_timer(void)
{
    if (s_clock_timer) return ESP_OK;

    const esp_timer_create_args_t timer_args = {
        .callback = clock_update_timer_cb,
        .name = "clock_update"
    };

    esp_err_t err = esp_timer_create(&timer_args, &s_clock_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create clock timer: %s", esp_err_to_name(err));
        return err;
    }

    /* Start timer with 1 second period */
    err = esp_timer_start_periodic(s_clock_timer, 1000000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start clock timer: %s", esp_err_to_name(err));
        esp_timer_delete(s_clock_timer);
        s_clock_timer = NULL;
        return err;
    }

    ESP_LOGI(TAG, "Clock update timer started (1s interval)");
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
