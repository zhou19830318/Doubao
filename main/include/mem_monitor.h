/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * Memory monitoring utilities header
 */

#ifndef MEM_MONITOR_H
#define MEM_MONITOR_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Take a memory snapshot and log it
 * @param context Description of the current context
 */
void mem_monitor_snapshot(const char *context);

/**
 * Log task statistics including stack usage
 */
void mem_monitor_task_stats(void);

/**
 * Print heap capabilities summary
 */
void mem_monitor_caps_summary(void);

#ifdef __cplusplus
}
#endif

#endif /* MEM_MONITOR_H */