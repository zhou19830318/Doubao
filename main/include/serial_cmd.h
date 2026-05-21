/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * Serial console — command processing task
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Start serial command processing task (call once from app_main) */
void serial_cmd_task_start(void);

#ifdef __cplusplus
}
#endif
