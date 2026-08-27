/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * Dynamic Island module — iPhone 17 style top pill indicator
 * Shows WiFi, state label, clock, and OpenClaw connection status
 */

#pragma once

#include "lvgl.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the Dynamic Island (top pill indicator)
 * @param parent Parent container (screen)
 * @return ESP_OK on success
 */
esp_err_t ui_state_chip_init(lv_obj_t *parent);

/**
 * @brief Update state display in Dynamic Island
 * @param state_label State name (e.g., "IDLE", "LISTENING")
 * @param chinese_text Chinese text description (e.g., "待机", "录音中")
 * @param color State accent color (used for border glow)
 */
void ui_state_chip_update(const char *state_label, const char *chinese_text, lv_color_t color);

/**
 * @brief Set WiFi connection status icon
 * @param connected true if connected to WiFi
 */
void ui_state_chip_set_wifi(bool connected);

/**
 * @brief Set clock time display
 * @param hour Hour (0-23)
 * @param minute Minute (0-59)
 */
void ui_state_chip_set_clock(uint8_t hour, uint8_t minute);

/**
 * @brief Set OpenClaw connection dot status
 * @param connected true if connected to OpenClaw server
 */
void ui_state_chip_set_oc(bool connected);

/**
 * @brief Get the Dynamic Island LVGL object
 * @return Dynamic Island LVGL object
 */
lv_obj_t *ui_state_chip_get_object(void);

#ifdef __cplusplus
}
#endif
