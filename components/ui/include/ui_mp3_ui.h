/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * MP3 player UI module - Progress bar, track name, control buttons
 */

#pragma once

#include "lvgl.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize MP3 player UI
 * @param parent Parent container (screen)
 * @return ESP_OK on success
 */
esp_err_t ui_mp3_ui_init(lv_obj_t *parent);

/**
 * @brief Show MP3 player interface
 * @param track_name Current track filename
 * @param progress Progress percentage (0-100)
 * @param current_sec Current playback time in seconds
 * @param total_sec Total track duration in seconds
 * @param is_playing true if currently playing
 */
void ui_mp3_ui_show(const char *track_name, int progress, 
                    int current_sec, int total_sec, bool is_playing);

/**
 * @brief Hide MP3 player interface
 */
void ui_mp3_ui_hide(void);

/**
 * @brief Update MP3 progress
 * @param progress Progress percentage (0-100)
 * @param current_sec Current playback time in seconds
 */
void ui_mp3_ui_update_progress(int progress, int current_sec);

/**
 * @brief Check if MP3 UI is visible
 * @return true if visible
 */
bool ui_mp3_ui_is_visible(void);

/**
 * @brief Enter song selection mode (show current selected song)
 * @param song_list Array of song filenames
 * @param song_count Number of songs
 * @param current_index Pointer to current selected index (will be updated by UI)
 */
void ui_mp3_ui_enter_selection_mode(const char **song_list, int song_count, int *current_index);

/**
 * @brief Handle rotary encoder input for song selection
 * @param delta Rotation delta (+1 or -1)
 * @return true if user confirmed selection (button pressed)
 */
bool ui_mp3_ui_handle_selection_input(int delta);

#ifdef __cplusplus
}
#endif
