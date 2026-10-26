/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * Shared application state — event groups, helpers, state transitions
 */

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "ui.h"
#include "ui_mp3_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Event bits */
#define WIFI_CONNECTED_BIT  BIT0
#define KNOB_PRESSED_BIT    BIT2
#define TTS_PLAY_BIT        BIT3
#define CANCEL_BIT          BIT6
#define TOUCH_BIT           BIT7
#define WAKE_WORD_BIT       BIT10
#define DOUBAO_START_BIT    BIT14
#define SETTINGS_LONG_PRESS_BIT  BIT15
#define DOUBLE_TAP_BIT       BIT16

/** Global event group — created in app_main */
extern EventGroupHandle_t g_app_events;

/** Recording flag */
extern bool g_recording;

/** Response shown timestamp (for auto-return to IDLE) */
extern int64_t g_response_shown_at;

/** Auto-listen flag: set true when the device should continue listening */
extern bool g_continue_listening;

/** TTS pending flag */
extern volatile bool g_tts_pending;

/** Transition to a new UI state + update RGB LED */
void app_set_state(ui_state_t st);

/** LED color for given UI state */
void app_led_for_state(ui_state_t st);

/** Chat response callback */
void app_on_chat_response(const char *text, bool is_final);

/** Parse [DEVICE:...] commands from text buffer (modifies buffer in-place) */
int parse_device_commands(char *buf);

/** Interpret a user ASR transcript as a local volume/brightness command.
 * Returns 1 when a command was applied, 0 when the text carried none. */
int handle_local_voice_control(const char *text);

#ifdef __cplusplus
}
#endif
