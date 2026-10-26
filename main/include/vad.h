/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * vad — Adaptive noise-floor Voice Activity Detection (Task 9).
 *
 * Ported from Ver2.0 voice_chat.c with enhancements:
 *   - 16-frame noise calibration at start
 *   - DC offset removal
 *   - Adaptive threshold (noise_floor + 6dB margin)
 *   - Silence timeout (default 1.5s) and max recording (default 15s)
 *   - RMS exposed for interrupt detection (Task 11)
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VAD_SPEECH,       /* voice detected (ongoing) */
    VAD_SILENCE,      /* silence timeout reached → commit */
    VAD_MAX_TIMEOUT,  /* max recording time reached → commit */
} vad_state_t;

/**
 * @brief Initialize VAD with timing parameters.
 * @param silence_ms    Silence duration to trigger commit (default 1500ms).
 * @param max_record_ms Max recording duration (default 15000ms).
 */
void vad_init(int16_t silence_ms, int16_t max_record_ms);

/**
 * @brief Reset VAD state for a new recording turn.
 *        Must be called before first vad_process() of each turn.
 */
void vad_reset(void);

/**
 * @brief Process one 40ms frame (640 samples @16kHz).
 * @param pcm     PCM samples (16-bit signed).
 * @param samples Number of samples (typically 640).
 * @return VAD_SPEECH, VAD_SILENCE, or VAD_MAX_TIMEOUT.
 */
vad_state_t vad_process(const int16_t *pcm, size_t samples);

/**
 * @brief Current RMS level (linear, 0–32767).
 *        Updated by vad_process(); used by interrupt detection.
 */
float vad_rms(void);

/**
 * @brief Whether VAD has detected speech at least once in this turn.
 */
bool vad_has_speech(void);

#ifdef __cplusplus
}
#endif
