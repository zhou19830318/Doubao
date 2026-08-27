/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * doubao_audio_out — Downlink playback ring + task (Task 10).
 *
 * Receives resampled 16kHz PCM from AUDIO_DELTA events, pushes into a
 * 64KB PSRAM ring buffer.  A high-priority playback task drains the ring
 * into board_audio_play().  Anti-pop fades on start/stop prevent DC
 *阶跃 pops (CLAUDE.md rule 14).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Push resampled 16kHz PCM into the playback ring buffer.
 *        Called from the ws_client task (AUDIO_DELTA handler).
 *        Overload: drops oldest frames (realtime over fidelity).
 *
 * @param pcm16    16kHz/16bit PCM samples (post-resample).
 * @param samples  Number of samples.
 * @return ESP_OK on success, ESP_ERR_NO_MEM if ring full (data dropped).
 */
esp_err_t dbaudio_out_push(const int16_t *pcm16, size_t samples);

/**
 * @brief Start the playback task (pinned to core 1, priority 10).
 *        Must be called once before first push. Idempotent.
 */
esp_err_t dbaudio_out_start(void);

/**
 * @brief Stop playback with a 50ms fade-out (anti-pop).
 *        Blocks up to 100ms for fade to complete.
 */
esp_err_t dbaudio_out_stop(void);

/**
 * @brief Immediate interrupt: fade-out in ≤20ms, discard the buffered
 *        backlog and stop. Used by barge-in (response.cancel).
 */
void dbaudio_out_interrupt(void);

/**
 * @brief Graceful stop (audio.done): play out everything still buffered
 *        at the normal real-time cadence, then fade out and exit.
 *        Non-blocking. Contrast with dbaudio_out_stop(), which cuts off
 *        immediately.
 */
void dbaudio_out_drain_stop(void);

/**
 * @brief Play a short feedback tone (叮咚/咚叮) through the play task
 *        (priority 10 — immune to RX-processing preemption that stutters
 *        low-priority direct writes). A silent pre-pad consumes the
 *        fade-in ramp so the tone starts at full amplitude. Blocks
 *        (bounded) until the tone has played out.
 */
esp_err_t dbaudio_out_play_tone(const int16_t *pcm, size_t samples);

/**
 * @brief Current RMS of the most recently played 16kHz PCM block.
 *        Used by interrupt detection (compare with mic RMS).
 * @return RMS value (linear, 0–32767 range).
 */
float dbaudio_out_current_rms(void);

/**
 * @brief Check if the playback task is currently running and has data.
 */
bool dbaudio_out_is_playing(void);

/**
 * @brief Current number of samples in the ring buffer (for diagnostics).
 */
int dbaudio_out_ring_depth(void);

#ifdef __cplusplus
}
#endif
