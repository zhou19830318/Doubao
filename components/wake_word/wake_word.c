/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * Wake word detection using ESP-SR.
 * Two engines supported (selected via menuconfig):
 *   - WakeNet (default): pre-trained models ("Hey Jarvis", "Hi ESP", etc.)
 *   - MultiNet: custom phrase recognition (3+ word commands only)
 *
 * Runs a background task that continuously reads mic audio and feeds
 * it to the selected engine. When the wake word is detected, sets an event bit.
 * Must be paused during recording/TTS to avoid I2S bus contention.
 */

#include "wake_word.h"
#include "board.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>

/* ESP-SR is only available on ESP32-S3 */
#if !CONFIG_IDF_TARGET_ESP32S3

static const char *TAG = "wake_word";

esp_err_t wake_word_init(void) {
    ESP_LOGW(TAG, "Wake word not supported on %s (requires ESP32-S3)", BOARD_MCU);
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t wake_word_start(EventGroupHandle_t eg, EventBits_t bit) { (void)eg; (void)bit; return ESP_ERR_NOT_SUPPORTED; }
void wake_word_pause(void) {}
void wake_word_resume(void) {}
void wake_word_stop(void) {}
bool wake_word_is_running(void) { return false; }
const char *wake_word_get_phrase(void) { return "N/A"; }

#else /* ESP32-S3 */

#include "model_path.h"

#ifdef CONFIG_AIDB_WAKE_WORD_MULTINET
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#else
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#endif

static const char *TAG = "wake_word";

static srmodel_list_t     *s_models       = NULL;
static TaskHandle_t        s_task_handle  = NULL;
static EventGroupHandle_t  s_event_group  = NULL;
static EventBits_t         s_event_bit    = 0;
static volatile bool       s_running      = false;
static volatile bool       s_paused       = false;
static SemaphoreHandle_t   s_pause_ack    = NULL;  /* Task→caller handshake for pause */
static SemaphoreHandle_t   s_exit_ack     = NULL;  /* C4 fix: task→caller handshake for stop */
static int                 s_chunk_size   = 0;

#ifdef CONFIG_AIDB_WAKE_WORD_MULTINET
static esp_mn_iface_t     *s_multinet_iface = NULL;
static model_iface_data_t *s_multinet_data  = NULL;
#else
static esp_wn_iface_t     *s_wakenet_iface = NULL;
static model_iface_data_t *s_wakenet_data  = NULL;
#endif

static void wake_word_task(void *arg);

esp_err_t wake_word_init(void)
{
#ifdef CONFIG_AIDB_WAKE_WORD_MULTINET
    if (s_multinet_data) {
#else
    if (s_wakenet_data) {
#endif
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    s_models = esp_srmodel_init("model");
    if (!s_models || s_models->num <= 0) {
        ESP_LOGE(TAG, "No SR models found on 'model' partition");
        return ESP_FAIL;
    }

#ifdef CONFIG_AIDB_WAKE_WORD_MULTINET
    /* --- MultiNet custom phrase detection (3+ word commands) --- */
    char *mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    if (!mn_name) {
        ESP_LOGE(TAG, "No English MultiNet model found");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Loading MultiNet model: %s", mn_name);
    s_multinet_iface = (esp_mn_iface_t *)esp_mn_handle_from_name(mn_name);
    if (!s_multinet_iface) {
        ESP_LOGE(TAG, "Failed to get MultiNet handle for '%s'", mn_name);
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }

    s_multinet_data = s_multinet_iface->create(mn_name, 6000);
    if (!s_multinet_data) {
        ESP_LOGE(TAG, "Failed to create MultiNet instance");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }

    float threshold = CONFIG_AIDB_WAKE_THRESHOLD / 100.0f;
    s_multinet_iface->set_det_threshold(s_multinet_data, threshold);
    esp_mn_commands_clear();
    esp_mn_commands_add(1, CONFIG_AIDB_WAKE_PHRASE);
    esp_mn_error_t *errors = esp_mn_commands_update();
    if (errors) {
        ESP_LOGW(TAG, "MultiNet command errors for '%s':", CONFIG_AIDB_WAKE_PHRASE);
        for (int i = 0; errors[i].command_id != -1; i++) {
            ESP_LOGW(TAG, "  error: id=%d", errors[i].command_id);
        }
    }

    s_chunk_size = s_multinet_iface->get_samp_chunksize(s_multinet_data);
    int freq = s_multinet_iface->get_samp_rate(s_multinet_data);

    ESP_LOGI(TAG, "MultiNet ready: phrase=\"%s\" threshold=%.0f%% freq=%dHz chunk=%d",
             CONFIG_AIDB_WAKE_PHRASE, threshold * 100, freq, s_chunk_size);
    s_multinet_iface->print_active_speech_commands(s_multinet_data);

#else
    /* --- WakeNet pre-trained model detection --- */
    char *model_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL);
    if (!model_name) {
        ESP_LOGE(TAG, "No WakeNet model found in partition");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Loading WakeNet model: %s", model_name);

    s_wakenet_iface = (esp_wn_iface_t *)esp_wn_handle_from_name(model_name);
    if (!s_wakenet_iface) {
        ESP_LOGE(TAG, "Failed to get WakeNet handle for '%s'", model_name);
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }

    s_wakenet_data = s_wakenet_iface->create(model_name, DET_MODE_95);
    if (!s_wakenet_data) {
        ESP_LOGE(TAG, "Failed to create WakeNet instance");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return ESP_FAIL;
    }

    int freq = s_wakenet_iface->get_samp_rate(s_wakenet_data);
    s_chunk_size = s_wakenet_iface->get_samp_chunksize(s_wakenet_data);

    /* Log the actual wake word from the model */
    const char *word = s_wakenet_iface->get_word_name(s_wakenet_data, 1);
    ESP_LOGI(TAG, "WakeNet ready: wake word=\"%s\" model=%s freq=%dHz chunk=%d",
             word ? word : "unknown", model_name, freq, s_chunk_size);
#endif

    return ESP_OK;
}

esp_err_t wake_word_start(EventGroupHandle_t event_group, EventBits_t event_bit)
{
#ifdef CONFIG_AIDB_WAKE_WORD_MULTINET
    if (!s_multinet_data) {
#else
    if (!s_wakenet_data) {
#endif
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task_handle) {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }

    s_event_group = event_group;
    s_event_bit   = event_bit;
    s_running     = true;
    s_paused      = false;

    /* Create pause-acknowledge semaphore for deterministic handshake.
     * wake_word_pause() waits on this; the detection task gives it when
     * it sees s_paused==true and is safely out of board_audio_record(). */
    if (!s_pause_ack) {
        s_pause_ack = xSemaphoreCreateBinary();
    }

    /* C4 fix: create exit-acknowledge semaphore for deterministic stop.
     * wake_word_stop() waits on this; the task gives it right before
     * vTaskDelete(NULL) so the caller knows the task has exited and it
     * is safe to destroy the model memory. */
    if (!s_exit_ack) {
        s_exit_ack = xSemaphoreCreateBinary();
    }

    /* H7 fix: 4KB is too small for ESP-SR model inference + logging +
     * floating-point math + board_audio_record. 8KB provides adequate
     * headroom (verified by typical ESP-SR stack usage of ~5KB). */
    BaseType_t ret = xTaskCreatePinnedToCore(
        wake_word_task, "wake_word", 8192, NULL, 5, &s_task_handle, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create wake word task");
        s_running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Wake word detection started");
    return ESP_OK;
}

void wake_word_pause(void)
{
    if (!s_running) return;
    if (s_paused) return;  /* Already paused — idempotent */

    s_paused = true;
    /* Wait for the detection task to acknowledge the pause.
     * The task gives s_pause_ack when it sees s_paused==true and has
     * released the I2S RX channel (not in the middle of board_audio_record).
     * This replaces the previous 110ms timing heuristic with a deterministic
     * handshake, preventing I2S reconfig races. */
    if (s_pause_ack) {
        TickType_t acked = xSemaphoreTake(s_pause_ack, pdMS_TO_TICKS(500));
        if (!acked) {
            ESP_LOGW(TAG, "Pause ack timeout — task may still hold I2S RX");
        }
    }
    ESP_LOGD(TAG, "Paused");
}

void wake_word_resume(void)
{
    if (!s_running) return;
    if (!s_paused) return;  /* Already running — idempotent */
    s_paused = false;
    ESP_LOGI(TAG, "Wake word resumed - detection task will continue");
}

void wake_word_stop(void)
{
    s_running = false;
    if (s_task_handle) {
        /* C4 fix: wait for the task to confirm it has exited before
         * destroying the model. The old code used a fixed 200ms delay
         * which was not deterministic — the task might still be in
         * board_audio_record() or running model detect() when the
         * caller destroys the model memory → Use-after-free.
         *
         * Timeout: 2s is generous for a single chunk read + model
         * inference cycle. If exceeded, we cannot safely destroy the
         * model — the task might still hold a pointer to it. Enter
         * safe degradation: the task will see s_running==false and
         * exit on its own, but we must NOT touch model pointers. */
        if (s_exit_ack) {
            TickType_t acked = xSemaphoreTake(s_exit_ack, pdMS_TO_TICKS(2000));
            if (!acked) {
                ESP_LOGE(TAG, "Stop ack timeout — task may still hold model pointers. "
                         "Skipping model destroy to avoid Use-after-free.");
                /* Leave s_task_handle non-NULL so we don't re-create the task
                 * (the old task will exit soon on its own via s_running==false). */
                ESP_LOGW(TAG, "Safe degradation: model will NOT be destroyed (leaked)");
                return;
            }
        }
        s_task_handle = NULL;
    }

    /* Only reach here if the task has confirmed exit (or was never running).
     * Safe to destroy model memory now. */
    if (s_pause_ack) {
        vSemaphoreDelete(s_pause_ack);
        s_pause_ack = NULL;
    }
    if (s_exit_ack) {
        vSemaphoreDelete(s_exit_ack);
        s_exit_ack = NULL;
    }

#ifdef CONFIG_AIDB_WAKE_WORD_MULTINET
    if (s_multinet_data) {
        s_multinet_iface->destroy(s_multinet_data);
        s_multinet_data = NULL;
    }
#else
    if (s_wakenet_data) {
        s_wakenet_iface->destroy(s_wakenet_data);
        s_wakenet_data = NULL;
    }
#endif

    if (s_models) {
        esp_srmodel_deinit(s_models);
        s_models = NULL;
    }
    ESP_LOGI(TAG, "Stopped");
}

bool wake_word_is_running(void)
{
    return s_running && !s_paused;
}

const char *wake_word_get_phrase(void)
{
#ifdef CONFIG_AIDB_WAKE_WORD_MULTINET
    return CONFIG_AIDB_WAKE_PHRASE;
#else
    /* Return human-readable name based on selected WakeNet model */
#if defined(CONFIG_SR_WN_WN9_JARVIS_TTS)
    return "Hey Jarvis";
#elif defined(CONFIG_SR_WN_WN9_HIESP)
    return "Hi ESP";
#elif defined(CONFIG_SR_WN_WN9_ALEXA)
    return "Alexa";
#elif defined(CONFIG_SR_WN_WN9_COMPUTER_TTS)
    return "Hey Computer";
#elif defined(CONFIG_SR_WN_WN9_HEYWILLOW_TTS)
    return "Hey Willow";
#elif defined(CONFIG_SR_WN_WN9_HIJASON_TTS2)
    return "Hi Jason";
#elif defined(CONFIG_SR_WN_WN9_HITELLY_TTS)
    return "Hi Telly";
#elif defined(CONFIG_SR_WN_WN9_HEYWANDA_TTS)
    return "Hey Wanda";
#elif defined(CONFIG_SR_WN_WN9_MYCROFT_TTS)
    return "Mycroft";
#elif defined(CONFIG_SR_WN_WN9_SOPHIA_TTS)
    return "Sophia";
#elif defined(CONFIG_SR_WN_WN9_HIJOY_TTS)
    return "Hi Joy";
#elif defined(CONFIG_SR_WN_WN9_HILILI_TTS)
    return "Hi Lily";
#elif defined(CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS)
    return "你好小智";
#elif defined(CONFIG_SR_WN_WN9_ASTROLABE_TTS)
    return "Astrolabe";
#elif defined(CONFIG_SR_WN_WN9_HIMFIVE)
    return "Hi M Five";
#else
    return "wake word";
#endif
#endif
}

static void wake_word_task(void *arg)
{
    int16_t *audio_buf = heap_caps_malloc(s_chunk_size * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM);
    if (!audio_buf) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer (%d bytes)",
                 s_chunk_size * (int)sizeof(int16_t));
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Detection task running (chunk=%d samples)", s_chunk_size);

    while (s_running) {
        if (s_paused) {
            /* Acknowledge the pause: tell wake_word_pause() that we've
             * released the I2S RX channel and won't call board_audio_record
             * until resumed. This is a deterministic handshake replacing
             * the old 110ms timing heuristic. 10ms delay keeps the task
             * responsive without burning CPU — the old 100ms caused "Pause
             * ack timeout" when wake_word_pause() was called while the task
             * was blocked in board_audio_record() (40ms read + 100ms delay
             * = 140ms, but the semaphore was already consumed from a
             * previous cycle). */
            if (s_pause_ack) {
                xSemaphoreGive(s_pause_ack);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Read mic audio — blocking call (up to ~40ms at 16kHz/512 samples).
         * Check s_paused after the read: if pause was requested while we
         * were blocked in board_audio_record(), we must acknowledge it
         * immediately instead of looping back to the top and spending
         * another 100ms in vTaskDelay before the ack. Field evidence:
         * pause ack timeout because the task was stuck in the read + delay
         * cycle while wake_word_pause() waited 500ms. */
        size_t samples_read = 0;
        esp_err_t err = board_audio_record(audio_buf, s_chunk_size, &samples_read);
        if (s_paused) {
            /* Acknowledge immediately — don't wait for next loop iteration */
            if (s_pause_ack) {
                xSemaphoreGive(s_pause_ack);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (err != ESP_OK || samples_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Diagnostic: early monitoring (first 5 chunks) + periodic (every ~30s) */
        static int diag_count = 0;
        static int early_diag = 0;
        if (early_diag < 5 || ++diag_count >= 900) {
            if (early_diag < 5) early_diag++;
            if (diag_count >= 900) diag_count = 0;
            int64_t sum_sq = 0;
            for (int i = 0; i < s_chunk_size; i++) {
                sum_sq += (int32_t)audio_buf[i] * audio_buf[i];
            }
            int rms = (int)sqrtf((float)(sum_sq / s_chunk_size));
            ESP_LOGD(TAG, "Audio monitor [%s]: RMS=%d first=[%d,%d,%d,%d]",
                     early_diag <= 5 ? "early" : "periodic",
                     rms,
                     s_chunk_size > 0 ? audio_buf[0] : 0,
                     s_chunk_size > 1 ? audio_buf[1] : 0,
                     s_chunk_size > 2 ? audio_buf[2] : 0,
                     s_chunk_size > 3 ? audio_buf[3] : 0);
        }

#ifdef CONFIG_AIDB_WAKE_WORD_MULTINET
        /* --- MultiNet detection --- */
        esp_mn_state_t state = s_multinet_iface->detect(s_multinet_data, audio_buf);
        if (state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *results = s_multinet_iface->get_results(s_multinet_data);
            ESP_LOGI(TAG, "*** Wake word detected: \"%s\" (id=%d, prob=%.2f) ***",
                     CONFIG_AIDB_WAKE_PHRASE,
                     results->command_id[0], results->prob[0]);

            if (s_event_group) {
                xEventGroupSetBits(s_event_group, s_event_bit);
            }
            s_multinet_iface->clean(s_multinet_data);
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else if (state == ESP_MN_STATE_TIMEOUT) {
            s_multinet_iface->clean(s_multinet_data);
        }
#else
        /* --- WakeNet detection --- */
        int result = s_wakenet_iface->detect(s_wakenet_data, audio_buf);
        if (result > 0) {
            const char *word = s_wakenet_iface->get_word_name(s_wakenet_data, result);
            ESP_LOGI(TAG, "*** Wake word detected: \"%s\" ***", word);

            if (s_event_group) {
                xEventGroupSetBits(s_event_group, s_event_bit);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
#endif
    }

    heap_caps_free(audio_buf);
    /* C4 fix: signal exit confirmation BEFORE clearing s_task_handle
     * and deleting the task. wake_word_stop() waits on this semaphore
     * to know it's safe to destroy the model. */
    ESP_LOGI(TAG, "Detection task exiting");
    if (s_exit_ack) {
        xSemaphoreGive(s_exit_ack);
    }
    s_task_handle = NULL;
    vTaskDelete(NULL);
}

#endif /* CONFIG_IDF_TARGET_ESP32S3 */
