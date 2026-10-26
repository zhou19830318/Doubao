/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * doubao_audio_in — Uplink capture task + send queue (Task 8).
 *
 * Captures 16kHz/16bit mono mic via board_audio_record(), enqueues 640-sample
 * (40ms) frames into a 12-frame ring buffer.  The ws_client task drains the
 * queue and sends each frame via proto_build_audio_append + dbws_send_frame.
 * Overflow drops oldest frames (early audio has least ASR impact).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Frame size: 640 samples = 40ms @ 16kHz (per design doc §3) */
#define DBAUDIO_FRAME_SAMPLES  640

/**
 * @brief Start the capture task (core 1, priority 9).
 *        Captures mic audio into the send queue. Idempotent.
 */
esp_err_t dbaudio_in_start(void);

/**
 * @brief Stop the capture task and flush the send queue.
 */
esp_err_t dbaudio_in_stop(void);

/**
 * @brief Reset the send queue (clear stale frames before a new turn).
 */
void dbaudio_in_reset_queue(void);

/**
 * @brief Dequeue one frame for sending (called from ws_client task).
 *
 * @param dst      Destination buffer (must be ≥ FRAME_SAMPLES int16_t).
 * @param max_samp Capacity of dst in samples.
 * @return Number of samples dequeued (0 if queue empty).
 */
size_t dbaudio_in_dequeue(int16_t *dst, size_t max_samp);

/**
 * @brief Number of frames currently in the send queue.
 */
int dbaudio_in_queue_depth(void);

/**
 * @brief Total frames dropped due to overflow (for diagnostics).
 */
int dbaudio_in_drop_count(void);
bool dbaudio_in_is_running(void);

#ifdef __cplusplus
}
#endif
