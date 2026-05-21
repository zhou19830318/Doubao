/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * State chip module - Bottom state indicator
 */

#pragma once

#include "lvgl.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the state chip
 * @param parent Parent container (screen)
 * @return ESP_OK on success
 */
esp_err_t ui_state_chip_init(lv_obj_t *parent);

/**
 * @brief Update state display
 * @param state_label State name (e.g., "IDLE", "LISTENING")
 * @param chinese_text Chinese text description (e.g., "待机", "录音中")
 * @param color State accent color
 */
void ui_state_chip_update(const char *state_label, const char *chinese_text, lv_color_t color);

/**
 * @brief Get the state chip object
 * @return State chip LVGL object
 */
lv_obj_t *ui_state_chip_get_object(void);

#ifdef __cplusplus
}
#endif
