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
 * 1048576 samples = 2MB PSRAM ≈ 65.5s @ 16kHz. Field logs with the 1MB
 * ring showed TTS arriving at ~2x realtime: a long reply filled the ring
 * and the overflow path dropped oldest samples — audible as a missing
 * word ("音频丢失") plus a waveform splice. 65.5s covers a ~2min reply
 * arriving at 2x, and leaves ~2MB of PSRAM free. */
#define RING_CAP_SAMPLES  (1024 * 1024)
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
static volatile bool s_tone_mode    = false;  /* one-shot feedback tone: no
                                                 pause/resume — a tone never
                                                 gets refilled, so pausing at
                                                 PAUSE_THRESHOLD split it into
                                                 two fragments with 700ms of
                                                 silence between ("叮咚" 卡顿) */
static volatile bool s_paused       = false;  /* pause/resume: I2S stopped,
                                                 waiting for ring to refill */
static volatile bool s_drop_gap     = false;  /* overflow dropped samples: the
                                                 next block must fade in, the
                                                 splice is otherwise a click */

/* Anti-pop fade state */
#define FADE_SAMPLES  800   /* 50ms @ 16kHz */
#define FADE_INTR_SAMPLES 320  /* 20ms @ 16kHz for interrupt */
/* Declick ramp length. Every path that stops feeding I2S (pause, stop,
 * interrupt, tail flush) must reach zero amplitude first: the I2S DMA's
 * auto_clear fills an underrun with zeros, so a last sample at full
 * amplitude steps straight to silence — the "偶尔爆音". 10ms is short
 * enough to be inaudible inside speech but long enough to kill the step. */
#define DECLICK_SAMPLES 160  /* 10ms @ 16kHz */

/* Actual I2S TX DMA depth (board.c: 12 descriptors × 240 frames, mono
 * 16-bit @16kHz = 180ms). A feed gap shorter than this cannot underrun —
 * the queue still holds audio — so the underrun recovery fade must stay
 * above it; a lower threshold would add a pointless 10ms dip on every
 * ordinary 90-130ms scheduler stall. */
#define DMA_DEPTH_US          180000
#define DMA_UNDERRUN_GAP_US   (DMA_DEPTH_US - 10000)
#define PREBUF_SAMPLES  8000  /* 0.5s @ 16kHz — pre-buffer before first play.
                               * Server streams TTS in bursts (measured 2.1×)
                               * with 1-2s gaps between chunks.  0.12s was
                               * too small — playback started after the first
                               * small chunk, ring emptied during the gap,
                               * causing audible silence ("ring empty" floods).
                               * 0.5s absorbs most inter-chunk jitter. */
/* Pause/resume thresholds: during playback, if the ring drops below
 * PAUSE_THRESHOLD the play task stops writing to I2S (DMA drains →
 * silence).  When the ring refills above RESUME_THRESHOLD playback
 * resumes.  This eliminates the "play partial block" artifacts that
 * sound like 卡顿 — better a brief clean pause than fragmented audio. */
#define PAUSE_THRESHOLD   1600  /* 100ms @ 16kHz — pause I2S when ring below */
#define RESUME_THRESHOLD  4800  /* 300ms @ 16kHz — resume I2S when ring above */

static float s_current_rms;

/* Declick bookkeeping: value of the last sample handed to the codec, and
 * when the last write returned. Both only touched by the play task. */
static int16_t s_last_out;
static int64_t s_last_feed_us;

static inline size_t ring_available(void)
{
    return s_ring_count;
}

/* Feed one block to I2S, remembering its final sample so a later
 * transition to silence can be ramped instead of stepped.
 * Returns samples accepted by the codec (0 on failure). */
static size_t feed_block(int16_t *pcm, size_t n)
{
    if (n == 0) return 0;
    size_t written = 0;
    board_audio_play(pcm, n, &written);
    if (written > 0) {
        s_last_out = pcm[n - 1];
        s_last_feed_us = esp_timer_get_time();
    }
    return written;
}

/* Ramp from the last fed sample down to silence, so the DMA's auto_clear
 * zero fill continues the waveform instead of cutting it. */
static void feed_declick_tail(int16_t *scratch)
{
    int16_t last = s_last_out;
    if (last > -16 && last < 16) {
        s_last_out = 0;
        return;
    }
    const int32_t den = DECLICK_SAMPLES - 1;
    for (size_t i = 0; i < DECLICK_SAMPLES; i++) {
        scratch[i] = (int16_t)((int32_t)last * (int32_t)(den - (int32_t)i) / den);
    }
    feed_block(scratch, DECLICK_SAMPLES);
    s_last_out = 0;
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

/* Peak/near-clip tracking, printed with the 5s play-loop line. It tells the
 * two remaining artifact sources apart at a glance: a run of near-clip
 * samples means the downlink level itself is hot (waveform flat-topped),
 * while a normal peak with silence gaps means the DMA ran dry. */
static void track_peak(const int16_t *pcm, size_t n,
                       int16_t *peak, uint32_t *near_clip)
{
    for (size_t i = 0; i < n; i++) {
        int32_t v = pcm[i];
        if (v < 0) v = -v;
        if (v > *peak) *peak = (v > 32767) ? 32767 : (int16_t)v;
        if (v >= 32000) (*near_clip)++;
    }
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
        s_tone_mode = false;
        vTaskDeleteWithCaps(NULL);  /* PSRAM stack — see doubao_task_mem.h */
        return;
    }

    ESP_LOGI(TAG, "play task started (ring=%d samples / %dKB)",
             RING_CAP_SAMPLES, RING_CAP_SAMPLES * 2 / 1024);

    uint32_t play_loop_cnt = 0;
    int16_t  win_peak   = 0;
    uint32_t win_clip   = 0;

    /* Anti-pop: write a short silence burst at start (50ms). Tones already
     * carry their own 50ms silent pad (dbaudio_out_play_tone), so skip the
     * extra 40ms — it only delayed the "叮". */
    if (!s_tone_mode) {
        int16_t sil[640];  /* 40ms @ 16kHz, stack is fine for this */
        memset(sil, 0, sizeof(sil));
        feed_block(sil, 640);
        /* First real block is a session start, not an underrun recovery. */
        s_last_feed_us = 0;
    } else {
        s_last_out = 0;
        s_last_feed_us = 0;
    }

    /* Fade-in bookkeeping: the first FADE_SAMPLES of real audio ramp up
     * from zero so the silence pre-roll → audio handoff has no DC step
     * (a hard 0→X step clicks every response start — field-confirmed).
     * After a pause/underrun the ramp is shorter: the waveform continues
     * mid-speech there, so a 50ms swell would be audible as a dip. */
    size_t fade_pos    = 0;
    size_t fade_in_len = FADE_SAMPLES;

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

    /* Tail flush: if the server stops sending without tts.ended (lost
     * frame / dropped connection), playback would otherwise hold the last
     * <RESUME_THRESHOLD samples in the ring forever and the UI stays in
     * SPEAKING until the 60s watchdog. Once the ring stops growing for
     * 700ms, play whatever is left instead of waiting for a refill that
     * is never coming. */
    bool     flush_tail       = false;
    int      pause_ring       = 0;
    int64_t  pause_change_us  = esp_timer_get_time();

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
                ESP_LOGD(TAG, "prebuf: ring=%d < %d, waiting",
                         (int)ring_available(), PREBUF_SAMPLES);
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            ESP_LOGI(TAG, "prebuf: ring=%d >= %d, ready to play",
                     (int)ring_available(), PREBUF_SAMPLES);
            pre_buffered = true;
        }

        /* ── Drain mode: audio.done received, ring draining to zero ── */
        if (s_drain_req && ring_available() < BLOCK_SAMPLES) {
            ESP_LOGI(TAG, "drain: starting, ring=%d samples",
                     dbaudio_out_ring_depth());
            size_t got = ring_pop_block(block, BLOCK_SAMPLES);
            if (got > 0) {
                s_current_rms = compute_rms(block, got);
                if (got >= DECLICK_SAMPLES) {
                    apply_fade(block, got, got, 0, false);
                    feed_block(block, got);
                } else {
                    /* Too short for a graceful fade — play it, then ramp
                     * down from its last sample. */
                    feed_block(block, got);
                    feed_declick_tail(block);
                }
            }
            ESP_LOGI(TAG, "drain: complete, ring=%d samples",
                     dbaudio_out_ring_depth());
            break;
        }

        /* ── Pause/resume: when ring drops low, stop writing to I2S ──
         * Instead of playing partial blocks (audible 卡顿 fragments),
         * pause playback and wait for the server's next burst to refill
         * the ring.  Resume when the ring crosses RESUME_THRESHOLD.
         * NOTE: skip pause entirely during drain — the server finished
         * sending and we must play out the tail, even if it's partial. */
        if (!s_drain_req && !flush_tail && !s_tone_mode &&
            ring_available() < PAUSE_THRESHOLD && !s_paused) {
            /* Ring low — enter pause.  Don't play partial blocks: the
             * fragments sound like stuttering ("卡顿").  A clean pause
             * (I2S DMA drains → brief silence) followed by a smooth
             * resume is far less noticeable. */
            static uint32_t s_pause_cnt = 0;
            if ((++s_pause_cnt % 5) == 1) {
                ESP_LOGI(TAG, "pause: ring=%d < %d, buffering (%u times)",
                         dbaudio_out_ring_depth(), PAUSE_THRESHOLD,
                         (unsigned)s_pause_cnt);
            }
            /* Ramp the tail to zero before the DMA runs dry. Without this
             * the auto_clear zero fill steps the DAC from the last audio
             * sample to 0 — the occasional click heard on delivery gaps. */
            feed_declick_tail(block);
            s_paused = true;
            pause_ring = ring_available();
            pause_change_us = now;
        }
        if (s_paused) {
            int ring = ring_available();
            if (ring != pause_ring) {
                pause_ring = ring;
                pause_change_us = now;
            }
            if (ring >= RESUME_THRESHOLD) {
                ESP_LOGI(TAG, "resume: ring=%d >= %d",
                         dbaudio_out_ring_depth(), RESUME_THRESHOLD);
                s_paused = false;
                flush_tail = false;
                /* Re-apply fade-in to avoid DC step after silence gap */
                fade_pos = 0;
                fade_in_len = DECLICK_SAMPLES;
            } else if (now - pause_change_us > 700000 && ring > 0) {
                ESP_LOGW(TAG, "pause stall: ring=%d unchanged 700ms — flushing tail",
                         ring);
                s_paused = false;
                flush_tail = true;
                fade_pos = 0;
                fade_in_len = DECLICK_SAMPLES;
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
        }

        /* New data arrived after a flush — back to normal block playback */
        if (flush_tail && ring_available() >= RESUME_THRESHOLD) {
            flush_tail = false;
        }

        /* ── Normal path: wait for a full block ── */
        if (ring_available() < BLOCK_SAMPLES) {
            if (flush_tail || s_tone_mode) {
                /* Stream is dead / tone is one-shot — play the partial
                 * tail now rather than holding it in the ring. */
                size_t got = ring_pop_block(block, BLOCK_SAMPLES);
                if (got == 0) {
                    flush_tail = false;
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                s_current_rms = compute_rms(block, got);
                if (got >= DECLICK_SAMPLES) {
                    apply_fade(block, got, got, 0, false);
                    feed_block(block, got);
                } else {
                    feed_block(block, got);
                    feed_declick_tail(block);
                }
                continue;
            }
            /* Partial data available but not enough for a full block.
             * Wait for more data instead of playing a fragment. */
            ESP_LOGD(TAG, "wait: ring=%d < %d",
                     (int)ring_available(), BLOCK_SAMPLES);
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        size_t got = ring_pop_block(block, BLOCK_SAMPLES);
        if (got == 0) continue;

        /* Overflow splice: the ring dropped samples ahead of the read
         * pointer, so this block is discontinuous with the last one.
         * Fade in so the jump is a soft resume instead of a click. */
        if (s_drop_gap) {
            s_drop_gap = false;
            fade_pos = 0;
            fade_in_len = DECLICK_SAMPLES;
        }

        ESP_LOGD(TAG, "play: popped %d samples, ring=%d",
                 (int)got, (int)ring_available());

        /* A gap since the last successful write longer than the DMA depth
         * means the queue ran dry and the output already stepped to zeros.
         * Ramp back in instead of resuming at full amplitude (fast restart
         * after a preemption stall — the other occasional click source). */
        if (s_last_feed_us != 0 &&
            esp_timer_get_time() - s_last_feed_us > DMA_UNDERRUN_GAP_US) {
            fade_pos = 0;
            fade_in_len = DECLICK_SAMPLES;
            static uint32_t s_underrun_cnt = 0;
            if ((++s_underrun_cnt % 10) == 1) {
                ESP_LOGW(TAG, "DMA underrun: %lldms since last feed — fade-in (%u times)",
                         (long long)(esp_timer_get_time() - s_last_feed_us) / 1000,
                         (unsigned)s_underrun_cnt);
            }
        }

        /* Fade-in over the first FADE_SAMPLES of data (continues across blocks) */
        if (fade_pos < fade_in_len) {
            apply_fade(block, got, fade_in_len, fade_pos, true);
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
            /* Feed ONLY the faded part. Writing the full block would put
             * full-amplitude audio after the ramp, reopening the step the
             * fade was supposed to close. */
            feed_block(block, fade_n);
            s_current_rms = compute_rms(block, got);
            ring_clear_locked();
            break;
        }

        if (feed_block(block, got) == 0) {
            static uint32_t s_feed_fail_cnt = 0;
            if ((++s_feed_fail_cnt % 25) == 1) {
                ESP_LOGW(TAG, "codec write failed %u times — audio stutter",
                         (unsigned)s_feed_fail_cnt);
            }
        }
        s_current_rms = compute_rms(block, got);
        track_peak(block, got, &win_peak, &win_clip);
        play_loop_cnt++;
        if ((play_loop_cnt % 125) == 0) {  /* every ~5s */
            ESP_LOGI(TAG, "play loop #%lu: ring=%d rms=%.0f peak=%d nearclip=%lu",
                     (unsigned long)play_loop_cnt,
                     dbaudio_out_ring_depth(),
                     s_current_rms, (int)win_peak, (unsigned long)win_clip);
            win_peak = 0;
            win_clip = 0;
        }
    }

    /* Anti-pop: ramp the current level to zero, then pad with silence.
     * A bare silence burst after full-amplitude audio is itself a step. */
    feed_declick_tail(block);
    {
        int16_t sil[640];
        memset(sil, 0, sizeof(sil));
        feed_block(sil, 640);
    }

    s_current_rms = 0.0f;
    s_tone_mode = false;   /* next start() (TTS) must pause/resume normally */
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

    size_t prev_count = s_ring_count;
    size_t free_space = RING_CAP_SAMPLES - s_ring_count;
    if (samples > free_space) {
        size_t drop = samples - free_space;
        s_ring_tail += drop;
        s_ring_count -= drop;
        s_drop_gap = true;
        ESP_LOGW(TAG, "ring overflow: dropped %d oldest samples (cap=%d)",
                 (int)drop, (int)RING_CAP_SAMPLES);
    }

    ring_push_block_nolock(pcm16, samples);

    if (s_ring_mutex) xSemaphoreGive(s_ring_mutex);

    /* Debug: log every push with ring depth for pipeline timing analysis */
    ESP_LOGD(TAG, "push %d samples → ring %d→%d (cap=%d)",
             (int)samples, (int)prev_count, (int)s_ring_count,
             (int)RING_CAP_SAMPLES);
    return ESP_OK;
}

esp_err_t dbaudio_out_start(void)
{
    if (s_play_running) return ESP_OK;  /* idempotent */
    if (!ensure_ring()) return ESP_ERR_NO_MEM;

    s_stop_req = false;
    s_interrupt_req = false;
    s_drain_req = false;
    s_paused = false;
    /* NOTE: s_skip_prebuf is intentionally NOT cleared here — dbaudio_out_
     * play_tone() sets it just before start(); the play task consumes and
     * clears it at entry (clearing it here raced the task and tones waited
     * for a 0.5s pre-buffer that never filled — field-confirmed stall). */
    s_ring_head = 0;
    s_ring_tail = 0;
    s_ring_count = 0;
    s_drop_gap = false;

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
    s_paused = false;   /* clear pause so the interrupted playback exits
                         * cleanly (ring_clear_locked in the play task) */
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
    /* If the play task is paused (ring was low), wake it so it can
     * process the drain. Without this, the pause loop does `continue`
     * which skips the drain check — the device hangs for 60s until the
     * speak timeout fires (field-confirmed on short audio responses). */
    s_paused = false;
}

esp_err_t dbaudio_out_play_tone(const int16_t *pcm, size_t samples)
{
    /* Feedback tones (叮咚/咚叮) go through the play task (prio 10):
     * playing them from a low-priority caller let RX-processing bursts
     * starve the feeding task and underrun the I2S TX DMA (audible
     * stutter). A silent pre-pad of FADE_SAMPLES consumes the play
     * task's fade-in ramp, so the tone itself starts at full amplitude
     * (crisp attack). Bounded wait for the tone to finish playing. */
    s_tone_mode = true;
    s_skip_prebuf = true;   /* tones start immediately */
    if (dbaudio_out_start() != ESP_OK) {
        s_tone_mode = false;
        return ESP_FAIL;
    }

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
