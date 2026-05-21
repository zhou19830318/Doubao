/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * MP3 Player — SD card MP3 playback via esp_audio_simple_player
 */

#pragma once

#include "esp_err.h"
#include "esp_audio_simple_player.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MP3_FILE_NAME_MAX 100
#define MP3_FILES_MAX      100

/** Playback state for UI */
typedef enum {
    MP3_STATE_IDLE = 0,
    MP3_STATE_PLAYING,
    MP3_STATE_PAUSED,
    MP3_STATE_STOPPED,
} mp3_playback_state_t;

/** Callback for state changes (called from audio pipeline task, NOT LVGL) */
typedef void (*mp3_state_cb_t)(mp3_playback_state_t state, const char *filename);

/** Callback for playback completion */
typedef void (*mp3_completion_cb_t)(void);

/**
 * Initialize MP3 player pipeline (esp_audio_simple_player)
 */
esp_err_t mp3_player_init(void);

/**
 * Play an MP3 file from SD card
 * @param filename  filename under /sdcard/, e.g. "song.mp3"
 */
esp_err_t mp3_player_play(const char *filename);

/** Stop playback */
esp_err_t mp3_player_stop(void);

/** Pause playback */
esp_err_t mp3_player_pause(void);

/** Resume paused playback */
esp_err_t mp3_player_resume(void);

/** Get current state */
mp3_playback_state_t mp3_player_get_state(void);

/** Get currently playing filename (empty string if idle) */
const char *mp3_player_get_current_file(void);

/** Get current playback position in seconds (approximate) */
uint32_t mp3_player_get_position_sec(void);

/** Get total duration in seconds (0 if unknown) */
uint32_t mp3_player_get_duration_sec(void);

/**
 * Scan SD card directory for MP3 files (fixed-size array version)
 * @return number of files found
 */
uint16_t mp3_player_scan_sd(const char *directory, char file_names[][MP3_FILE_NAME_MAX], uint16_t max_files);

/**
 * Scan SD card directory for MP3 files (dynamic allocation version)
 * @param directory Directory to scan
 * @param[out] out_files Pointer to receive dynamically allocated array of filenames
 * @param[out] out_count Pointer to receive the number of files found
 * @return ESP_OK on success, error code otherwise
 * @note Caller must free(*out_files) when done
 */
esp_err_t mp3_player_scan_sd_dynamic(const char *directory, char ***out_files, uint16_t *out_count);

/** Set state change callback */
void mp3_player_set_state_cb(mp3_state_cb_t cb);

/** Set completion callback (fired when song ends naturally) */
void mp3_player_set_completion_cb(mp3_completion_cb_t cb);

#ifdef __cplusplus
}
#endif
