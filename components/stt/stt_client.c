/*
 * SPDX-FileCopyrightText: 2024-2026 AIClaw Contributors
 * SPDX-License-Identifier: MIT
 *
 * STT client — DashScope Fun-ASR Realtime via WebSocket (Streaming version)
 *
 * Adapted from VoiceClaw project for AIClaw.
 * Uses Alibaba DashScope (百炼) ASR service with WebSocket streaming.
 */

#include "stt_client.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_websocket_client.h"

static const char *TAG = "stt";

#define STT_RINGBUF_SIZE (640000) // 20 seconds at 16kHz 16-bit mono
#define STT_CHUNK_SIZE (3200)   // 100ms chunks for WebSocket transmission
#define STT_TIMEOUT_MS (5000)
#define AUDIO_RINGBUF_BYTES STT_RINGBUF_SIZE
#define WS_RX_BUFFER_SIZE    2048

typedef enum {
    STT_STATE_IDLE,
    STT_STATE_CONNECTING,
    STT_STATE_STARTED,
    STT_STATE_STREAMING,
    STT_STATE_STOPPING,
} stt_state_t;

static struct {
    stt_config_t           cfg;
    bool                   initialized;

    esp_websocket_client_handle_t ws;
    RingbufHandle_t         audio_ringbuf;
    TaskHandle_t            task_handle;

    volatile stt_state_t    state;
    char                    task_id[33];
    volatile bool           task_started;

    char                    final_text[STT_TEXT_MAX];
    SemaphoreHandle_t       result_sem;
    
    /* Upload control for avoiding beep interference */
    volatile bool           upload_paused;
} s_stt;

static esp_err_t generate_task_id(char *out, size_t len)
{
    uint32_t ticks = xTaskGetTickCount();
    uint32_t lr = esp_random();
    snprintf(out, len, "%08x%04x%04x%04x%08x",
             (unsigned)(ticks ^ lr), (unsigned)(lr >> 16),
             (unsigned)(ticks >> 8), (unsigned)(lr), (unsigned)(ticks >> 16));
    return ESP_OK;
}

static cJSON *create_run_task_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *header = cJSON_CreateObject();
    cJSON *payload = cJSON_CreateObject();
    cJSON *parameters = cJSON_CreateObject();
    cJSON *input = cJSON_CreateObject();

    cJSON_AddStringToObject(header, "action", "run-task");
    cJSON_AddStringToObject(header, "task_id", s_stt.task_id);
    cJSON_AddStringToObject(header, "streaming", "duplex");

    cJSON_AddStringToObject(payload, "task_group", "audio");
    cJSON_AddStringToObject(payload, "task", "asr");
    cJSON_AddStringToObject(payload, "function", "recognition");
    cJSON_AddStringToObject(payload, "model", s_stt.cfg.model[0] ? s_stt.cfg.model : "fun-asr-realtime-2026-02-28");

    cJSON_AddStringToObject(parameters, "format", "pcm");
    cJSON_AddNumberToObject(parameters, "sample_rate", (double)s_stt.cfg.sample_rate);
    cJSON_AddBoolToObject(parameters, "semantic_punctuation_enabled", true);
    cJSON_AddNumberToObject(parameters, "max_sentence_silence", (double)s_stt.cfg.silence_ms);
    cJSON_AddBoolToObject(parameters, "heartbeat", false);

    cJSON_AddItemToObject(payload, "parameters", parameters);
    cJSON_AddItemToObject(payload, "input", input);
    cJSON_AddItemToObject(root, "header", header);
    cJSON_AddItemToObject(root, "payload", payload);

    return root;
}

static cJSON *create_finish_task_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *header = cJSON_CreateObject();
    cJSON *payload = cJSON_CreateObject();
    cJSON *input = cJSON_CreateObject();

    cJSON_AddStringToObject(header, "action", "finish-task");
    cJSON_AddStringToObject(header, "task_id", s_stt.task_id);
    cJSON_AddStringToObject(header, "streaming", "duplex");

    cJSON_AddItemToObject(payload, "input", input);
    cJSON_AddItemToObject(root, "header", header);
    cJSON_AddItemToObject(root, "payload", payload);

    return root;
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        if (s_stt.state == STT_STATE_CONNECTING) s_stt.state = STT_STATE_STARTED;
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 1) { // TEXT frame
            char *json_str = heap_caps_malloc(data->data_len + 1, MALLOC_CAP_SPIRAM);
            if (!json_str) json_str = strndup((char *)data->data_ptr, data->data_len);
            else { memcpy(json_str, data->data_ptr, data->data_len); json_str[data->data_len] = '\0'; }
            if (!json_str) break;
            
            ESP_LOGI(TAG, "STT response: %s", json_str);
            
            cJSON *root = cJSON_Parse(json_str);
            free(json_str);
            if (!root) break;
            cJSON *header = cJSON_GetObjectItem(root, "header");
            if (!header) { cJSON_Delete(root); break; }
            const char *event_str = cJSON_GetObjectItem(header, "event")->valuestring;
            if (strcmp(event_str, "task-started") == 0) {
                s_stt.task_started = true;
                s_stt.state = STT_STATE_STREAMING;
                ESP_LOGI(TAG, "Task started: %s", s_stt.task_id);
            } else if (strcmp(event_str, "result-generated") == 0) {
                cJSON *sentence = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(root, "payload"), "output"), "sentence");
                if (!sentence) { cJSON_Delete(root); break; }
                const char *text = cJSON_GetObjectItem(sentence, "text")->valuestring;
                if (text && text[0] != '\0') {
                    if (cJSON_IsTrue(cJSON_GetObjectItem(sentence, "sentence_end"))) {
                        ESP_LOGI(TAG, "Result [final]: %s", text);
                        strlcpy(s_stt.final_text, text, sizeof(s_stt.final_text));
                    } else {
                        /* Interim result: store it as a fallback if final result never comes */
                        ESP_LOGD(TAG, "Result [interim]: %s", text);
                        strlcpy(s_stt.final_text, text, sizeof(s_stt.final_text));
                    }
                }
            } else if (strcmp(event_str, "task-finished") == 0 || strcmp(event_str, "task-failed") == 0) {
                if (strcmp(event_str, "task-failed") == 0) {
                    const char *err_msg = cJSON_GetObjectItem(header, "error_message") ? cJSON_GetObjectItem(header, "error_message")->valuestring : "Unknown";
                    const char *err_code = cJSON_GetObjectItem(header, "error_code") ? cJSON_GetObjectItem(header, "error_code")->valuestring : "N/A";
                    ESP_LOGE(TAG, "Task failed: %s (code=%s)", err_msg, err_code);
                } else {
                    /* Log detailed info about the finished task */
                    int text_len = strlen(s_stt.final_text);
                    ESP_LOGI(TAG, "Task finished. Result: '%s' (len=%d)", s_stt.final_text, text_len);
                    
                    /* If result is empty, log a warning with context */
                    if (text_len == 0) {
                        ESP_LOGW(TAG, "WARNING: STT returned empty result despite audio being sent");
                        ESP_LOGW(TAG, "This could be due to:");
                        ESP_LOGW(TAG, "  1. Audio quality issues (too quiet, too noisy)");
                        ESP_LOGW(TAG, "  2. Language mismatch (speaking Chinese but model expects English)");
                        ESP_LOGW(TAG, "  3. Network timeout during processing");
                        ESP_LOGW(TAG, "  4. Server-side recognition failure");
                    }
                }
                s_stt.state = STT_STATE_IDLE;
                xSemaphoreGive(s_stt.result_sem);
            }
            cJSON_Delete(root);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        s_stt.state = STT_STATE_IDLE;
        xSemaphoreGive(s_stt.result_sem);
        break;
    default: break;
    }
}

static void stt_task(void *arg)
{
    ESP_LOGI(TAG, "STT background task started");
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        s_stt.state = STT_STATE_CONNECTING;
        s_stt.task_started = false;
        s_stt.final_text[0] = '\0';
        generate_task_id(s_stt.task_id, sizeof(s_stt.task_id));

        char auth_header[160];
        snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s\r\n", s_stt.cfg.api_key);

        ESP_LOGI(TAG, "Connecting to DashScope STT... Free internal heap: %d",
                 (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

        esp_websocket_client_config_t ws_cfg = {
            .uri = s_stt.cfg.endpoint[0] ? s_stt.cfg.endpoint : "wss://dashscope.aliyuncs.com/api-ws/v1/inference/",
            .headers = auth_header,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .task_stack = 6144,
            .buffer_size = 2048,
            .user_agent = "AIClaw/1.0",
            .network_timeout_ms = 20000,
            .disable_auto_reconnect = true,
            /* Use global CA store to reduce internal RAM usage */
            .use_global_ca_store = true,
        };

        s_stt.ws = esp_websocket_client_init(&ws_cfg);
        esp_websocket_register_events(s_stt.ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
        esp_websocket_client_start(s_stt.ws);

        int wait_ms = 0;
        while (s_stt.state == STT_STATE_CONNECTING && wait_ms < 10000) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_ms += 100;
        }

        if (s_stt.state == STT_STATE_STARTED) {
            cJSON *run_task = create_run_task_json();
            char *run_task_str = cJSON_PrintUnformatted(run_task);
            esp_websocket_client_send_text(s_stt.ws, run_task_str, strlen(run_task_str), pdMS_TO_TICKS(1000));
            free(run_task_str);
            cJSON_Delete(run_task);

            wait_ms = 0;
            while (!s_stt.task_started && s_stt.state != STT_STATE_IDLE && wait_ms < 5000) {
                vTaskDelay(pdMS_TO_TICKS(100));
                wait_ms += 100;
            }

            if (s_stt.task_started) {
                int total_sent = 0;
                int send_count = 0;
                /* Process ringbuffer even in STOPPING state until empty */
                while (s_stt.state != STT_STATE_IDLE) {
                    size_t chunk_len = 0;
                    void *chunk = xRingbufferReceive(s_stt.audio_ringbuf, &chunk_len, pdMS_TO_TICKS(100));
                    if (chunk) {
                        int sent = esp_websocket_client_send_bin(s_stt.ws, chunk, chunk_len, pdMS_TO_TICKS(2000));
                        if (sent < 0) {
                            ESP_LOGE(TAG, "WS send failed: %d (state=%d)", sent, s_stt.state);
                            s_stt.state = STT_STATE_IDLE;
                            vRingbufferReturnItem(s_stt.audio_ringbuf, chunk);
                            break;
                        }
                        total_sent += sent;
                        send_count++;
                        if (send_count <= 5 || send_count % 100 == 0) {
                            ESP_LOGI(TAG, "Sent audio chunk #%d: %d bytes (total=%d, ret=%d, state=%d)",
                                     send_count, (int)chunk_len, total_sent, sent, s_stt.state);
                        }
                        vRingbufferReturnItem(s_stt.audio_ringbuf, chunk);
                        /* Small delay for WS processing */
                        vTaskDelay(pdMS_TO_TICKS(2));
                    } else {
                        /* Ringbuffer empty. If we were stopping, now we are actually done. */
                        if (s_stt.state == STT_STATE_STOPPING) {
                            ESP_LOGI(TAG, "Stopping: ringbuffer empty, sending finish-task");
                            break;
                        }
                    }
                }
                ESP_LOGI(TAG, "Audio upload done: %d chunks, %d bytes total. State: %d", send_count, total_sent, s_stt.state);

                if (s_stt.state != STT_STATE_IDLE) {
                    cJSON *finish_task = create_finish_task_json();
                    char *finish_task_str = cJSON_PrintUnformatted(finish_task);
                    int sent = esp_websocket_client_send_text(s_stt.ws, finish_task_str, strlen(finish_task_str), pdMS_TO_TICKS(2000));
                    ESP_LOGI(TAG, "Sent finish-task (ret=%d)", sent);
                    free(finish_task_str);
                    cJSON_Delete(finish_task);
                    
                    /* Wait for event handler to set state to IDLE after receiving task-finished */
                    int final_wait = 0;
                    while (s_stt.state != STT_STATE_IDLE && final_wait < 10000) {
                        vTaskDelay(pdMS_TO_TICKS(100));
                        final_wait += 100;
                    }
                    if (s_stt.state != STT_STATE_IDLE) {
                        ESP_LOGW(TAG, "Timeout waiting for task-finished event");
                    }
                }
            } else {
                ESP_LOGE(TAG, "STT task never started (timeout or error)");
            }
        } else {
            ESP_LOGE(TAG, "Failed to connect to DashScope STT server");
        }

        if (s_stt.ws) {
            esp_websocket_client_stop(s_stt.ws);
            esp_websocket_client_destroy(s_stt.ws);
            s_stt.ws = NULL;
        }
        s_stt.state = STT_STATE_IDLE;
        ESP_LOGI(TAG, "STT session ended");
    }
}

esp_err_t stt_init(const stt_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    s_stt.cfg = *cfg;
    if (!s_stt.audio_ringbuf) {
        /* Use NOSPLIT to preserve 3200-byte chunk boundaries */
        s_stt.audio_ringbuf = xRingbufferCreateWithCaps(AUDIO_RINGBUF_BYTES, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
    }
    if (!s_stt.result_sem) s_stt.result_sem = xSemaphoreCreateBinary();
    if (!s_stt.task_handle) {
        xTaskCreatePinnedToCore(stt_task, "stt_task", 10240, NULL, 3, &s_stt.task_handle, 1);
    }
    s_stt.initialized = true;
    s_stt.state = STT_STATE_IDLE;
    ESP_LOGI(TAG, "STT initialized (DashScope streaming, model=%s)", s_stt.cfg.model);
    return ESP_OK;
}

esp_err_t stt_start(void)
{
    if (!s_stt.initialized || s_stt.state != STT_STATE_IDLE) return ESP_FAIL;
    xTaskNotifyGive(s_stt.task_handle);
    return ESP_OK;
}

esp_err_t stt_upload_chunk(const int16_t *pcm, size_t len)
{
    if (!s_stt.initialized || !pcm || len == 0) return ESP_ERR_INVALID_ARG;
    
    /* If upload is paused (e.g., during countdown beep), discard audio to avoid
     * capturing system sounds in STT recognition */
    if (s_stt.upload_paused) {
        return ESP_OK;  /* Silently discard */
    }
    
    const size_t bytes_to_send = len * sizeof(int16_t);
    size_t sent_bytes = 0;
    
    /* Split into smaller chunks (100ms) to avoid massive WebSocket frames */
    while (sent_bytes < bytes_to_send) {
        size_t to_write = bytes_to_send - sent_bytes;
        if (to_write > STT_CHUNK_SIZE) to_write = STT_CHUNK_SIZE;
        
        if (xRingbufferSend(s_stt.audio_ringbuf, (const uint8_t *)pcm + sent_bytes, to_write, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "Audio ringbuffer full, sent %d/%d bytes", sent_bytes, bytes_to_send);
            return ESP_ERR_NO_MEM;
        }
        sent_bytes += to_write;
    }
    
    return ESP_OK;
}

void stt_pause_upload(void)
{
    s_stt.upload_paused = true;
    ESP_LOGD(TAG, "STT upload paused");
}

void stt_resume_upload(void)
{
    s_stt.upload_paused = false;
    ESP_LOGD(TAG, "STT upload resumed");
}

void stt_reset(void)
{
    s_stt.state = STT_STATE_IDLE;
    s_stt.final_text[0] = '\0';
    if (s_stt.audio_ringbuf) {
        size_t sz;
        /* For NOSPLIT ringbuffer, use xRingbufferReceive to clear items one by one */
        while (1) {
            void *item = xRingbufferReceive(s_stt.audio_ringbuf, &sz, 0);
            if (!item) break;
            vRingbufferReturnItem(s_stt.audio_ringbuf, item);
        }
    }
}

esp_err_t stt_finalize(char *out_text, size_t out_len)
{
    if (!s_stt.initialized || s_stt.state == STT_STATE_IDLE) return ESP_FAIL;
    
    /* Clear semaphore before waiting */
    xSemaphoreTake(s_stt.result_sem, 0);
    
    /* Trigger stopping phase */
    s_stt.state = STT_STATE_STOPPING;

    ESP_LOGI(TAG, "Finalizing STT, waiting for result...");

    /* Wait for task to finish and signal result_sem */
    if (xSemaphoreTake(s_stt.result_sem, pdMS_TO_TICKS(5000)) == pdTRUE) {
        if (s_stt.final_text[0] != '\0') {
            strlcpy(out_text, s_stt.final_text, out_len);
            return ESP_OK;
        }
    } else {
        ESP_LOGW(TAG, "STT finalize timeout");
    }
    return ESP_FAIL;
}

bool stt_has_result(void)
{
    return s_stt.initialized && s_stt.final_text[0] != '\0' &&
           (s_stt.state == STT_STATE_STREAMING || s_stt.state == STT_STATE_STARTED);
}
