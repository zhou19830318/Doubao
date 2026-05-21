/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * Status bar module - Top status bar with WiFi, clock, OC indicator
 */

#pragma once

#include "lvgl.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the status bar
 * @param parent Parent container (screen)
 * @return ESP_OK on success
 */
esp_err_t ui_status_bar_init(lv_obj_t *parent);

/**
 * @brief Update WiFi status
 * @param connected true if connected
 * @param rssi Signal strength (not used for display, kept for API compatibility)
 */
void ui_status_bar_set_wifi(bool connected, int rssi);

/**
 * @brief Update OpenClaw connection status
 * @param connected true if connected to OpenClaw
 */
void ui_status_bar_set_oc_connected(bool connected);

/**
 * @brief Update clock display
 * @param hour Hour (0-23)
 * @param minute Minute (0-59)
 */
void ui_status_bar_set_clock(uint8_t hour, uint8_t minute);

/**
 * @brief Get the status bar object
 * @return Status bar LVGL object
 */
lv_obj_t *ui_status_bar_get_object(void);

#ifdef __cplusplus
}
#endif
