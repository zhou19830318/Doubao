/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * vad — Adaptive noise-floor Voice Activity Detection (Task 9).
 *
 * Algorithm (ported from Ver2.0 voice_chat.c):
 *   1. DC offset removal (running average)
 *   2. Frame RMS computation
 *   3. Noise calibration: first 16 frames (640ms) compute noise floor
 *   4. Adaptive threshold: noise_floor × 2 (≈6dB above noise floor)
 *   5. Speech detected when RMS > threshold
 *   6. Silence timer: consecutive silent frames → commit
 *   7. Max timeout: force commit after max_record_ms
 *
 * CLAUDE.md rules applied:
 *   8 — This runs in the capture task context; no locks/mallocs here.
 */

#include "vad.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"

/* ── Configuration ───────────────────────────────────────────────────── */

#define CALIBRATION_FRAMES   16    /* 16 × 40ms = 640ms calibration */
#define FRAME_MS             40    /* one frame duration */
#define DEFAULT_SILENCE_MS   1500  /* 1.5s silence → commit */
#define DEFAULT_MAX_MS       15000 /* 15s max recording */
#define NOISE_MARGIN_DB      6.0f  /* speech threshold = noise_floor + 6dB */
#define MIN_ENERGY_RMS       80.0f /* below this, treat as silence (ambient) */

/* ── State ───────────────────────────────────────────────────────────── */

static float    s_noise_floor;       /* adaptive noise floor (RMS) */
static int      s_calib_count;       /* frames in calibration phase */
static float    s_calib_sum;         /* sum of RMS during calibration */
static bool     s_calibrated;        /* true after CALIBRATION_FRAMES */

static float    s_current_rms;       /* latest frame RMS */
static float    s_threshold;         /* noise_floor × 2 (linear) */

static int32_t  s_dc_offset;         /* running DC offset (Q15-ish) */
static bool     s_has_speech;        /* speech detected at least once */

static int      s_silence_frames;    /* consecutive silent frames */
static int      s_total_frames;      /* total frames this turn */

static int16_t  s_silence_ms;        /* configured silence timeout */
static int16_t  s_max_ms;            /* configured max recording */

/* ── Helpers ─────────────────────────────────────────────────────────── */

static float frame_rms(const int16_t *pcm, size_t n, int32_t *dc)
{
    if (n == 0) return 0.0f;

    /* DC offset removal (running average, slow adaptation) */
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += pcm[i];
    }
    float frame_mean = (float)sum / (float)n;
    /* Update DC offset with exponential moving average */
    *dc = (int32_t)((float)*dc * 0.99f + frame_mean * 0.01f);

    /* RMS after DC removal */
    float dc_f = (float)*dc;
    float sum_sq = 0.0f;
    for (size_t i = 0; i < n; i++) {
        float v = (float)pcm[i] - dc_f;
        sum_sq += v * v;
    }
    return sqrtf(sum_sq / (float)n);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void vad_init(int16_t silence_ms, int16_t max_record_ms)
{
    s_silence_ms = (silence_ms > 0) ? silence_ms : DEFAULT_SILENCE_MS;
    s_max_ms     = (max_record_ms > 0) ? max_record_ms : DEFAULT_MAX_MS;
    vad_reset();
}

void vad_reset(void)
{
    s_noise_floor   = 0.0f;
    s_calib_count   = 0;
    s_calib_sum     = 0.0f;
    s_calibrated    = false;
    s_current_rms   = 0.0f;
    s_threshold     = MIN_ENERGY_RMS;
    s_dc_offset     = 0;
    s_has_speech    = false;
    s_silence_frames = 0;
    s_total_frames  = 0;
}

vad_state_t vad_process(const int16_t *pcm, size_t samples)
{
    if (!pcm || samples == 0) return VAD_SPEECH;

    s_total_frames++;
    s_current_rms = frame_rms(pcm, samples, &s_dc_offset);

    /* Calibration phase: build noise floor estimate */
    if (!s_calibrated) {
        s_calib_sum += s_current_rms;
        s_calib_count++;
        if (s_calib_count >= CALIBRATION_FRAMES) {
            s_noise_floor = s_calib_sum / (float)s_calib_count;
            if (s_noise_floor < MIN_ENERGY_RMS) {
                s_noise_floor = MIN_ENERGY_RMS;
            }
            s_threshold = s_noise_floor * 2.0f;  /* ≈6dB above noise */
            s_calibrated = true;
            ESP_LOGI("vad", "calibrated: noise_floor=%.0f threshold=%.0f",
                     s_noise_floor, s_threshold);
        }
        /* During calibration, always report SPEECH (keep recording) */
        return VAD_SPEECH;
    }

    /* Check max recording time */
    int max_frames = s_max_ms / FRAME_MS;
    if (s_total_frames >= max_frames) {
        ESP_LOGI("vad", "max timeout (%dms) reached", s_max_ms);
        return VAD_MAX_TIMEOUT;
    }

    /* Speech detection */
    bool is_speech = (s_current_rms > s_threshold);
    if (is_speech) {
        s_has_speech = true;
        s_silence_frames = 0;
        return VAD_SPEECH;
    }

    /* Silence counting */
    s_silence_frames++;
    int silence_frames_needed = s_silence_ms / FRAME_MS;

    /* Only commit silence if we had speech before (avoid false commit
     * on quiet ambient noise before user speaks) */
    if (s_has_speech && s_silence_frames >= silence_frames_needed) {
        ESP_LOGD("vad", "silence %dms (%d frames) after speech → commit",
                 s_silence_frames * FRAME_MS, s_silence_frames);
        return VAD_SILENCE;
    }

    return VAD_SPEECH;
}

float vad_rms(void)
{
    return s_current_rms;
}

bool vad_has_speech(void)
{
    return s_has_speech;
}
