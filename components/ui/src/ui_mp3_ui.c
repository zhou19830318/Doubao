/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * MP3 player UI — stub (MP3 feature removed to save memory).
 */

#include "ui_mp3_ui.h"

esp_err_t ui_mp3_ui_init(lv_obj_t *parent)
{
    (void)parent;
    return ESP_OK;
}

void ui_mp3_ui_show(const char *track_name, int progress,
                    int current_sec, int total_sec, bool is_playing)
{
    (void)track_name; (void)progress; (void)current_sec;
    (void)total_sec; (void)is_playing;
}

void ui_mp3_ui_hide(void) {}

void ui_mp3_ui_update_progress(int progress, int current_sec)
{
    (void)progress; (void)current_sec;
}

void ui_mp3_ui_update_playback(int current_sec, int total_sec, bool is_playing)
{
    (void)current_sec; (void)total_sec; (void)is_playing;
}

bool ui_mp3_ui_is_visible(void) { return false; }

void ui_mp3_ui_enter_selection_mode(const char **song_list, int song_count,
                                     int *current_index)
{
    (void)song_list; (void)song_count; (void)current_index;
}

bool ui_mp3_ui_handle_selection_input(int delta)
{
    (void)delta;
    return false;
}

bool ui_mp3_ui_consume_selection_confirmed(void) { return false; }
