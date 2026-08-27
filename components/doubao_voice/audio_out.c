/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * doubao_audio_out — Downlink playback ring buffer + task (Task 10).
 *
 * Ring buffer: 64KB PSRAM, stores 16kHz/16bit PCM (post-resample).
 * Playback task: core 1, priority 10 (highest), drains ring → board_audio_play().
 * Anti-pop: 50ms fade-in on start, 50ms fade-out on stop, 20ms on interrupt.
 *
 * CLAUDE.md rules applied:
 *   13 — ring buffer absorbs network jitter; DMA total 80-150ms
 *   14 — anti-pop fades prevent DC阶跃
 *   15 — playback RMS exposed for interrupt detection
 *   7  — task TCB internal RAM, stack PSRAM 16KB (铁律3)
 */

#include "doubao_audio_out.h"
#include "board.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "doubao_task_mem.h"

static const char *TAG = "doubao_aout";

/* ── Ring buffer (PSRAM, power-of-two for fast mod) ────────────────────
 * 524288 samples = 1MB PSRAM ≈ 32.8s @ 16kHz. The server streams TTS in
 * bursts faster than real-time (measured 2.1×: 4.8s of audio in 2.3s) —
 * the old 2s ring overflowed on every response and the drop-oldest
 * policy skipped chunks of speech ("断断续续"). 32.8s absorbs any
 * realistic response incl. long song segments without overflow. */

#define RING_CAP_SAMPLES  (512 * 1024)
#define RING_CAP_MASK     (RING_CAP_SAMPLES - 1)
#define BLOCK_SAMPLES     640            /* 40ms @ 16kHz — I2S write chunk */

static int16_t  *s_ring;                /* PSRAM */
static volatile size_t s_ring_head;     /* write index (producer: dispatch) */
static volatile size_t s_ring_tail;     /* read index  (consumer: play task) */
static volatile size_t s_ring_count;    /* available samples (both tasks access) */
static SemaphoreHandle_t s_ring_mutex;  /* protects during reset/overflow */
static bool s_ring_allocd = false;

/* ── Playback task ───────────────────────────────────────────────────── */

static TaskHandle_t  s_play_task;
static volatile bool s_play_running = false;
static volatile bool s_stop_req     = false;
static volatile bool s_interrupt_req = false;
static volatile bool s_drain_req    = false;  /* audio.done received: play
                                                 out the backlog, then exit */
static volatile bool s_skip_prebuf  = false;  /* tone playback: start
                                                 immediately, no 0.5s pre-buffer */

/* Anti-pop fade state */
#define FADE_SAMPLES  800   /* 50ms @ 16kHz */
#define FADE_INTR_SAMPLES 320  /* 20ms @ 16kHz for interrupt */
#define PREBUF_SAMPLES  8192  /* 0.5s @ 16kHz — pre-buffer before first play */

static float s_current_rms;

static inline size_t ring_available(void)
{
    return s_ring_count;
}

static inline void ring_push_block_nolock(const int16_t *src, size_t n)
{
    /* Two-part memcpy: handles wrap-around without per-sample loop.
     * PSRAM sequential writes benefit from burst transfers — the SPI
     * controller pipelines 64-byte cache lines, so a memcpy of 640
     * samples (1280 bytes) completes in ~10μs vs ~30μs for a per-sample
     * loop with mask + increment on each iteration. */
    size_t head = s_ring_head & RING_CAP_MASK;
    size_t first = RING_CAP_SAMPLES - head;
    if (first > n) first = n;
    memcpy(&s_ring[head], src, first * sizeof(int16_t));
    if (n > first) {
        memcpy(&s_ring[0], src + first, (n - first) * sizeof(int16_t));
    }
    s_ring_head += n;
    s_ring_count += n;
}

/* C3 fix: ring_pop_block now acquires s_ring_mutex to prevent races
 * with dbaudio_out_push() which modifies s_ring_count and may advance
 * s_ring_tail on overflow. Without this, the play task could read
 * partially-overwritten data or observe inconsistent s_ring_count.
 * The play task (prio 10) will briefly wait while the push completes
 * its memcpy (~5μs), which is acceptable for data integrity. */
static inline size_t ring_pop_block(int16_t *dst, size_t cap)
{
    if (s_ring_mutex) xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    size_t avail = s_ring_count;
    size_t n = (avail < cap) ? avail : cap;
    if (n == 0) {
        if (s_ring_mutex) xSemaphoreGive(s_ring_mutex);
        return 0;
    }
    size_t tail = s_ring_tail & RING_CAP_MASK;
    size_t first = RING_CAP_SAMPLES - tail;
    if (first > n) first = n;
    memcpy(dst, &s_ring[tail], first * sizeof(int16_t));
    if (n > first) {
        memcpy(dst + first, &s_ring[0], (n - first) * sizeof(int16_t));
    }
    s_ring_tail += n;
    s_ring_count -= n;
    if (s_ring_mutex) xSemaphoreGive(s_ring_mutex);
    return n;
}

/* C3 fix: clear the ring buffer under the mutex — used by interrupt
 * handler to discard backlog atomically with respect to push() on
 * another core. Must be called AFTER ring_pop_block has returned
 * (mutex was released). */
static inline void ring_clear_locked(void)
{
    if (s_ring_mutex) xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    s_ring_tail = s_ring_head;
    s_ring_count = 0;
    if (s_ring_mutex) xSemaphoreGive(s_ring_mutex);
}

/* Compute RMS of a PCM block */
static float compute_rms(const int16_t *pcm, size_t n)
{
    if (n == 0) return 0.0f;
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += (int32_t)pcm[i] * pcm[i];
    }
    return sqrtf((float)sum / (float)n);
}

/* Apply linear fade to a block in-place */
static void apply_fade(int16_t *pcm, size_t n, size_t fade_total,
                       size_t fade_pos, bool fade_in)
{
    for (size_t i = 0; i < n; i++) {
        size_t global_pos = fade_pos + i;
        if (global_pos >= fade_total) break;
        float t = (float)global_pos / (float)fade_total;
        float gain = fade_in ? t : (1.0f - t);
        pcm[i] = (int16_t)((float)pcm[i] * gain);
    }
}


static void play_task(void *arg)
{
    (void)arg;
    /* Block buffer in INTERNAL DMA RAM: this is the hot-path buffer that
     * the play task reads from the ring and writes to I2S every 40ms.
 * PSRAM access adds ~40ns per sample (16 taps × 640 samples = 10k reads
 * per block) which compounds to measurable play-loop slowness. Internal
 * DMA RAM is only 1280 bytes — easily fits the remaining ~21KB. */
    int16_t *block = heap_caps_calloc(1, BLOCK_SAMPLES * sizeof(int16_t),
                                       MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!block) {
        ESP_LOGE(TAG, "OOM for play block (internal DMA RAM)");
        s_play_running = false;
        vTaskDeleteWithCaps(NULL);  /* PSRAM stack — see doubao_task_mem.h */
        return;
    }

    ESP_LOGI(TAG, "play task started (ring=%d samples / %dKB)",
             RING_CAP_SAMPLES, RING_CAP_SAMPLES * 2 / 1024);

    uint32_t play_loop_cnt = 0;

    /* Anti-pop: write a short silence burst at start (50ms */
    {
        int16_t sil[640];  /* 40ms @ 16kHz, stack is fine for this */
        memset(sil, 0, sizeof(sil));
        size_t written = 0;
        board_audio_play(sil, 640, &written);
    }

    /* Fade-in bookkeeping: the first FADE_SAMPLES of real audio ramp up
     * from zero so the silence pre-roll → audio handoff has no DC step
     * (a hard 0→X step clicks every response start — field-confirmed). */
    size_t fade_pos = 0;

    /* Pre-buffer ~0.5s before the first play so bursty delivery gaps
     * can't empty the ring mid-sentence. The old behaviour started at
     * 640 samples and padded the shortfall with zeros — audible dropouts
     * on every delivery gap ("卡顿/丢帧感"). Feedback tones skip the
     * pre-buffer (dbaudio_out_play_tone sets s_skip_prebuf — they carry
     * their own silence pad). */
    bool pre_buffered = s_skip_prebuf;
    s_skip_prebuf = false;   /* consumed — next playback pre-buffers again */

    /* Loop-period instrumentation: a real stall (>60ms between loop
     * entries) is the signature of whatever steals CPU/cache from this
     * priority-10 task. Rate-limited so it stays quiet when healthy. */
    int64_t prev_tick = esp_timer_get_time();

    while (!s_stop_req) {
        int64_t now = esp_timer_get_time();
        if (now - prev_tick > 60000) {
            static uint32_t s_slow_cnt = 0;
            if ((++s_slow_cnt % 10) == 1) {
                ESP_LOGW(TAG, "play loop slow: %lld ms (%u times)",
                         (long long)(now - prev_tick) / 1000,
                         (unsigned)s_slow_cnt);
            }
        }
        prev_tick = now;

        /* Pre-buffer wait (only until the first play) */
        if (!pre_buffered && !s_drain_req) {
            if (ring_available() < PREBUF_SAMPLES) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            pre_buffered = true;
        }

        /* Wait for data in ring (with timeout for stop check) */
        if (ring_available() < BLOCK_SAMPLES) {
            /* Drain mode: the whole backlog has been played out — play
             * the remaining partial block with a fade-out (no DC step at
             * the tail) and exit. (audio.done arrived while seconds of
             * audio were still buffered; playing them out is the whole
             * point of the drain.) */
            if (s_drain_req) {
                size_t got = ring_pop_block(block, BLOCK_SAMPLES);
                if (got > 0) {
                    apply_fade(block, got, got, 0, false);
                    size_t written = 0;
                    board_audio_play(block, got, &written);
                    s_current_rms = compute_rms(block, got);
                }
                break;
            }
            /* Underflow: play the partial block AS-IS. Padding it with
             * zeros inserted audible silences mid-speech on every
             * delivery lag (dropout "卡顿"). The stream stays contiguous;
             * the loop just iterates more often until data catches up.
             * (The old tight spin held the codec mutex ~40ms per
             * iteration at prio 10, starving the capture task's reads —
             * "mic read incomplete" floods; the 10ms wait below keeps
             * that from returning.) */
            size_t need = BLOCK_SAMPLES;
            size_t got = ring_pop_block(block, need);
            if (got > 0) {
                if (fade_pos < FADE_SAMPLES) {
                    apply_fade(block, got, FADE_SAMPLES, fade_pos, true);
                    fade_pos += got;
                }
                size_t written = 0;
                board_audio_play(block, got, &written);
                s_current_rms = compute_rms(block, got);
            } else {
                /* Ring fully empty — a delivery gap exceeded the buffered
                 * audio: real output gap. Rate-limited evidence. */
                static uint32_t s_uf_cnt = 0;
                if ((++s_uf_cnt % 25) == 1) {
                    ESP_LOGW(TAG, "ring empty %u times — output gap",
                             (unsigned)s_uf_cnt);
                }
                vTaskDelay(pdMS_TO_TICKS(10));  /* ring empty — yield I2S */
            }
            continue;
        }

        size_t got = ring_pop_block(block, BLOCK_SAMPLES);
        if (got == 0) continue;

        /* Fade-in over the first 50ms of data (continues across blocks) */
        if (fade_pos < FADE_SAMPLES) {
            apply_fade(block, got, FADE_SAMPLES, fade_pos, true);
            fade_pos += got;
        }

        /* Interrupt fade: 20ms fade-out in place */
        if (s_interrupt_req) {
            s_interrupt_req = false;
            size_t fade_n = (got < FADE_INTR_SAMPLES) ? got : FADE_INTR_SAMPLES;
            for (size_t i = 0; i < fade_n; i++) {
                float t = 1.0f - (float)i / (float)fade_n;
                block[i] = (int16_t)((float)block[i] * t);
            }
            size_t written = 0;
            board_audio_play(block, got, &written);
            s_current_rms = compute_rms(block, got);
            /* Discard the buffered backlog: the user asked for silence
             * NOW. Playing out the ring after an interrupt was tolerable
             * at 2s capacity, but with the 32.8s ring it would keep
             * talking for half a minute after "stop".
             * C3 fix: use ring_clear_locked() to atomically reset the
             * ring with respect to push() on another core. */
            ring_clear_locked();
            break;
        }

        size_t written = 0;
        board_audio_play(block, got, &written);
        if (written == 0) {
            /* DAC feed stall: codec mutex busy or I2S write failed — this
             * 40ms block is lost (audible stutter). Rate-limited log so
             * the mechanism shows up in field logs. */
            static uint32_t s_feed_fail_cnt = 0;
            if ((++s_feed_fail_cnt % 25) == 1) {
                ESP_LOGW(TAG, "codec write failed %u times — audio stutter",
                         (unsigned)s_feed_fail_cnt);
            }
        }
        s_current_rms = compute_rms(block, got);
        play_loop_cnt++;
        if ((play_loop_cnt % 125) == 0) {  /* every ~5s */
            ESP_LOGI(TAG, "play loop #%lu: ring=%d rms=%.0f",
                     (unsigned long)play_loop_cnt,
                     dbaudio_out_ring_depth(),
                     s_current_rms);
        }
    }

    /* Anti-pop: write a short silence burst at stop (40ms) */
    {
        int16_t sil[640];
        memset(sil, 0, sizeof(sil));
        size_t written = 0;
        board_audio_play(sil, 640, &written);
    }

    s_current_rms = 0.0f;
    s_play_running = false;
    ESP_LOGI(TAG, "play task stopped");
    heap_caps_free(block);
    vTaskDeleteWithCaps(NULL);  /* PSRAM stack — see doubao_task_mem.h */
}

/* ── Public API ──────────────────────────────────────────────────────── */

static bool ensure_ring(void)
{
    if (s_ring_allocd) return true;
    s_ring = heap_caps_calloc(1, RING_CAP_SAMPLES * sizeof(int16_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) {
        ESP_LOGE(TAG, "OOM for ring buffer (%dKB PSRAM)",
                 RING_CAP_SAMPLES * 2 / 1024);
        return false;
    }
    s_ring_mutex = xSemaphoreCreateMutex();
    s_ring_head = 0;
    s_ring_tail = 0;
    s_ring_count = 0;
    s_ring_allocd = true;
    return true;
}

esp_err_t dbaudio_out_push(const int16_t *pcm16, size_t samples)
{
    if (!ensure_ring()) return ESP_ERR_NO_MEM;
    if (!pcm16 || samples == 0) return ESP_ERR_INVALID_ARG;

    /* C3: reject oversized pushes that would exceed ring capacity */
    if (samples > RING_CAP_SAMPLES) {
        ESP_LOGE(TAG, "push too large: %u > %u samples", (unsigned)samples, (unsigned)RING_CAP_SAMPLES);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Drop oldest frames if ring is full (realtime over fidelity).
     * Mutex held → play_task blocked → tail advance is atomic with push. */
    if (s_ring_mutex) xSemaphoreTake(s_ring_mutex, portMAX_DELAY);

    size_t free_space = RING_CAP_SAMPLES - s_ring_count;
    if (samples > free_space) {
        size_t drop = samples - free_space;
        s_ring_tail += drop;
        s_ring_count -= drop;
        ESP_LOGW(TAG, "ring overflow: dropped %d oldest samples", (int)drop);
    }

    ring_push_block_nolock(pcm16, samples);

    if (s_ring_mutex) xSemaphoreGive(s_ring_mutex);
    return ESP_OK;
}

esp_err_t dbaudio_out_start(void)
{
    if (s_play_running) return ESP_OK;  /* idempotent */
    if (!ensure_ring()) return ESP_ERR_NO_MEM;

    s_stop_req = false;
    s_interrupt_req = false;
    s_drain_req = false;
    /* NOTE: s_skip_prebuf is intentionally NOT cleared here — dbaudio_out_
     * play_tone() sets it just before start(); the play task consumes and
     * clears it at entry (clearing it here raced the task and tones waited
     * for a 0.5s pre-buffer that never filled — field-confirmed stall). */
    s_ring_head = 0;
    s_ring_tail = 0;
    s_ring_count = 0;

    BaseType_t ret = DB_TASK_CREATE_PSRAM(
        play_task, "doubao_play", 16384, NULL, 10, &s_play_task, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create play task");
        return ESP_ERR_NO_MEM;
    }
    s_play_running = true;
    return ESP_OK;
}

esp_err_t dbaudio_out_stop(void)
{
    if (!s_play_running) return ESP_OK;
    s_stop_req = true;
    /* Wait for task to finish (max 200ms for fade + drain) */
    for (int i = 0; i < 20 && s_play_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    /* H3 fix: if the play task is still running after timeout, return
     * ESP_ERR_TIMEOUT. The caller (state machine) must NOT proceed to
     * reconfigure I2S hardware while the play task still holds the
     * codec mutex — that causes hardware access races (field-confirmed:
     * audio corruption after rapid state transitions). */
    if (s_play_running) {
        ESP_LOGE(TAG, "play task stop timeout (still running) — will leak stack");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void dbaudio_out_interrupt(void)
{
    if (!s_play_running) return;
    s_interrupt_req = true;
}

void dbaudio_out_drain_stop(void)
{
    /* Graceful end (audio.done): play out everything still buffered, then
     * exit. Non-blocking — the play task notices the flag and keeps its
     * normal real-time cadence until the ring runs dry, then fades out.
     * Contrast with dbaudio_out_stop(), which cuts off immediately (used
     * by error/abort paths where discarding the backlog is intended). */
    if (!s_play_running) return;
    s_drain_req = true;
}

esp_err_t dbaudio_out_play_tone(const int16_t *pcm, size_t samples)
{
    /* Feedback tones (叮咚/咚叮) go through the play task (prio 10):
     * playing them from a low-priority caller let RX-processing bursts
     * starve the feeding task and underrun the I2S TX DMA (audible
     * stutter). A silent pre-pad of FADE_SAMPLES consumes the play
     * task's fade-in ramp, so the tone itself starts at full amplitude
     * (crisp attack). Bounded wait for the tone to finish playing. */
    s_skip_prebuf = true;   /* tones start immediately */
    if (dbaudio_out_start() != ESP_OK) return ESP_FAIL;

    static const int16_t sil_pad[FADE_SAMPLES];  /* zero-initialised */
    dbaudio_out_push(sil_pad, FADE_SAMPLES);
    if (dbaudio_out_push(pcm, samples) != ESP_OK) {
        dbaudio_out_stop();
        return ESP_FAIL;
    }
    for (int i = 0; i < 60 && dbaudio_out_is_playing(); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));   /* tones are ≤ ~200ms */
    }
    dbaudio_out_stop();
    return ESP_OK;
}

float dbaudio_out_current_rms(void)
{
    return s_current_rms;
}

bool dbaudio_out_is_playing(void)
{
    return s_play_running && ring_available() > 0;
}

int dbaudio_out_ring_depth(void)
{
    return (int)ring_available();
}
