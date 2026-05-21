/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * Background tasks — knob monitor, status updates, TTS playback
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start all background tasks: knob, status, TTS, sleep */
void app_tasks_start(void);

/** Reset the sleep inactivity timer (call on user interaction) */
void app_reset_activity_timer(void);

/** Check if device is in light sleep mode */
bool app_is_sleeping(void);

/** True for ~800ms after waking from sleep (prevents accidental recording) */
bool app_just_woke(void);

/** Trigger deep sleep (full power off, wakes via button press → reboot) */
void app_enter_deep_sleep(void);

#ifdef __cplusplus
}
#endif
