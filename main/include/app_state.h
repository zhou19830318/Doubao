/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
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
#define OC_CONNECTED_BIT    BIT1
#define KNOB_PRESSED_BIT    BIT2
#define TTS_PLAY_BIT        BIT3
#define WEBSERVER_TOGGLE_BIT BIT4
#define DETAILS_BIT         BIT5
#define CANCEL_BIT          BIT6
#define TOUCH_BIT           BIT7
#define TASKS_SCREEN_BIT    BIT8
#define CAMERA_BIT          BIT9
#define WAKE_WORD_BIT       BIT10
#define MP3_PLAYER_BIT      BIT11
#define MP3_CMD_BIT         BIT12

/** Global event group — created in app_main */
extern EventGroupHandle_t g_app_events;

/** Recording flag */
extern bool g_recording;

/** Response shown timestamp (for auto-return to IDLE) */
extern int64_t g_response_shown_at;

/** Pending camera image for next voice chat (PSRAM-allocated, caller frees) */
extern uint8_t *g_pending_jpeg;
extern size_t   g_pending_jpeg_size;

/** Auto-listen flag: set true when OpenClaw expects a follow-up reply */
extern bool g_continue_listening;

/** TTS pending flag: set true when TTS_PLAY_BIT is queued, prevents premature dismissal */
extern volatile bool g_tts_pending;

/** Transition to a new UI state + update RGB LED */
void app_set_state(ui_state_t st);

/** LED color for given UI state */
void app_led_for_state(ui_state_t st);

/** Chat response callback (used by voice_chat and serial_cmd) */
void app_on_chat_response(const char *text, bool is_final);

/** Parse [DEVICE:...] commands from text buffer (modifies buffer in-place) */
int parse_device_commands(char *buf);

/** Queue a deferred MP3 command for execution in the main task context.
 *  Safe to call from any task (websocket, etc.) without stack overflow risk. */
void app_queue_mp3_cmd(const char *cmd_with_args);

/** Process any pending deferred MP3 command. Call from main loop only. */
void app_process_mp3_cmd(void);

/** Scan SD card for MP3 files and populate the cache. Call once during boot. */
void app_sd_mp3_scan_init(void);

/** Get cached SD card MP3 file list as a compact string (e.g. "1:song.mp3, 2:music.mp3").
 *  Scanned at boot and refreshed on [DEVICE:mp3=scan]. Returns "" if no files. */
const char *app_get_sd_mp3_list_str(void);

/** Get cached SD card MP3 file count */
int app_get_sd_mp3_count(void);

/** Get cached SD card MP3 filename by index (0-based). Returns NULL if out of range. */
const char *app_get_sd_mp3_name(int index);

/** Cleanup function to free dynamically allocated MP3 file list. Call during shutdown. */
void app_sd_mp3_cleanup(void);

#ifdef __cplusplus
}
#endif
