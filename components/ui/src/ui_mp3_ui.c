/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * MP3 player UI implementation - GIF animation + status text (aligned with other state pages)
 */

#include "ui_mp3_ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>

#if LV_USE_GIF
#include "extra/libs/gif/lv_gif.h"
#endif

/* External Chinese font */
extern const lv_font_t SourceHanSansCN_Medium_16;

static const char *TAG = "ui_mp3_ui";

/* MP3 UI objects */
static lv_obj_t *s_mp3_panel = NULL;
static lv_obj_t *s_gif = NULL;
static lv_obj_t *s_status_label = NULL;

/* Song selection mode state */
static bool s_selection_mode = false;
static const char **s_song_list = NULL;
static int s_song_count = 0;
static int *s_current_index = NULL;

/* Touch gesture tracking */
static int32_t s_touch_start_x = 0;
static int32_t s_touch_start_y = 0;
static bool s_touch_active = false;
static int64_t s_last_tap_time = 0;
static int32_t s_last_tap_x = 0;
static int32_t s_last_tap_y = 0;

/* Touch event handler for song selection */
static void mp3_panel_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (!s_selection_mode) return;
    
    /* Safety check: ensure panel still exists */
    if (!s_mp3_panel) {
        ESP_LOGW(TAG, "Panel is NULL, ignoring touch event");
        s_selection_mode = false;
        return;
    }
    
    if (code == LV_EVENT_PRESSED) {
        /* Record touch start position */
        lv_indev_t *indev = lv_indev_get_act();
        if (indev) {
            lv_point_t point;
            lv_indev_get_point(indev, &point);
            s_touch_start_x = point.x;
            s_touch_start_y = point.y;
            s_touch_active = true;
            ESP_LOGD(TAG, "Touch pressed at (%d, %d)", s_touch_start_x, s_touch_start_y);
        }
    } else if (code == LV_EVENT_RELEASED) {
        if (s_touch_active) {
            lv_indev_t *indev = lv_indev_get_act();
            if (indev) {
                lv_point_t point;
                lv_indev_get_point(indev, &point);
                int32_t delta_x = point.x - s_touch_start_x;
                int32_t delta_y = s_touch_start_y - point.y;  /* Up is positive */
                int64_t current_time = esp_timer_get_time() / 1000;  /* ms */
                
                ESP_LOGD(TAG, "Touch released: start=(%d,%d), end=(%d,%d), delta=(%d,%d)",
                         s_touch_start_x, s_touch_start_y, point.x, point.y, delta_x, delta_y);
                
                /* Check for double tap (within 300ms and 50 pixels) */
                int32_t tap_distance = abs(point.x - s_last_tap_x) + abs(point.y - s_last_tap_y);
                int64_t tap_interval = current_time - s_last_tap_time;
                
                ESP_LOGD(TAG, "Double-tap check: interval=%lldms, distance=%dpx, last_tap=(%d,%d)",
                         (long long)tap_interval, tap_distance, s_last_tap_x, s_last_tap_y);
                
                if (tap_interval < 300 && tap_distance < 50 && abs(delta_x) < 20 && abs(delta_y) < 20) {
                    /* Double tap detected - confirm selection */
                    ESP_LOGI(TAG, "Double tap detected - confirming selection");
                    
                    /* Confirm selection by calling with delta=0 */
                    bool confirmed = ui_mp3_ui_handle_selection_input(0);
                    if (confirmed) {
                        /* Selection confirmed - trigger playback via event */
                        extern void app_queue_mp3_cmd(const char *cmd);
                        extern const char **sd_mp3_get_list(void);
                        extern int sd_mp3_get_count(void);
                        extern int *sd_mp3_get_selected_index(void);
                        
                        const char **list = sd_mp3_get_list();
                        int count = sd_mp3_get_count();
                        int *idx = sd_mp3_get_selected_index();
                        
                        if (list && idx && *idx >= 0 && *idx < count) {
                            char cmd[256];
                            snprintf(cmd, sizeof(cmd), "play:%s", list[*idx]);
                            app_queue_mp3_cmd(cmd);
                            ESP_LOGI(TAG, "Queued play command: %s", list[*idx]);
                            
                            /* Hide MP3 UI and restore normal state */
                            ui_mp3_ui_hide();
                            ESP_LOGI(TAG, "MP3 UI hidden, returning to normal state");
                        }
                    }
                    
                    /* Reset tap tracking */
                    s_last_tap_time = 0;
                    s_last_tap_x = 0;
                    s_last_tap_y = 0;
                } else if (abs(delta_x) > 30 || abs(delta_y) > 30) {
                    /* Swipe detected - determine direction */
                    int direction;
                    
                    if (abs(delta_x) > abs(delta_y)) {
                        /* Horizontal swipe */
                        direction = (delta_x > 0) ? 1 : -1;  /* Right=+1, Left=-1 */
                        ESP_LOGI(TAG, "Horizontal swipe: delta_x=%d, direction=%d", delta_x, direction);
                    } else {
                        /* Vertical swipe */
                        direction = (delta_y > 0) ? 1 : -1;  /* Up=+1, Down=-1 */
                        ESP_LOGI(TAG, "Vertical swipe: delta_y=%d, direction=%d", delta_y, direction);
                    }
                    
                    /* Handle as selection input */
                    ui_mp3_ui_handle_selection_input(direction);
                    
                    /* Do NOT update tap tracking for swipes - only short taps should be tracked for double-tap */
                } else {
                    /* Short tap - record for potential double tap */
                    s_last_tap_x = point.x;
                    s_last_tap_y = point.y;
                    s_last_tap_time = current_time;
                    ESP_LOGD(TAG, "Single tap recorded at (%d, %d)", point.x, point.y);
                }
            }
            s_touch_active = false;
        }
    }
}

esp_err_t ui_mp3_ui_init(lv_obj_t *parent)
{
    if (!parent) {
        ESP_LOGE(TAG, "Invalid parent object");
        return ESP_ERR_INVALID_ARG;
    }

    /* Create MP3 panel (hidden by default) - full screen overlay */
    s_mp3_panel = lv_obj_create(parent);
    lv_obj_set_size(s_mp3_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_align(s_mp3_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_mp3_panel, LV_OBJ_FLAG_HIDDEN);
    
    /* Panel styling - dark background like other state pages */
    lv_obj_set_style_bg_color(s_mp3_panel, lv_color_hex(0x0e0e10), 0);
    lv_obj_set_style_bg_opa(s_mp3_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_mp3_panel, 0, 0);
    lv_obj_set_style_pad_all(s_mp3_panel, 0, 0);
    
    /* Add touch event handler for swipe gestures */
    lv_obj_add_event_cb(s_mp3_panel, mp3_panel_event_cb, LV_EVENT_ALL, NULL);
    
    /* GIF animation (centered, 172x172 pixels like other states) */
#if LV_USE_GIF
    s_gif = lv_gif_create(s_mp3_panel);
    lv_obj_set_size(s_gif, 172, 172);
    lv_obj_center(s_gif);
    
    /* Set speaking GIF for MP3 playback */
    lv_gif_set_src(s_gif, "S:gifs/speaking.gif");
#else
    ESP_LOGW(TAG, "LV_USE_GIF is disabled, GIF will not be displayed");
#endif
    
    /* Status label (below GIF, like state chip style) */
    s_status_label = lv_label_create(s_mp3_panel);
    lv_label_set_text(s_status_label, "Music Playing");
    lv_obj_set_width(s_status_label, LV_PCT(80));
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_status_label, &SourceHanSansCN_Medium_16, 0);  /* Use Chinese font */
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xe0e0e0), 0);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -40);
    
    ESP_LOGI(TAG, "MP3 UI initialized (GIF + status text layout)");
    return ESP_OK;
}

void ui_mp3_ui_show(const char *track_name, int progress, 
                    int current_sec, int total_sec, bool is_playing)
{
    if (!s_mp3_panel) return;
    
    /* Update status label with track name and play state */
    char status_text[128];
    if (track_name && track_name[0]) {
        const char *state_str = is_playing ? "▶" : "⏸";
        snprintf(status_text, sizeof(status_text), "%s %s", state_str, track_name);
    } else {
        /* No track playing - show selection prompt */
        snprintf(status_text, sizeof(status_text), "请选择歌曲");
    }
    lv_label_set_text(s_status_label, status_text);
    
    /* Show panel */
    lv_obj_clear_flag(s_mp3_panel, LV_OBJ_FLAG_HIDDEN);
    
    ESP_LOGD(TAG, "MP3 UI shown: %s", status_text);
}

void ui_mp3_ui_hide(void)
{
    if (s_mp3_panel) {
        lv_obj_add_flag(s_mp3_panel, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGD(TAG, "MP3 UI hidden");
    }
}

void ui_mp3_ui_update_progress(int progress, int current_sec)
{
    /* Progress update not needed in new GIF + text layout */
    (void)progress;
    (void)current_sec;
}

bool ui_mp3_ui_is_visible(void)
{
    if (!s_mp3_panel) return false;
    return !lv_obj_has_flag(s_mp3_panel, LV_OBJ_FLAG_HIDDEN);
}

void ui_mp3_ui_enter_selection_mode(const char **song_list, int song_count, int *current_index)
{
    if (!song_list || song_count <= 0 || !current_index) {
        ESP_LOGE(TAG, "Invalid parameters for selection mode");
        return;
    }
    
    s_selection_mode = true;
    s_song_list = song_list;
    s_song_count = song_count;
    s_current_index = current_index;
    
    /* Ensure index is valid */
    if (*s_current_index < 0 || *s_current_index >= s_song_count) {
        *s_current_index = 0;
    }
    
    /* Update display to show current selection */
    char status_text[128];
    snprintf(status_text, sizeof(status_text), "%d/%d: %s", 
             *s_current_index + 1, s_song_count, s_song_list[*s_current_index]);
    lv_label_set_text(s_status_label, status_text);
    
    /* Show panel */
    lv_obj_clear_flag(s_mp3_panel, LV_OBJ_FLAG_HIDDEN);
    
    ESP_LOGI(TAG, "Entered selection mode: %d songs, showing #%d", s_song_count, *s_current_index + 1);
}

bool ui_mp3_ui_handle_selection_input(int delta)
{
    if (!s_selection_mode || !s_song_list || !s_current_index) {
        ESP_LOGW(TAG, "Selection input ignored: mode=%d, list=%p, index=%p", 
                 s_selection_mode, s_song_list, s_current_index);
        return false;
    }
    
    ESP_LOGD(TAG, "Selection input: delta=%d, current=%d", delta, *s_current_index);
    
    if (delta != 0) {
        /* Rotate to select different song */
        *s_current_index += delta;
        
        /* Wrap around */
        if (*s_current_index < 0) {
            *s_current_index = s_song_count - 1;
        } else if (*s_current_index >= s_song_count) {
            *s_current_index = 0;
        }
        
        /* Update display */
        char status_text[128];
        snprintf(status_text, sizeof(status_text), "%d/%d: %s", 
                 *s_current_index + 1, s_song_count, s_song_list[*s_current_index]);
        lv_label_set_text(s_status_label, status_text);
        
        ESP_LOGD(TAG, "Selection changed to #%d: %s", *s_current_index + 1, s_song_list[*s_current_index]);
        return false;  /* Not confirmed yet */
    }
    
    /* delta == 0 means button press (confirmed) */
    ESP_LOGI(TAG, "Song selection confirmed: #%d - %s", 
             *s_current_index + 1, s_song_list[*s_current_index]);
    
    /* Exit selection mode */
    s_selection_mode = false;
    s_song_list = NULL;
    s_song_count = 0;
    s_current_index = NULL;
    
    return true;  /* Confirmed */
}
