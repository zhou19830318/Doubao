/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
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

/**
 * Register callback invoked after the Doubao API key is updated via
 * POST /api/doubao and persisted to NVS. Called from the web server task.
 * Setter injection (模式同 ui_set_event_group)：main 注册 = doubao 断开重连，
 * webserver 不依赖 doubao_voice 组件。
 */
void webserver_set_doubao_changed_cb(void (*cb)(void));

#ifdef __cplusplus
}
#endif
