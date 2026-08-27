/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * doubao_resampler — 3:2 polyphase resampler (24kHz → 16kHz).
 *
 * Two phases, straight in the 24kHz domain (no 48kHz upsampling framing):
 *
 *   inputs arrive in groups of 3. After the group's 1st sample, the
 *   output at that exact input position is produced (phase A). After the
 *   2nd sample, the output that lies half a sample EARLIER is produced
 *   (phase B). The 3rd sample produces nothing — 2 outputs per 3 inputs.
 *
 *   phase A: out = Σ_j x[n-j] · sinc_win(j - 7.5)   (delay 7.5)
 *   phase B: out = Σ_j x[n-j] · sinc_win(j - 8.0)   (delay 8.0)
 *   with 16 taps per phase, cutoff fc = 1/3 (8kHz at 24kHz), Kaiser β=6.
 *
 * The previous implementation decomposed coefficients phase-contiguously
 * (phase p got h[0..3] instead of the strided h[p], h[p+3], …) which
 * host-testing exposed as a 3.5× frequency error — a 1kHz sine came out
 * at 3.5kHz, i.e. exactly the "unintelligible voice" symptom.
 *
 * Coefficients are stored delay-reversed (h_rev[t] = g[15-t]) so the
 * inner loop is a plain dot product against history[] (most recent
 * sample at the last index). Q15 fixed-point, int64 accumulator.
 */

#include "doubao_resampler.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define KAISER_BETA   6.0f
#define CUTOFF_NORM   (1.0f / 3.0f)  /* 8kHz at 24kHz = 1/3 of Nyquist */
#define TAPS          RESAMPLE_TAPS_PER_PHASE
#define DELAY_A       (TAPS / 2.0f)              /* 8.0 — integer delay:
                                                  * output at integer input
                                                  * position n-8, produced
                                                  * when input n arrives */
#define DELAY_B       (TAPS / 2.0f + 0.5f)       /* 8.5 — half-sample delay */

static float bessel_i0(float x)
{
    float sum  = 1.0f;
    float term = 1.0f;
    for (int k = 1; k <= 20; k++) {
        term *= (x * x) / (4.0f * k * k);
        sum += term;
        if (term < 1e-7f * sum) break;
    }
    return sum;
}

void resampler_init(resampler_t *r)
{
    if (!r) return;
    memset(r->history, 0, sizeof(r->history));
    r->wr_idx = 0;
    r->phase = 0;

    const float beta = KAISER_BETA;
    const float i0_beta = bessel_i0(beta);
    const int   M = TAPS - 1;

    for (int p = 0; p < RESAMPLE_PHASES; p++) {
        float delay = (p == 0) ? DELAY_A : DELAY_B;
        for (int j = 0; j < TAPS; j++) {
            float n = (float)j - delay;
            float sinc;
            if (fabsf(n) < 1e-6f) {
                sinc = 2.0f * CUTOFF_NORM;
            } else {
                sinc = sinf(2.0f * (float)M_PI * CUTOFF_NORM * n) /
                       ((float)M_PI * n);
            }
            float w_arg = 2.0f * (float)j / (float)M - 1.0f;
            float w = bessel_i0(beta * sqrtf(1.0f - w_arg * w_arg)) / i0_beta;
            /* store delay-reversed: coeffs[p*TAPS + t] weights history[t],
             * where history[t] is the sample at delay (TAPS-1-t). */
            int t = M - j;
            r->coeffs[p * TAPS + t] = (int32_t)(sinc * w * 32768.0f);
        }
    }
}

/* Produce one output for the given phase against the circular history.
 * The history ring is indexed with modular arithmetic: history[(wr - k) & mask]
 * gives the sample at delay k (most recent at wr, oldest at wr-TAPS+1). */
static inline int16_t produce_output(resampler_t *r, int phase)
{
    int64_t acc = 0;
    const int32_t *c = &r->coeffs[phase * TAPS];
    for (int t = 0; t < TAPS; t++) {
        /* history[(wr - (TAPS-1-t)) & mask] = sample at delay (TAPS-1-t)
         * Coefficients are stored delay-reversed: c[t] weights delay (TAPS-1-t).
         * So we want history[(wr - (TAPS-1-t)) & mask] * c[t]. */
        int idx = (r->wr_idx - (TAPS - 1 - t)) & (TAPS - 1);
        acc += (int64_t)r->history[idx] * c[t];
    }
    int32_t s = (int32_t)(acc >> 15);
    if (s > 32767)  s = 32767;
    if (s < -32768) s = -32768;
    return (int16_t)s;
}

/* Push a new input sample into the circular history buffer.
 * No memmove — just advance the write pointer (O(1) vs O(TAPS)). */
static inline void shift_in(resampler_t *r, int16_t sample)
{
    r->wr_idx = (r->wr_idx + 1) & (TAPS - 1);
    r->history[r->wr_idx] = sample;
}

size_t resampler_process(resampler_t *r, const int16_t *in24, size_t n_in,
                         int16_t *out16, size_t cap_out)
{
    if (!r || (!in24 && n_in > 0) || !out16) return 0;

    size_t out_count = 0;

    for (size_t i = 0; i < n_in; i++) {
        shift_in(r, in24[i]);

        /* Output schedule: output m sits at input position 1.5·m. With the
         * 8/8.5-sample filter delays, output 2q   (phase A, integer
         * position) is producible once input n with n ≡ 2 (mod 3) arrives,
         * output 2q+1 (phase B, half position) once n ≡ 1 (mod 3) arrives.
         * n ≡ 0 produces nothing — 2 outputs per 3 inputs. */
        switch (r->phase) {
        case 0:
            r->phase = 1;   /* nothing */
            break;
        case 1:
            if (out_count < cap_out) {
                out16[out_count++] = produce_output(r, 1);   /* phase B */
            }
            r->phase = 2;
            break;
        default:
            if (out_count < cap_out) {
                out16[out_count++] = produce_output(r, 0);   /* phase A */
            }
            r->phase = 0;
            break;
        }
    }

    return out_count;
}
