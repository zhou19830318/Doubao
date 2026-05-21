/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Show provisioning QR code screen
 * 
 * Displays a QR code that users can scan to access the WiFi configuration page.
 * The QR code contains the URL http://192.168.4.1 (or custom URL if provided).
 * 
 * @param url Optional custom URL (NULL uses default http://192.168.4.1)
 */
void ui_show_provisioning_qr(const char *url);

/**
 * @brief Hide provisioning QR code screen
 */
void ui_hide_provisioning_qr(void);

/**
 * @brief Check if provisioning screen is currently showing
 * 
 * @return true if showing, false otherwise
 */
bool ui_is_provisioning_shown(void);

#ifdef __cplusplus
}
#endif
