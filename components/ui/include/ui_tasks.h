/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * Tasks screen — shows OpenClaw cron jobs
 */

#pragma once

#include "esp_err.h"
#include "board.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the tasks screen (call after ui_init) */
esp_err_t ui_tasks_init(void);

/** Pass the app event group for navigation callbacks */
void ui_tasks_set_event_group(void *event_group);

/** Show the tasks screen (switch from main) */
void ui_tasks_show(void);

/** Hide the tasks screen (switch back to main) */
void ui_tasks_hide(void);

/** Is the tasks screen currently visible? */
bool ui_tasks_is_visible(void);

/** Scroll the task list (direction: >0 = down, <0 = up) */
void ui_tasks_scroll(int direction);

/** Refresh the task list from current openclaw data */
void ui_tasks_refresh(void);

/** Get the LVGL screen object (for screen management) */
lv_obj_t *ui_tasks_get_screen(void);

#ifdef __cplusplus
}
#endif
