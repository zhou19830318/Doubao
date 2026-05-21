/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start captive portal DNS server
 * 
 * This will intercept all DNS queries and redirect them to the device's
 * AP IP address (192.168.4.1), forcing mobile devices to automatically
 * open the configuration page.
 * 
 * @return ESP_OK on success
 */
esp_err_t captive_portal_start(void);

/**
 * @brief Stop captive portal DNS server
 * 
 * @return ESP_OK on success
 */
esp_err_t captive_portal_stop(void);

/**
 * @brief Check if captive portal is running
 * 
 * @return true if running, false otherwise
 */
bool captive_portal_is_running(void);

#ifdef __cplusplus
}
#endif
