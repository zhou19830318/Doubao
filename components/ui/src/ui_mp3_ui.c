/*
 * SPDX-FileCopyrightText: 2024-2026 AIWatch Contributors
 * SPDX-License-Identifier: MIT
 *
 * MP3 player UI — music.html-inspired design (no-transform safe version):
 *   • Circular album art with gradient background + music note icon
 *   • Glow ring with border-opacity pulse when playing (NO transforms)
 *   • Progress bar (lv_bar) with current / total time labels
 *   • Tap anywhere to toggle play/pause (ring flash feedback)
 *   • Dark theme with coral-red accent (#ff6b6b)
 *
 * IMPORTANT: This code deliberately avoids lv_obj_set_style_transform_angle
 * and transform_zoom because LVGL 8.3's SW renderer allocates PSRAM snapshot
 * buffers for transformed layers.  When audio DMA (I2S) is active, parallel
 * PSRAM access from the transform layer causes MMU cache faults (EXCCAUSE=7).
 */

#include "ui_mp3_ui.h"
#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>    /* strcasecmp */
#include <ctype.h>
#include <math.h>       /* sinf */

/* External Chinese font */
extern const lv_font_t SourceHanSansCN_Medium_16;

/* Forward declarations from app_state module */
extern void app_queue_mp3_cmd(const char *cmd);
extern const char **sd_mp3_get_list(void);
extern int sd_mp3_get_count(void);
extern int *sd_mp3_get_selected_index(void);

static const char *TAG = "ui_mp3_ui";

/* ══════════════════════════════════════════════════════════════════════════
 * Color scheme — aligned with music.html (#ff6b6b accent)
 * ══════════════════════════════════════════════════════════════════════════ */
#define COLOR_BG            lv_color_hex(0x12121f)
#define COLOR_ACCENT        lv_color_hex(0xff6b6b)
#define COLOR_WHITE         lv_color_hex(0xffffff)
#define COLOR_TEXT_WHITE    lv_color_hex(0xf0f0f5)
#define COLOR_TEXT_GRAY     lv_color_hex(0xb0b0c0)
#define COLOR_COVER_BG      lv_color_hex(0x2d2050)
#define COLOR_COVER_GRAD    lv_color_hex(0x3a2080)
#define COLOR_NOTE          lv_color_hex(0xc8b8ff)

/* ══════════════════════════════════════════════════════════════════════════
 * Layout dimensions (screen: 172×320, portrait)
 * ══════════════════════════════════════════════════════════════════════════ */
#define COVER_SIZE           (BOARD_LCD_H_RES * 80 / 172)   /* Album art diameter */
#define RING_SIZE            (BOARD_LCD_H_RES * 96 / 172)   /* Glow ring outer diameter */
#define RING_BORDER          3       /* Ring border thickness */
#define RING_Y_OFFSET        (BOARD_LCD_V_RES * 38 / 320)   /* Ring top from panel top */
#define TITLE_Y              (BOARD_LCD_V_RES * 160 / 320)  /* Song title y */
#define ARTIST_Y             (BOARD_LCD_V_RES * 180 / 320)  /* Artist text y */
#define STATUS_Y_OFFSET      (BOARD_LCD_V_RES * -18 / 320)  /* Status label from bottom */

/* Pulse timer period (ms) — 200ms reduces LCD SPI DMA pressure during MP3
 * playback. At 60ms the constant opacity changes flood the already
 * DMA-starved display pipeline, freezing the animation. 200ms is still
 * smooth breathing (~3s per full sine cycle). */
#define PULSE_PERIOD_MS      200

/* ══════════════════════════════════════════════════════════════════════════
 * Static widgets
 * ══════════════════════════════════════════════════════════════════════════ */
static lv_obj_t  *s_mp3_panel    = NULL;
static lv_obj_t  *s_ring         = NULL;
static lv_obj_t  *s_album_art    = NULL;
static lv_obj_t  *s_album_note   = NULL;
static lv_obj_t  *s_title_label  = NULL;
static lv_obj_t  *s_artist_label = NULL;
static lv_obj_t  *s_status_label = NULL;

/* ══════════════════════════════════════════════════════════════════════════
 * Playback / animation state
 * ══════════════════════════════════════════════════════════════════════════ */
static bool       s_is_playing   = false;
static int        s_total_sec    = 0;
static lv_timer_t *s_pulse_timer = NULL;   /* Ring opacity pulse timer */
static int        s_pulse_phase  = 0;       /* 0..31 for sine-based opacity */

/* ══════════════════════════════════════════════════════════════════════════
 * Selection-mode state
 * ══════════════════════════════════════════════════════════════════════════ */
static bool          s_selection_mode = false;
static const char  **s_song_list      = NULL;
static int           s_song_count     = 0;
static int          *s_current_index  = NULL;

/* Touch gesture tracking */
static int32_t  s_touch_start_x = 0;
static int32_t  s_touch_start_y = 0;
static bool     s_touch_active  = false;
static int64_t  s_last_tap_time = 0;
static int32_t  s_last_tap_x    = 0;
static int32_t  s_last_tap_y    = 0;

/* ══════════════════════════════════════════════════════════════════════════
 * Forward declarations
 * ══════════════════════════════════════════════════════════════════════════ */
static void pulse_timer_start(void);
static void pulse_timer_stop(void);
static void ring_flash(void);

/* ══════════════════════════════════════════════════════════════════════════
 * Ring opacity pulse — safe alternative to transform_angle rotation.
 *
 * Uses a lv_timer at 200ms to vary the ring's border opacity
 * with a sine-wave pattern, creating a breathing effect.
 * Only border_opa is changed (NOT shadow_opa) — box shadow on a
 * 228px circle takes >5s to render, triggering the task watchdog.
 * ══════════════════════════════════════════════════════════════════════════ */

static void pulse_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_ring) return;

    /* Sine wave: 0..2π → smooth pulse over 32 steps */
    s_pulse_phase = (s_pulse_phase + 1) & 0x1F;  /* wrap at 32 */
    float t = (float)s_pulse_phase / 32.0f * 3.14159f * 2.0f;
    float v = (sinf(t) + 1.0f) / 2.0f;  /* 0..1 */

    /* Only change border_opa — NO shadow_opa changes.
     * Box shadow rendering on a 228px circular object takes >5s in LVGL's
     * SW renderer (no ASM optimization), triggering the task watchdog.
     * Border-only redraw is ~100x faster (just a circle outline). */
    int border_opa = 30 + (int)(v * 40.0f);   /* 30..70 */
    lv_obj_set_style_border_opa(s_ring, (lv_opa_t)border_opa, 0);
}

static void pulse_timer_start(void)
{
    if (s_pulse_timer) return;
    s_pulse_phase = 0;
    s_pulse_timer = lv_timer_create(pulse_timer_cb, PULSE_PERIOD_MS, NULL);
}

static void pulse_timer_stop(void)
{
    if (s_pulse_timer) {
        lv_timer_del(s_pulse_timer);
        s_pulse_timer = NULL;
    }
    /* Restore dim ring */
    if (s_ring) {
        lv_obj_set_style_border_opa(s_ring, LV_OPA_30, 0);
    }
}

/* ── Ring flash: brief bright flash on tap (like bounce without transforms) ── */
static void ring_flash_cb(lv_timer_t *timer)
{
    (void)timer;
    /* Restore normal playing/dim state */
    if (!s_ring) return;
    if (s_is_playing) {
        /* Let pulse timer take over again */
        lv_obj_set_style_border_color(s_ring, COLOR_ACCENT, 0);
    } else {
        lv_obj_set_style_border_opa(s_ring, LV_OPA_30, 0);
    }
}

static void ring_flash(void)
{
    if (!s_ring) return;
    /* Brighten the ring momentarily */
    lv_obj_set_style_border_color(s_ring, COLOR_WHITE, 0);
    lv_obj_set_style_border_opa(s_ring, LV_OPA_90, 0);
    /* Restore after 200ms */
    lv_timer_t *t = lv_timer_create(ring_flash_cb, 200, NULL);
    lv_timer_set_repeat_count(t, 1);  /* one-shot */
}

/* ══════════════════════════════════════════════════════════════════════════
 * Panel touch handler — two modes:
 *   Selection  – swipe = navigate, double-tap = confirm
 *   Playback   – short tap = toggle play/pause + ring flash
 * ══════════════════════════════════════════════════════════════════════════ */
static void mp3_panel_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (!s_mp3_panel) {
        s_selection_mode = false;
        return;
    }

    /* ── Selection mode ──────────────────────────────────────────────── */
    if (s_selection_mode) {
        if (code == LV_EVENT_PRESSED) {
            lv_indev_t *indev = lv_indev_get_act();
            if (indev) {
                lv_point_t pt;
                lv_indev_get_point(indev, &pt);
                s_touch_start_x = pt.x;
                s_touch_start_y = pt.y;
                s_touch_active = true;
            }
            return;
        }

        if (code == LV_EVENT_RELEASED && s_touch_active) {
            lv_indev_t *indev = lv_indev_get_act();
            if (!indev) { s_touch_active = false; return; }

            lv_point_t pt;
            lv_indev_get_point(indev, &pt);
            int32_t dx = pt.x - s_touch_start_x;
            int32_t dy = s_touch_start_y - pt.y;
            int64_t now = esp_timer_get_time() / 1000;

            int32_t tap_dist = abs(pt.x - s_last_tap_x) + abs(pt.y - s_last_tap_y);
            int64_t tap_intv = now - s_last_tap_time;

            if (tap_intv < 300 && tap_dist < 50 &&
                abs(dx) < 20 && abs(dy) < 20)
            {
                ESP_LOGI(TAG, "Double-tap → confirm selection");
                bool ok = ui_mp3_ui_handle_selection_input(0);
                if (ok) {
                    const char **lst = sd_mp3_get_list();
                    int cnt = sd_mp3_get_count();
                    int *idx = sd_mp3_get_selected_index();
                    if (lst && idx && *idx >= 0 && *idx < cnt) {
                        char cmd[256];
                        snprintf(cmd, sizeof(cmd), "play:%s", lst[*idx]);
                        app_queue_mp3_cmd(cmd);
                        ui_mp3_ui_hide();
                    }
                }
                s_last_tap_time = 0;
                s_last_tap_x = 0;
                s_last_tap_y = 0;
            }
            else if (abs(dx) > 30 || abs(dy) > 30) {
                int dir = (abs(dx) > abs(dy))
                    ? ((dx > 0) ? 1 : -1)
                    : ((dy > 0) ? 1 : -1);
                ui_mp3_ui_handle_selection_input(dir);
            }
            else {
                s_last_tap_x = pt.x;
                s_last_tap_y = pt.y;
                s_last_tap_time = now;
            }
            s_touch_active = false;
            return;
        }
        return;
    }

    /* ── Playback mode — tap toggles play/pause ──────────────────────── */
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGD(TAG, "Tap → toggle play/pause (currently %s)",
                 s_is_playing ? "playing" : "paused");
        ring_flash();
        app_queue_mp3_cmd(s_is_playing ? "pause" : "resume");
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Helpers
 * ══════════════════════════════════════════════════════════════════════════ */

static void strip_audio_extension(char *dst, const char *src, size_t dst_size)
{
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
    size_t len = strlen(dst);
    if (len >= 4 && (strcasecmp(dst + len - 4, ".mp3") == 0 ||
                     strcasecmp(dst + len - 4, ".wav") == 0)) {
        dst[len - 4] = '\0';
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Initialization
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t ui_mp3_ui_init(lv_obj_t *parent)
{
    if (!parent) {
        ESP_LOGE(TAG, "Invalid parent object");
        return ESP_ERR_INVALID_ARG;
    }

    /* ── Full-screen overlay panel ──────────────────────────────────── */
    s_mp3_panel = lv_obj_create(parent);
    lv_obj_set_size(s_mp3_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_align(s_mp3_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_mp3_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_mp3_panel, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_mp3_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_mp3_panel, 0, 0);
    lv_obj_set_style_pad_all(s_mp3_panel, 0, 0);
    lv_obj_add_flag(s_mp3_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_mp3_panel, mp3_panel_event_cb, LV_EVENT_ALL, NULL);

    /* ── Glow ring (static, NO transform — pulse timer varies opacity) ── */
    s_ring = lv_obj_create(s_mp3_panel);
    lv_obj_set_size(s_ring, RING_SIZE, RING_SIZE);
    lv_obj_set_style_radius(s_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ring, RING_BORDER, 0);
    lv_obj_set_style_border_color(s_ring, COLOR_ACCENT, 0);
    lv_obj_set_style_border_opa(s_ring, LV_OPA_30, 0);
    /* NO shadow — box shadow on a 228px circle takes >5s to render in
     * LVGL's SW renderer (no ASM), causing task watchdog timeouts.
     * The breathing effect uses border_opa only (circle outline = fast). */
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_ring, LV_ALIGN_TOP_MID, 0, RING_Y_OFFSET);
    lv_obj_move_background(s_ring);

    /* ── Album art (static circle with gradient) ─────────────────────── */
    s_album_art = lv_obj_create(s_mp3_panel);
    lv_obj_set_size(s_album_art, COVER_SIZE, COVER_SIZE);
    lv_obj_set_style_radius(s_album_art, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_album_art, COLOR_COVER_BG, 0);
    lv_obj_set_style_bg_grad_color(s_album_art, COLOR_COVER_GRAD, 0);
    lv_obj_set_style_bg_grad_dir(s_album_art, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_album_art, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_album_art, 2, 0);
    lv_obj_set_style_border_color(s_album_art, lv_color_white(), 0);
    lv_obj_set_style_border_opa(s_album_art, LV_OPA_20, 0);
    /* NO shadow — same perf reason as ring above */
    lv_obj_remove_flag(s_album_art, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_album_art, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_album_art, LV_ALIGN_TOP_MID, 0,
                 RING_Y_OFFSET + (RING_SIZE - COVER_SIZE) / 2);

    /* ── Music-note symbol (sibling of album art, on top) ────────────── */
    s_album_note = lv_label_create(s_mp3_panel);
    lv_label_set_text(s_album_note, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(s_album_note, COLOR_NOTE, 0);
    lv_obj_set_style_text_font(s_album_note, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_align(s_album_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(s_album_note, COVER_SIZE, COVER_SIZE);
    lv_obj_set_style_bg_opa(s_album_note, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_album_note, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_album_note, LV_ALIGN_TOP_MID, 0,
                 RING_Y_OFFSET + (RING_SIZE - COVER_SIZE) / 2 + 65);  /* +25 to center icon in circle */

    /* ── Song title ──────────────────────────────────────────────────── */
    s_title_label = lv_label_create(s_mp3_panel);
    lv_label_set_text(s_title_label, "");
    lv_obj_set_style_text_font(s_title_label, &SourceHanSansCN_Medium_16, 0);
    lv_obj_set_style_text_color(s_title_label, COLOR_TEXT_WHITE, 0);
    lv_obj_set_style_text_align(s_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_title_label, BOARD_LCD_H_RES - 24);
    lv_label_set_long_mode(s_title_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_time(s_title_label, 6000, 0);  /* 6s per scroll cycle */
    lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, TITLE_Y + 60);

    /* ── Artist / secondary text ─────────────────────────────────────── */
    s_artist_label = lv_label_create(s_mp3_panel);
    lv_label_set_text(s_artist_label, "");
    lv_obj_set_style_text_font(s_artist_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_artist_label, COLOR_TEXT_GRAY, 0);
    lv_obj_set_style_text_align(s_artist_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_artist_label, LV_ALIGN_TOP_MID, 0, ARTIST_Y + 60);

    /* ── Status / countdown label (bottom area, single-line) ─────────── */
    s_status_label = lv_label_create(s_mp3_panel);
    lv_label_set_text(s_status_label, "");
    lv_obj_set_style_text_font(s_status_label, &SourceHanSansCN_Medium_16, 0);
    lv_obj_set_style_text_color(s_status_label, COLOR_TEXT_GRAY, 0);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_status_label, BOARD_LCD_H_RES - 24);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_time(s_status_label, 6000, 0);  /* 6s per scroll cycle */
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, STATUS_Y_OFFSET);

    ESP_LOGI(TAG, "MP3 UI initialized (no-transform safe version, "
             "cover=%dpx, ring=%dpx)", COVER_SIZE, RING_SIZE);
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Show / Hide
 * ══════════════════════════════════════════════════════════════════════════ */

void ui_mp3_ui_show(const char *track_name, int progress,
                    int current_sec, int total_sec, bool is_playing)
{
    if (!s_mp3_panel) return;

    /* Only unhide + move foreground when transitioning from hidden.
     * Calling move_foreground() every time forces a full panel redraw,
     * flooding the DMA-starved LCD SPI and freezing animations. */
    bool was_hidden = lv_obj_has_flag(s_mp3_panel, LV_OBJ_FLAG_HIDDEN);

    s_total_sec  = total_sec;
    s_is_playing = is_playing;

    /* Only update labels when the panel was hidden or track changed.
     * Repeatedly calling lv_label_set_text on a LV_LABEL_LONG_SCROLL_CIRCULAR
     * label restarts the scroll animation, making it never scroll. */
    if (was_hidden || (track_name && track_name[0])) {
        if (track_name && track_name[0]) {
            if (s_title_label) {
                char name[128];
                strip_audio_extension(name, track_name, sizeof(name));
                lv_label_set_text(s_title_label, name);
            }
            if (s_artist_label) {
                lv_label_set_text(s_artist_label, "MP3 Player");
            }
            if (s_status_label) {
                lv_label_set_text(s_status_label, "");
            }
        } else {
            if (s_title_label)  lv_label_set_text(s_title_label, "Music Player");
            if (s_artist_label) lv_label_set_text(s_artist_label, "");
            if (s_status_label) lv_label_set_text(s_status_label, "请选择歌曲");
        }
    }

    /* Animation state: pulse when playing, dim when paused */
    if (is_playing) {
        pulse_timer_start();
        if (was_hidden) {
            lv_obj_set_style_border_opa(s_ring, LV_OPA_70, 0);
        }
    } else {
        pulse_timer_stop();
    }

    if (was_hidden) {
        lv_obj_remove_flag(s_mp3_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_mp3_panel);
    }

    ESP_LOGD(TAG, "MP3 UI shown: track=%s, playing=%d, %d/%ds",
             track_name ? track_name : "(none)",
             is_playing, current_sec, total_sec);
}

void ui_mp3_ui_hide(void)
{
    pulse_timer_stop();
    if (s_mp3_panel) {
        lv_obj_add_flag(s_mp3_panel, LV_OBJ_FLAG_HIDDEN);
    }
    s_is_playing = false;
    ESP_LOGD(TAG, "MP3 UI hidden");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Progress update
 * ══════════════════════════════════════════════════════════════════════════ */

void ui_mp3_ui_update_progress(int progress, int current_sec)
{
    /* Progress bar removed — no-op */
    (void)progress;
    (void)current_sec;
}

void ui_mp3_ui_update_playback(int current_sec, int total_sec, bool is_playing)
{
    if (!s_mp3_panel) return;
    if (lv_obj_has_flag(s_mp3_panel, LV_OBJ_FLAG_HIDDEN)) return;
    if (s_selection_mode) return;

    (void)current_sec;
    s_total_sec = total_sec;

    /* Play/pause transition */
    if (is_playing != s_is_playing) {
        s_is_playing = is_playing;
        if (is_playing) {
            pulse_timer_start();
        } else {
            pulse_timer_stop();
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * Visibility
 * ══════════════════════════════════════════════════════════════════════════ */

bool ui_mp3_ui_is_visible(void)
{
    if (!s_mp3_panel) return false;
    return !lv_obj_has_flag(s_mp3_panel, LV_OBJ_FLAG_HIDDEN);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Selection mode (song picker via swipe / double-tap)
 * ══════════════════════════════════════════════════════════════════════════ */

void ui_mp3_ui_enter_selection_mode(const char **song_list, int song_count,
                                     int *current_index)
{
    if (!song_list || song_count <= 0 || !current_index) {
        ESP_LOGE(TAG, "Invalid params for selection mode");
        return;
    }

    s_selection_mode = true;
    s_song_list      = song_list;
    s_song_count     = song_count;
    s_current_index  = current_index;

    if (*s_current_index < 0 || *s_current_index >= s_song_count) {
        *s_current_index = 0;
    }

    if (s_status_label) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%d/%d  %s",
                 *s_current_index + 1, s_song_count,
                 s_song_list[*s_current_index]);
        lv_label_set_text(s_status_label, buf);
    }
    if (s_title_label) {
        lv_label_set_text(s_title_label, "选择歌曲");
    }
    if (s_artist_label) {
        lv_label_set_text(s_artist_label, "滑动切换 · 双击确认");
    }

    pulse_timer_stop();

    lv_obj_remove_flag(s_mp3_panel, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "Selection mode: %d songs, index=%d",
             s_song_count, *s_current_index);
}

bool ui_mp3_ui_handle_selection_input(int delta)
{
    if (!s_selection_mode || !s_song_list || !s_current_index) {
        ESP_LOGW(TAG, "Selection input ignored");
        return false;
    }

    if (delta != 0) {
        *s_current_index += delta;
        if (*s_current_index < 0)
            *s_current_index = s_song_count - 1;
        else if (*s_current_index >= s_song_count)
            *s_current_index = 0;

        if (s_status_label) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%d/%d  %s",
                     *s_current_index + 1, s_song_count,
                     s_song_list[*s_current_index]);
            lv_label_set_text(s_status_label, buf);
        }
        ESP_LOGD(TAG, "Selection → #%d: %s",
                 *s_current_index + 1, s_song_list[*s_current_index]);
        return false;
    }

    ESP_LOGI(TAG, "Selection confirmed: #%d - %s",
             *s_current_index + 1, s_song_list[*s_current_index]);

    s_selection_mode = false;
    s_song_list      = NULL;
    s_song_count     = 0;
    s_current_index  = NULL;

    return true;
}
