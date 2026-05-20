/*
 * SPDX-FileCopyrightText: 2024-2026 AIClaw Contributors
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

#ifdef CONFIG_HEYCLAWY_WAKE_WORD_MULTINET
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
static int                 s_chunk_size   = 0;

#ifdef CONFIG_HEYCLAWY_WAKE_WORD_MULTINET
static esp_mn_iface_t     *s_multinet_iface = NULL;
static model_iface_data_t *s_multinet_data  = NULL;
#else
static esp_wn_iface_t     *s_wakenet_iface = NULL;
static model_iface_data_t *s_wakenet_data  = NULL;
#endif

static void wake_word_task(void *arg);

esp_err_t wake_word_init(void)
{
#ifdef CONFIG_HEYCLAWY_WAKE_WORD_MULTINET
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

#ifdef CONFIG_HEYCLAWY_WAKE_WORD_MULTINET
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

    float threshold = CONFIG_HEYCLAWY_WAKE_THRESHOLD / 100.0f;
    s_multinet_iface->set_det_threshold(s_multinet_data, threshold);
    esp_mn_commands_clear();
    esp_mn_commands_add(1, CONFIG_HEYCLAWY_WAKE_PHRASE);
    esp_mn_error_t *errors = esp_mn_commands_update();
    if (errors) {
        ESP_LOGW(TAG, "MultiNet command errors for '%s':", CONFIG_HEYCLAWY_WAKE_PHRASE);
        for (int i = 0; errors[i].command_id != -1; i++) {
            ESP_LOGW(TAG, "  error: id=%d", errors[i].command_id);
        }
    }

    s_chunk_size = s_multinet_iface->get_samp_chunksize(s_multinet_data);
    int freq = s_multinet_iface->get_samp_rate(s_multinet_data);

    ESP_LOGI(TAG, "MultiNet ready: phrase=\"%s\" threshold=%.0f%% freq=%dHz chunk=%d",
             CONFIG_HEYCLAWY_WAKE_PHRASE, threshold * 100, freq, s_chunk_size);
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
#ifdef CONFIG_HEYCLAWY_WAKE_WORD_MULTINET
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

    /* Use PSRAM for task stack to save internal RAM */
    BaseType_t ret = xTaskCreatePinnedToCore(
        wake_word_task, "wake_word", 4096, NULL, 5, &s_task_handle, 1);
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
    s_paused = true;
    /* Wait for task to exit board_audio_record() and enter its idle delay,
     * so the I2S RX channel is no longer actively held before the caller
     * reconfigures audio (e.g., MP3 playback switching to 48kHz stereo). */
    vTaskDelay(pdMS_TO_TICKS(110));
    ESP_LOGD(TAG, "Paused");
}

void wake_word_resume(void)
{
    s_paused = false;
    ESP_LOGI(TAG, "Wake word resumed - detection task will continue");
}

void wake_word_stop(void)
{
    s_running = false;
    if (s_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(200));
        s_task_handle = NULL;
    }

#ifdef CONFIG_HEYCLAWY_WAKE_WORD_MULTINET
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
#ifdef CONFIG_HEYCLAWY_WAKE_WORD_MULTINET
    return CONFIG_HEYCLAWY_WAKE_PHRASE;
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
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Read mic audio — blocking call */
        size_t samples_read = 0;
        esp_err_t err = board_audio_record(audio_buf, s_chunk_size, &samples_read);
        if (err != ESP_OK || samples_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Diagnostic: check audio level every 900 chunks (~30 seconds) */
        static int diag_count = 0;
        if (++diag_count >= 900) {
            diag_count = 0;
            int64_t sum_sq = 0;
            for (int i = 0; i < s_chunk_size; i++) {
                sum_sq += (int32_t)audio_buf[i] * audio_buf[i];
            }
            int rms = (int)sqrtf((float)(sum_sq / s_chunk_size));
            ESP_LOGI(TAG, "Audio monitor: RMS=%d", rms);
        }

#ifdef CONFIG_HEYCLAWY_WAKE_WORD_MULTINET
        /* --- MultiNet detection --- */
        esp_mn_state_t state = s_multinet_iface->detect(s_multinet_data, audio_buf);
        if (state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *results = s_multinet_iface->get_results(s_multinet_data);
            ESP_LOGI(TAG, "*** Wake word detected: \"%s\" (id=%d, prob=%.2f) ***",
                     CONFIG_HEYCLAWY_WAKE_PHRASE,
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
    ESP_LOGI(TAG, "Detection task exiting");
    s_task_handle = NULL;
    vTaskDelete(NULL);
}

#endif /* CONFIG_IDF_TARGET_ESP32S3 */
