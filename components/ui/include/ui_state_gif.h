/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * UI State GIF Manager - Displays animated GIFs for different UI states
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/** GIF state type mapping to UI states */
typedef enum {
    GIF_STATE_SLEEPING,     /* UI_STATE_SLEEP / UI_STATE_ARMED */
    GIF_STATE_BOOT,         /* UI_STATE_BOOT */
    GIF_STATE_CONNECTING,   /* UI_STATE_CONNECTING */
    GIF_STATE_IDLE,         /* UI_STATE_IDLE */
    GIF_STATE_LISTENING,    /* UI_STATE_LISTENING */
    GIF_STATE_SENDING,      /* UI_STATE_SENDING */
    GIF_STATE_THINKING,     /* UI_STATE_THINKING */
    GIF_STATE_PLAYING,      /* UI_STATE_PLAYING_MP3 */
    GIF_STATE_RESPONSE,     /* UI_STATE_RESPONSE */
    GIF_STATE_SPEAKING,     /* UI_STATE_TTS_PLAYING */
    GIF_STATE_ERROR,        /* UI_STATE_ERROR */
    GIF_STATE_NOTIFYING,    /* UI_STATE_NOTIFYING */
    GIF_STATE_COUNT
} gif_state_type_t;

/**
 * Initialize the GIF state manager
 * Must be called after LVGL and SD card are initialized
 * @return ESP_OK on success
 */
esp_err_t ui_state_gif_init(void);

/**
 * Show GIF for the specified UI state
 * Automatically maps UI state to corresponding GIF
 * @param state Current UI state
 */
void ui_state_gif_show_for_state(ui_state_t state);

/**
 * Hide the currently displayed GIF
 */
void ui_state_gif_hide(void);

/**
 * Manually show a specific GIF by type
 * @param gif_type The GIF state type to display
 * @return ESP_OK on success
 */
esp_err_t ui_state_gif_show(gif_state_type_t gif_type);

/**
 * Get the current GIF object (for debugging)
 * @return Pointer to current GIF LVGL object, or NULL if none
 */
lv_obj_t *ui_state_gif_get_current(void);

#ifdef __cplusplus
}
#endif
