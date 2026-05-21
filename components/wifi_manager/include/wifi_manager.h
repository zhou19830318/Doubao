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

typedef enum {
    WIFI_STATE_DISCONNECTED = 0,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_FAILED,
} wifi_state_t;

typedef void (*wifi_state_cb_t)(wifi_state_t state);

esp_err_t wifi_manager_init(const char *ssid, const char *password, wifi_state_cb_t cb);
esp_err_t wifi_manager_reconnect(const char *ssid, const char *password);
esp_err_t wifi_manager_start_ap(const char *ssid, const char *password);
esp_err_t wifi_manager_stop_ap(void);
wifi_state_t wifi_manager_get_state(void);
const char *wifi_manager_get_ip(void);
int8_t wifi_manager_get_rssi(void);

#ifdef __cplusplus
}
#endif
