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
 *
 * CLAUDE.md rules applied:
 *   7  — task priority 9 (below play=10, above WS/UI); core 1
 *   8  — ISR/callback prohibitions N/A here (blocking I2S read in task)
 *   12 — I2S RX right channel only (ES7210 mic data on right slot)
 *   13 — DMA 80-150ms total; ring absorbs jitter; i2s read has timeout
 *   3  — TCB internal RAM, stack 16KB PSRAM (铁律3 ≥16KB)
 */

#include "doubao_audio_in.h"
#include "board.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "doubao_task_mem.h"

static const char *TAG = "doubao_ain";

/* ── Send queue (12 frames × 640 samples × 2 bytes = ~15KB) ──────────── */

#define QUEUE_FRAMES  12

static int16_t  *s_queue;           /* PSRAM: QUEUE_FRAMES × FRAME_SAMPLES */
static volatile size_t s_q_head;    /* write index */
static volatile size_t s_q_tail;    /* read index */
static SemaphoreHandle_t s_q_mutex;
static int s_overflow_cnt;          /* total dropped frames */
static bool s_allocd = false;

/* ── Capture task ────────────────────────────────────────────────────── */

static TaskHandle_t s_cap_task;
static volatile bool s_cap_running = false;
static volatile bool s_stop_req = false;

static inline size_t queue_avail(void)
{
    size_t h = s_q_head;
    size_t t = s_q_tail;
    return (h - t);
}

static inline size_t queue_free(void)
{
    return QUEUE_FRAMES - queue_avail();
}

static inline void queue_push(const int16_t *frame)
{
    size_t idx = s_q_head % QUEUE_FRAMES;
    memcpy(&s_queue[idx * DBAUDIO_FRAME_SAMPLES], frame,
           DBAUDIO_FRAME_SAMPLES * sizeof(int16_t));
    s_q_head++;
}

static void cap_task(void *arg)
{
    (void)arg;

    /* Allocate capture buffer in internal DMA-capable RAM */
    int16_t *cap_buf = heap_caps_calloc(1, DBAUDIO_FRAME_SAMPLES * sizeof(int16_t),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!cap_buf) {
        ESP_LOGE(TAG, "OOM for capture buffer (internal DMA RAM)");
        s_cap_running = false;
        vTaskDeleteWithCaps(NULL);  /* PSRAM stack — see doubao_task_mem.h */
        return;
    }

    ESP_LOGI(TAG, "capture task started (queue=%d frames)", QUEUE_FRAMES);
    uint32_t loop_cnt = 0;

    while (!s_stop_req) {
        size_t samples_read = 0;
        esp_err_t ret = board_audio_record(cap_buf, DBAUDIO_FRAME_SAMPLES,
                                            &samples_read);
        if (ret != ESP_OK || samples_read < DBAUDIO_FRAME_SAMPLES) {
            ESP_LOGW(TAG, "mic read incomplete: ret=%s read=%d/%d",
                     esp_err_to_name(ret), (int)samples_read,
                     DBAUDIO_FRAME_SAMPLES);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* Enqueue frame (drop oldest on overflow) — mutex protects
         * s_q_tail vs send task's dequeue. */
        if (s_q_mutex) xSemaphoreTake(s_q_mutex, portMAX_DELAY);
        if (queue_free() == 0) {
            s_q_tail++;  /* discard oldest */
            s_overflow_cnt++;
            if ((s_overflow_cnt % 50) == 1) {
                ESP_LOGW(TAG, "send queue overflow: %d frames dropped total",
                         s_overflow_cnt);
            }
        }
        queue_push(cap_buf);
        if (s_q_mutex) xSemaphoreGive(s_q_mutex);

        loop_cnt++;
        if ((loop_cnt % 250) == 0) {
            ESP_LOGI(TAG, "capture loop #%lu: qdepth=%d overflow=%d",
                     (unsigned long)loop_cnt,
                     dbaudio_in_queue_depth(),
                     s_overflow_cnt);
        }
    }

    heap_caps_free(cap_buf);
    s_cap_running = false;
    ESP_LOGI(TAG, "capture task stopped (loops=%lu, overflow=%d)",
             (unsigned long)loop_cnt, s_overflow_cnt);
    vTaskDeleteWithCaps(NULL);  /* PSRAM stack — see doubao_task_mem.h */
}

/* ── Public API ──────────────────────────────────────────────────────── */

static bool ensure_alloc(void)
{
    if (s_allocd) return true;
    s_queue = heap_caps_calloc(1,
                QUEUE_FRAMES * DBAUDIO_FRAME_SAMPLES * sizeof(int16_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_queue) {
        ESP_LOGE(TAG, "OOM for send queue (%dKB PSRAM)",
                 QUEUE_FRAMES * DBAUDIO_FRAME_SAMPLES * 2 / 1024);
        return false;
    }
    s_q_mutex = xSemaphoreCreateMutex();
    s_q_head = 0;
    s_q_tail = 0;
    s_overflow_cnt = 0;
    s_allocd = true;
    return true;
}

esp_err_t dbaudio_in_start(void)
{
    if (s_cap_running) return ESP_OK;
    if (!ensure_alloc()) return ESP_ERR_NO_MEM;

    s_stop_req = false;
    s_q_head = 0;
    s_q_tail = 0;
    s_overflow_cnt = 0;

    /* H1 fix: set s_cap_running BEFORE creating the task to prevent
     * a race where the task exits immediately (e.g., OOM for cap_buf)
     * and sets s_cap_running=false, then the creator sets it true. */
    s_cap_running = true;

    BaseType_t ret = DB_TASK_CREATE_PSRAM(
        cap_task, "doubao_cap", 16384, NULL, 9, &s_cap_task, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create capture task");
        s_cap_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t dbaudio_in_stop(void)
{
    if (!s_cap_running) return ESP_OK;
    s_stop_req = true;
    for (int i = 0; i < 20 && s_cap_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    /* H2 fix: if the capture task is still running after timeout,
     * return ESP_ERR_TIMEOUT. The caller must NOT proceed to
     * reconfigure I2S hardware while the capture task still holds
     * the codec RX channel — that causes hardware access races. */
    if (s_cap_running) {
        ESP_LOGE(TAG, "capture task stop timeout (still running) — will leak stack");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void dbaudio_in_reset_queue(void)
{
    if (!s_allocd) return;
    if (s_q_mutex) xSemaphoreTake(s_q_mutex, portMAX_DELAY);
    s_q_head = 0;
    s_q_tail = 0;
    s_overflow_cnt = 0;
    if (s_q_mutex) xSemaphoreGive(s_q_mutex);
}

size_t dbaudio_in_dequeue(int16_t *dst, size_t max_samp)
{
    if (!s_allocd || !dst) return 0;

    /* Mutex: prevent race with capture task's overflow drop */
    if (s_q_mutex) xSemaphoreTake(s_q_mutex, portMAX_DELAY);
    size_t avail = queue_avail();
    if (avail == 0) {
        if (s_q_mutex) xSemaphoreGive(s_q_mutex);
        return 0;
    }

    size_t idx = s_q_tail % QUEUE_FRAMES;
    /* The queue is frame-granular: each slot holds exactly
     * DBAUDIO_FRAME_SAMPLES. `avail` above counts FRAMES, so a frame is
     * either returned whole (n = 640 samples) or not at all — comparing n
     * (samples) against avail (frames) here used to clamp the copy to
     * 1..12 SAMPLES per frame while still consuming the whole slot,
     * shredding 99% of the mic audio on its way to the server. */
    size_t n = (max_samp < DBAUDIO_FRAME_SAMPLES) ? max_samp
                                                    : DBAUDIO_FRAME_SAMPLES;
    memcpy(dst, &s_queue[idx * DBAUDIO_FRAME_SAMPLES], n * sizeof(int16_t));
    s_q_tail++;
    if (s_q_mutex) xSemaphoreGive(s_q_mutex);
    return n;
}

int dbaudio_in_queue_depth(void)
{
    return (int)queue_avail();
}

bool dbaudio_in_is_running(void)
{
    return s_cap_running;
}

int dbaudio_in_drop_count(void)
{
    return s_overflow_cnt;
}
