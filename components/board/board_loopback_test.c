/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * 临时测试模块 — Task 12 Step 5 删除（M1 全双工硬件回路验证）
 *
 * Goal: verify the shared-clock full-duplex I2S path on this board
 * (ES8311 DAC + ES7210 ADC, one I2S bus, single 16kHz clock — see
 * CLAUDE.md rule 11/12). The loopback task plays a continuous 440Hz sine
 * while recording the mic, logging every underrun. Conclusion data
 * (underrun count, pops) feeds the M1 acceptance record.
 *
 * Board notes (do not duplicate):
 *  - board.c already initializes I2S once (TX SLOT_BOTH / RX SLOT_BOTH at
 *    16kHz) and owns the codec handles + s_codec_mutex. We only call
 *    board_audio_record()/board_audio_play() — each call is internally
 *    mutex-protected (100ms timeout), so no extra locking here.
 *  - Actual signatures: board_audio_record(int16_t*, size_t, size_t*)
 *    and board_audio_play(const int16_t*, size_t, size_t*) — the out
 *    param reports samples read/written; 0 + ESP_ERR_INVALID_STATE means
 *    codec busy/unavailable (mutex timeout).
 *  - 440Hz at 16kHz over a 100ms block = exactly 44 complete cycles, so
 *    repeating the same buffer is phase-continuous (no click at block wrap).
 *  - While `audio loop` runs, the wake-word task's mic reads are starved
 *    (it silently retries on mutex timeout) — acceptable for the M1 test;
 *    the formal build arbitrates mic access via the state machine.
 */

#include "board_loopback_test.h"
#include "board.h"

#include <math.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BLOCK_MS       100                       /* block length in ms          */
#define BLOCK_SAMPLES  (BOARD_AUDIO_SAMPLE_RATE / (1000 / BLOCK_MS))  /* 1600 @16k */
#define LOOP_PRIORITY  5                         /* same as wake_word task      */
#define LOOP_CORE      1                         /* core 0 keeps LVGL/WiFi      */
#define LOOP_STACK     (8 * 1024)                /* internal RAM, per task spec */
#define TAU_F          (6.283185307179586f)

static const char *TAG = "loopback";

static TaskHandle_t  s_task            = NULL;
static volatile bool s_stop_requested  = false;

/* ── Loopback task: simultaneous play (440Hz) + record ──────────────────── */

static void loopback_task(void *arg)
{
    int16_t *play = heap_caps_malloc(BLOCK_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    int16_t *rec  = heap_caps_malloc(BLOCK_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    if (!play || !rec) {
        ESP_LOGE(TAG, "OOM allocating %d-sample buffers (internal RAM)",
                 BLOCK_SAMPLES);
        goto out;
    }

    /* 440Hz sine, 8k amplitude (no clipping, no DC): 44 cycles per block */
    for (int i = 0; i < BLOCK_SAMPLES; i++) {
        play[i] = (int16_t)(sinf(i * TAU_F * 440.0f / BOARD_AUDIO_SAMPLE_RATE) * 8000.0f);
    }

    ESP_LOGI(TAG, "started: play 440Hz + record, %d samples (%dms) @ %dHz",
             BLOCK_SAMPLES, BLOCK_MS, BOARD_AUDIO_SAMPLE_RATE);

    uint32_t iterations = 0;
    uint32_t underruns  = 0;
    uint32_t early_fail = 0;   /* consecutive total failures at startup */

    while (!s_stop_requested) {
        size_t r = 0, w = 0;
        esp_err_t rerr = board_audio_record(rec, BLOCK_SAMPLES, &r);
        esp_err_t werr = board_audio_play(play, BLOCK_SAMPLES, &w);
        iterations++;

        bool bad = (rerr != ESP_OK) || (werr != ESP_OK) ||
                   (r != BLOCK_SAMPLES) || (w != BLOCK_SAMPLES);
        if (!bad) {
            early_fail = 0;
        } else {
            if ((rerr == ESP_ERR_INVALID_STATE || werr == ESP_ERR_INVALID_STATE) &&
                iterations <= 20) {
                /* Codec unavailable at startup (not initialized, or another
                 * task held the mutex the whole time) — give up cleanly. */
                early_fail++;
                if (early_fail >= 20) {
                    ESP_LOGE(TAG, "codec not ready (rerr=%s werr=%s) — aborting; "
                             "was board_audio_init() called?",
                             esp_err_to_name(rerr), esp_err_to_name(werr));
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            early_fail = 0;
            underruns++;
            ESP_LOGW(TAG, "underrun: r=%d/%d w=%d/%d (rerr=%s werr=%s)",
                     (int)r, BLOCK_SAMPLES, (int)w, BLOCK_SAMPLES,
                     esp_err_to_name(rerr), esp_err_to_name(werr));
        }

        if (iterations % 300 == 0) {   /* heartbeat every ~30s */
            ESP_LOGI(TAG, "heartbeat: %lu iterations (%lus), underruns=%lu",
                     (unsigned long)iterations,
                     (unsigned long)(iterations * BLOCK_MS / 1000),
                     (unsigned long)underruns);
        }
    }

    ESP_LOGI(TAG, "stopped: %lu iterations (%lus), underruns=%lu",
             (unsigned long)iterations,
             (unsigned long)(iterations * BLOCK_MS / 1000),
             (unsigned long)underruns);

    free(play);
    free(rec);
out:
    s_task = NULL;
    s_stop_requested = false;
    vTaskDelete(NULL);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t board_loopback_start(void)
{
    if (s_task) {
        ESP_LOGW(TAG, "already running");
        return ESP_OK;
    }
    s_stop_requested = false;
    BaseType_t ret = xTaskCreatePinnedToCore(loopback_task, "audio_loop",
                                             LOOP_STACK, NULL,
                                             LOOP_PRIORITY, &s_task, LOOP_CORE);
    if (ret != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "task create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "task created (core %d, stack %d B, prio %d)",
             LOOP_CORE, LOOP_STACK, LOOP_PRIORITY);
    return ESP_OK;
}

esp_err_t board_loopback_stop(void)
{
    if (!s_task) {
        ESP_LOGW(TAG, "not running");
        return ESP_OK;
    }
    s_stop_requested = true;
    /* Wait for the task to exit by itself (it never deletes while holding
     * the codec mutex — a remote vTaskDelete could orphan the mutex). */
    TickType_t start = xTaskGetTickCount();
    while (s_task && (xTaskGetTickCount() - start) < pdMS_TO_TICKS(500)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_task) ESP_LOGW(TAG, "stop timeout — task still finishing current block");
    return ESP_OK;
}

bool board_loopback_is_running(void)
{
    return s_task != NULL;
}

esp_err_t board_loopback_record_rms(void)
{
    if (s_task) {
        ESP_LOGW(TAG, "loopback is running — stop it first (audio loop stop)");
        return ESP_ERR_INVALID_STATE;
    }
    int16_t *buf = heap_caps_malloc(BLOCK_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    if (!buf) return ESP_ERR_NO_MEM;

    size_t got = 0;
    esp_err_t err = board_audio_record(buf, BLOCK_SAMPLES, &got);
    if (err != ESP_OK || got == 0) {
        ESP_LOGE(TAG, "record failed: %s (samples=%d)", esp_err_to_name(err), (int)got);
        free(buf);
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    int64_t sum_sq = 0;
    int     min_v  = 32767, max_v = -32768;
    for (size_t i = 0; i < got; i++) {
        int v = buf[i];
        sum_sq += (int64_t)v * v;
        if (v < min_v) min_v = v;
        if (v > max_v) max_v = v;
    }
    float rms = sqrtf((float)(sum_sq / (int64_t)got));

    printf("[audio rec] %d samples (%dms @ %dHz): RMS=%d (%.1f%% FS)  min=%d max=%d\n",
           (int)got, BLOCK_MS, BOARD_AUDIO_SAMPLE_RATE,
           (int)rms, rms / 32768.0f * 100.0f, min_v, max_v);
    free(buf);
    return ESP_OK;
}
