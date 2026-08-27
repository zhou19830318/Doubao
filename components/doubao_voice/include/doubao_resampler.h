/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * doubao_resampler — 3:2 polyphase resampler (24kHz → 16kHz).
 * Used on the downlink path: Doubao sends 24kHz PCM, codec runs at 16kHz.
 *
 * Design (rewritten 2026-08-21 after the original was host-tested broken —
 * a 1kHz input came out at 3.5kHz): two phases in the 24kHz domain.
 *   Phase A: output lands on an integer input position  (every 3rd input
 *            sample, at group offset 0)
 *   Phase B: output lands half a sample early              (group offset 1)
 *   Group offset 2 produces nothing → 2 outputs per 3 inputs.
 * Each phase is a windowed-sinc FIR (Kaiser β=6, cutoff 8kHz) of
 * RESAMPLE_TAPS_PER_PHASE taps at 24kHz, Q15 arithmetic.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RESAMPLE_TAPS_PER_PHASE 16
#define RESAMPLE_PHASES         2
#define RESAMPLE_TOTAL_TAPS     (RESAMPLE_PHASES * RESAMPLE_TAPS_PER_PHASE)

typedef struct {
    int32_t coeffs[RESAMPLE_TOTAL_TAPS];   /* Q15, phase A then phase B,
                                            * stored delay-reversed.  Keep in
                                            * internal RAM — only 128 bytes
                                            * and the dot-product inner loop
                                            * hits these on every tap. */
    int16_t history[RESAMPLE_TAPS_PER_PHASE];  /* Circular ring buffer of last
                                                * 16 inputs. Most recent sample
                                                * at history[wr_idx]. Replaces
                                                * the old memmove shift — saves
                                                * ~15 pointer moves per input
                                                * sample (~43k memmoves/chunk).
                                                * Keep in internal RAM (32B).
                                                */
    int     wr_idx;       /* write index into history[] (circular) */
    int     phase;        /* group offset counter 0..2 */
} resampler_t;

/* Initialise filter coefficients (windowed-sinc, Kaiser β=6).
 * Must be called once before first resampler_process(). */
void resampler_init(resampler_t *r);

/**
 * @brief Resample 24kHz PCM → 16kHz PCM (3:2 ratio).
 *
 * For every 3 input samples, 2 output samples are produced. The whole
 * input range is consumed (phase and history carry across calls), so the
 * caller must size cap_out to fit floor(2*n_in/3)+2 outputs.
 *
 * @param r       Resampler state (init'd once, kept across calls).
 * @param in24    Input PCM samples at 24kHz, 16-bit signed.
 * @param n_in    Number of input samples.
 * @param out16   Output buffer for 16kHz samples.
 * @param cap_out Capacity of out16 in samples.
 * @return        Number of output samples actually written.
 */
size_t resampler_process(resampler_t *r, const int16_t *in24, size_t n_in,
                         int16_t *out16, size_t cap_out);

#ifdef __cplusplus
}
#endif
