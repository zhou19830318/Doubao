/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * Web server — configuration UI + status API
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start the web server on port 80 */
esp_err_t webserver_start(void);

/** Stop the web server */
esp_err_t webserver_stop(void);

/** Check if web server is running */
bool webserver_is_running(void);

#ifdef __cplusplus
}
#endif
