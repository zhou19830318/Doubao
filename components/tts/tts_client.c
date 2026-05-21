/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * TTS client — MiMo (Xiaomi MiMo-V2-TTS) streaming via HTTP SSE
 *
 * Provider: MiMo (Xiaomi): OpenAI-compatible /v1/chat/completions, SSE streaming PCM16
 * Each SSE event: "data: {json with audio.data (base64 pcm16)}"
 * Sample rate: 24000Hz, mono, 16-bit (resampled to 16kHz for playback)
 *
 * Adapted from VoiceClaw project for AIWearable.
 */

#include "tts_client.h"
#include "board.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "tts";

/* ── Internal constants ── */
#define MIMO_LINE_BUF_SIZE   (64 * 1024)  /* 64KB for large base64 audio chunks */
#define MIMO_PCM_INIT_SIZE   (256 * 1024) /* 256KB initial PCM buffer */
#define MIMO_PCM_MAX_SIZE    (3 * 1024 * 1024) /* 3MB max (~98s @16kHz 16bit mono) */
#define MIMO_READ_BUF_SIZE   4096

/* ── Module state ── */
static struct {
    tts_config_t cfg;
    bool         playing;
    bool         stop_requested;
    StaticTask_t *play_tcb;      /**< TCB for play task (alloc once, reused) */
    StackType_t  *play_stack;    /**< Stack for play task (alloc once, reused) */
} s_tts;

/* ── Playback task notification bits ── */
#define MIMO_NOTIFY_DATA       (1 << 0)
#define MIMO_NOTIFY_DONE       (1 << 1)
#define MIMO_NOTIFY_STREAM_END (1 << 2)

/* ── Shared stream state between SSE receiver and playback task ── */
typedef struct {
    uint8_t      *pcm_buf;       /**< PSRAM buffer for accumulated PCM data */
    size_t        pcm_capacity;  /**< Current buffer capacity */
    size_t        pcm_write_pos; /**< Bytes written by SSE receiver (producer) */
    size_t        pcm_read_pos;  /**< Bytes consumed by playback task (consumer) */
    volatile bool stream_done;   /**< True after all SSE chunks received */
    volatile bool has_error;     /**< True on fatal error */
    uint32_t      src_rate;      /**< Source sample rate (from WAV header or default) */
    uint16_t      src_ch;        /**< Source channels (from WAV header or default) */
    TaskHandle_t  play_task;     /**< Playback task handle */
} mimo_stream_t;

/* Forward declarations */
static void mimo_append_pcm(mimo_stream_t *st, const uint8_t *data, size_t len);
static void mimo_play_task(void *arg);

/* ── Resample PCM data to 16kHz (linear interpolation) ── */
static int16_t *resample_to_16k(const int16_t *pcm, size_t pcm_len,
                                uint32_t src_rate, uint16_t src_ch,
                                size_t *out_samples)
{
    if (!pcm || pcm_len == 0 || src_rate == 0 || src_ch == 0) return NULL;

    size_t in_samples = pcm_len / sizeof(int16_t);

    /* Stereo -> mono first if needed */
    int16_t *mono_buf = NULL;
    size_t mono_samples = in_samples;
    if (src_ch == 2 && in_samples >= 2) {
        mono_samples = in_samples / 2;
        mono_buf = heap_caps_malloc(mono_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!mono_buf) mono_buf = malloc(mono_samples * sizeof(int16_t));
        if (!mono_buf) return NULL;
        for (size_t i = 0; i < mono_samples; i++) {
            int32_t s = ((int32_t)pcm[i * 2] + (int32_t)pcm[i * 2 + 1]) / 2;
            mono_buf[i] = (int16_t)s;
        }
    }

    const int16_t *src = mono_buf ? mono_buf : pcm;
    size_t src_count = mono_buf ? mono_samples : in_samples;

    if (src_rate == 16000) {
        *out_samples = src_count;
        if (mono_buf) return mono_buf;
        int16_t *copy = heap_caps_malloc(src_count * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!copy) copy = malloc(src_count * sizeof(int16_t));
        if (copy) memcpy(copy, src, src_count * sizeof(int16_t));
        if (mono_buf) free(mono_buf);
        return copy;
    }

    float ratio = (float)16000 / (float)src_rate;
    size_t dst_count = (size_t)(src_count * ratio);
    if (dst_count == 0) { if (mono_buf) free(mono_buf); return NULL; }

    int16_t *dst = heap_caps_malloc(dst_count * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!dst) dst = malloc(dst_count * sizeof(int16_t));
    if (!dst) { if (mono_buf) free(mono_buf); return NULL; }

    for (size_t i = 0; i < dst_count; i++) {
        float src_idx = (float)i / ratio;
        size_t idx0 = (size_t)src_idx;
        size_t idx1 = idx0 + 1;
        if (idx1 >= src_count) idx1 = src_count - 1;
        float frac = src_idx - idx0;
        dst[i] = (int16_t)(src[idx0] * (1.0f - frac) + src[idx1] * frac);
    }

    if (mono_buf) free(mono_buf);
    *out_samples = dst_count;
    return dst;
}

/* ── Append PCM data to the stream buffer, growing as needed ── */
static void mimo_append_pcm(mimo_stream_t *st, const uint8_t *data, size_t len)
{
    if (!st || !data || len == 0) return;

    /* Grow buffer if needed */
    if (st->pcm_write_pos + len > st->pcm_capacity) {
        size_t new_cap = st->pcm_capacity * 2;
        while (new_cap < st->pcm_write_pos + len && new_cap < MIMO_PCM_MAX_SIZE) {
            new_cap *= 2;
        }
        if (new_cap > MIMO_PCM_MAX_SIZE) new_cap = MIMO_PCM_MAX_SIZE;
        if (st->pcm_write_pos + len > new_cap) {
            ESP_LOGW(TAG, "PCM buffer full, truncating %zu bytes",
                     len - (new_cap - st->pcm_write_pos));
            len = new_cap - st->pcm_write_pos;
        }
        uint8_t *new_buf = heap_caps_realloc(st->pcm_buf, new_cap, MALLOC_CAP_SPIRAM);
        if (!new_buf) {
            ESP_LOGE(TAG, "PCM buffer grow failed");
            return;
        }
        st->pcm_buf = new_buf;
        st->pcm_capacity = new_cap;
    }

    memcpy(st->pcm_buf + st->pcm_write_pos, data, len);
    st->pcm_write_pos += len;
}

/* ── Playback task: reads PCM from shared buffer, resamples, plays ── */
static void mimo_play_task(void *arg)
{
    mimo_stream_t *st = (mimo_stream_t *)arg;
    const size_t play_chunk = 2048;
    const size_t prebuf_bytes = (st->src_rate / 5) * sizeof(int16_t);
    bool first_play = true;

    ESP_LOGI(TAG, "MiMo play task started (prebuf=%zu bytes)", prebuf_bytes);

    uint32_t notify_val = 0;
    bool running = true;

    while (running) {
        xTaskNotifyWait(0, MIMO_NOTIFY_DATA | MIMO_NOTIFY_DONE | MIMO_NOTIFY_STREAM_END,
                         &notify_val, pdMS_TO_TICKS(100));

        if (notify_val & MIMO_NOTIFY_STREAM_END) break;
        if (s_tts.stop_requested) { ESP_LOGI(TAG, "MiMo play task aborted"); break; }

        /* Pre-buffering: wait until enough data before first play */
        if (first_play && !st->stream_done) {
            size_t available = st->pcm_write_pos - st->pcm_read_pos;
            if (available < prebuf_bytes) continue;
            ESP_LOGI(TAG, "MiMo prebuf done (%zu bytes), starting playback", available);
            first_play = false;
        }

        /* Play all available unplayed data */
        while (1) {
            size_t write_pos = st->pcm_write_pos;
            size_t read_pos = st->pcm_read_pos;

            if (read_pos >= write_pos) {
                if (st->stream_done) running = false;
                break;
            }

            size_t unplayed_bytes = write_pos - read_pos;

            /* Resample unplayed portion */
            size_t out_samples = 0;
            int16_t *resampled = resample_to_16k(
                (const int16_t *)(st->pcm_buf + read_pos), unplayed_bytes,
                st->src_rate, st->src_ch, &out_samples);

            if (resampled && out_samples > 0) {
                size_t sent = 0;
                while (sent < out_samples) {
                    if (s_tts.stop_requested) break;
                    size_t remain = out_samples - sent;
                    size_t this_chunk = (remain < play_chunk) ? remain : play_chunk;
                    size_t written = 0;
                    board_audio_play(resampled + sent, this_chunk, &written);
                    sent += this_chunk;
                }
                free(resampled);
            } else if (resampled) {
                free(resampled);
            }

            st->pcm_read_pos = write_pos;

            if (st->pcm_write_pos <= write_pos) break;
        }
    }

    ESP_LOGI(TAG, "MiMo play task exiting");
    st->play_task = NULL;
    vTaskDelete(NULL);
}

/* ── Run one MiMo-V2-TTS streaming session via HTTP SSE ── */
static void run_mimo_session(const char *text)
{
    const char *api_key = s_tts.cfg.mimo_api_key;
    const char *url     = s_tts.cfg.mimo_url;
    const char *model   = s_tts.cfg.mimo_model;
    const char *voice   = s_tts.cfg.mimo_voice;

    if (!url || !url[0]) url = "https://api.xiaomimimo.com/v1/chat/completions";
    if (!model || !model[0]) model = "mimo-v2-tts";
    if (!voice || !voice[0]) voice = "mimo_default";
    ESP_LOGI(TAG, "MiMo URL: %s", url);

    /* Build request JSON */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddBoolToObject(root, "stream", true);

    /* Audio config */
    cJSON *audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "format", "pcm16");
    cJSON_AddStringToObject(audio, "voice", voice);
    cJSON_AddItemToObject(root, "audio", audio);

    /* Messages: assistant message with the text to synthesize */
    cJSON *messages = cJSON_CreateArray();
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "assistant");
    cJSON_AddStringToObject(msg, "content", text);
    cJSON_AddItemToArray(messages, msg);
    cJSON_AddItemToObject(root, "messages", messages);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!body) {
        ESP_LOGE(TAG, "MiMo JSON build failed");
        return;
    }
    ESP_LOGI(TAG, "MiMo request: %s", body);

    /* HTTP client config */
    esp_http_client_config_t http_cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = 60000,
        .buffer_size       = 4096,
        .buffer_size_tx    = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        ESP_LOGE(TAG, "MiMo HTTP client init failed");
        free(body);
        return;
    }

    /* Set headers */
    esp_http_client_set_header(client, "api-key", api_key);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, strlen(body));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MiMo HTTP open failed: 0x%x", err);
        free(body);
        esp_http_client_cleanup(client);
        return;
    }

    int written = esp_http_client_write(client, body, strlen(body));
    free(body);

    if (written < 0) {
        ESP_LOGE(TAG, "MiMo HTTP write failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    /* Fetch response headers */
    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "MiMo HTTP %d, content_len=%d", status, content_len);

    if (status != 200) {
        int read_sz = (content_len > 0 && content_len < 1024) ? content_len : 1024;
        char *err_buf = heap_caps_malloc(read_sz + 1, MALLOC_CAP_SPIRAM);
        if (!err_buf) err_buf = malloc(read_sz + 1);
        if (err_buf) {
            int n = esp_http_client_read(client, err_buf, read_sz);
            if (n > 0) { err_buf[n] = '\0'; ESP_LOGE(TAG, "MiMo error response: %s", err_buf); }
            free(err_buf);
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    /* Allocate SSE line buffer in PSRAM */
    char *line_buf = heap_caps_malloc(MIMO_LINE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!line_buf) {
        ESP_LOGE(TAG, "SSE line buffer alloc failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    /* Initialize shared stream state */
    mimo_stream_t stream = {
        .pcm_buf       = heap_caps_malloc(MIMO_PCM_INIT_SIZE, MALLOC_CAP_SPIRAM),
        .pcm_capacity  = MIMO_PCM_INIT_SIZE,
        .pcm_write_pos = 0,
        .pcm_read_pos  = 0,
        .stream_done   = false,
        .has_error     = false,
        .src_rate      = 24000,
        .src_ch        = 1,
        .play_task     = NULL,
    };

    if (!stream.pcm_buf) {
        ESP_LOGE(TAG, "PCM buffer alloc failed");
        free(line_buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    /* Lazy-allocate play task static resources once, shared across all sessions.
     * Previously each TTS session allocated+then-freed TCB/stack, but if the
     * idle task hadn't processed vTaskDelete yet, freeing the TCB caused a
     * StoreProhibited crash when FreeRTOS later walked the terminated-tasks list. */
    #define MIMO_PLAY_STACK 8192
    if (!s_tts.play_tcb) {
        s_tts.play_tcb = heap_caps_calloc(1, sizeof(StaticTask_t),
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_tts.play_stack) {
        s_tts.play_stack = heap_caps_calloc(1, MIMO_PLAY_STACK,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_tts.play_tcb || !s_tts.play_stack) {
        ESP_LOGE(TAG, "Play task static alloc failed (tcb=%p stack=%p)",
                 (void *)s_tts.play_tcb, (void *)s_tts.play_stack);
        free(line_buf);
        free(stream.pcm_buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    stream.play_task = xTaskCreateStaticPinnedToCore(mimo_play_task, "mimo_play",
                     MIMO_PLAY_STACK, &stream, 4,
                     s_tts.play_stack, s_tts.play_tcb, 1);
    if (!stream.play_task) {
        ESP_LOGE(TAG, "Play task create failed");
        free(line_buf);
        free(stream.pcm_buf);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    int line_pos = 0;
    int sse_lines = 0;
    int audio_chunks = 0;
    bool done = false;

    char *read_buf = heap_caps_malloc(MIMO_READ_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!read_buf) read_buf = malloc(MIMO_READ_BUF_SIZE);
    if (!read_buf) {
        ESP_LOGE(TAG, "MiMo read buffer alloc failed");
        free(line_buf);
        stream.stream_done = true;
        if (stream.play_task) xTaskNotify(stream.play_task, MIMO_NOTIFY_DONE, eSetBits);
        goto mimo_wait_play;
    }

    ESP_LOGI(TAG, "MiMo reading SSE stream (streaming playback)...");

    while (!done) {
        if (s_tts.stop_requested) {
            ESP_LOGI(TAG, "MiMo SSE aborted by tts_stop");
            break;
        }
        int n = esp_http_client_read(client, read_buf, MIMO_READ_BUF_SIZE);
        if (n < 0) {
            if (audio_chunks > 0) {
                ESP_LOGI(TAG, "MiMo read error after %d chunks, assuming stream end", audio_chunks);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            n = esp_http_client_read(client, read_buf, MIMO_READ_BUF_SIZE);
            if (n <= 0) {
                ESP_LOGE(TAG, "MiMo initial read failed");
                break;
            }
        }
        if (n == 0) break;

        for (int i = 0; i < n; i++) {
            char ch = read_buf[i];

            if (ch == '\n') {
                line_buf[line_pos] = '\0';
                line_pos = 0;
                sse_lines++;

                if (line_buf[0] == '\0' || line_buf[0] == ':') continue;

                char *json_str = line_buf;
                if (strncmp(line_buf, "data: ", 6) == 0) {
                    json_str = line_buf + 6;
                } else if (strncmp(line_buf, "data:", 5) == 0) {
                    json_str = line_buf + 5;
                } else {
                    continue;
                }

                if (strcmp(json_str, "[DONE]") == 0) {
                    ESP_LOGI(TAG, "MiMo SSE [DONE], total PCM=%zu bytes, chunks=%d",
                             stream.pcm_write_pos, audio_chunks);
                    done = true;
                    continue;
                }

                cJSON *jroot = cJSON_Parse(json_str);
                if (!jroot) continue;

                cJSON *choices = cJSON_GetObjectItem(jroot, "choices");
                if (choices && cJSON_IsArray(choices)) {
                    cJSON *choice = cJSON_GetArrayItem(choices, 0);
                    if (choice) {
                        cJSON *delta = cJSON_GetObjectItem(choice, "delta");
                        if (!delta) delta = cJSON_GetObjectItem(choice, "message");

                        if (delta) {
                            cJSON *audio_item = cJSON_GetObjectItem(delta, "audio");
                            if (audio_item) {
                                cJSON *b64 = cJSON_GetObjectItem(audio_item, "data");
                                if (b64 && cJSON_IsString(b64) && b64->valuestring[0]) {
                                    size_t b64_len = strlen(b64->valuestring);
                                    size_t max_dec = (b64_len * 3) / 4 + 4;
                                    uint8_t *raw = heap_caps_malloc(max_dec, MALLOC_CAP_SPIRAM);
                                    if (!raw) raw = malloc(max_dec);
                                    if (raw) {
                                        size_t dec_len = 0;
                                        int ret = mbedtls_base64_decode(raw, max_dec, &dec_len,
                                                                        (const unsigned char *)b64->valuestring,
                                                                        b64_len);
                                        if (ret == 0 && dec_len > 0) {
                                            audio_chunks++;
                                            size_t append_start = stream.pcm_write_pos;

                                            /* Detect WAV header on first chunk */
                                            if (audio_chunks == 1 && dec_len > 44 &&
                                                raw[0] == 'R' && raw[1] == 'I' &&
                                                raw[2] == 'F' && raw[3] == 'F') {
                                                size_t pos = 12;
                                                while (pos < dec_len - 8) {
                                                    if (memcmp(raw + pos, "fmt ", 4) == 0) {
                                                        if (pos + 24 <= dec_len) {
                                                            stream.src_ch = (uint8_t)raw[pos + 10] |
                                                                            ((uint8_t)raw[pos + 11] << 8);
                                                            stream.src_rate = (uint8_t)raw[pos + 12] |
                                                                             ((uint8_t)raw[pos + 13] << 8) |
                                                                             ((uint8_t)raw[pos + 14] << 16) |
                                                                             ((uint8_t)raw[pos + 15] << 24);
                                                            ESP_LOGI(TAG, "MiMo WAV: rate=%lu ch=%d",
                                                                     (unsigned long)stream.src_rate, stream.src_ch);
                                                        }
                                                        break;
                                                    }
                                                    uint32_t csz = (uint8_t)raw[pos + 4] |
                                                                   ((uint8_t)raw[pos + 5] << 8) |
                                                                   ((uint8_t)raw[pos + 6] << 16) |
                                                                   ((uint8_t)raw[pos + 7] << 24);
                                                    pos += 8 + csz;
                                                    if (csz % 2 != 0) pos++;
                                                }
                                                dec_len -= 44;
                                                if (dec_len > 0) mimo_append_pcm(&stream, raw + 44, dec_len);
                                            } else {
                                                mimo_append_pcm(&stream, raw, dec_len);
                                            }

                                            /* Notify playback task about new data */
                                            if (stream.pcm_write_pos > append_start && stream.play_task) {
                                                xTaskNotify(stream.play_task, MIMO_NOTIFY_DATA, eSetBits);
                                            }
                                        }
                                        free(raw);
                                    }
                                }
                            }
                        }
                    }
                }
                cJSON_Delete(jroot);
            } else if (ch != '\r' && line_pos < (MIMO_LINE_BUF_SIZE - 16)) {
                line_buf[line_pos++] = ch;
            }
        }
    }

    free(read_buf);
    free(line_buf);

mimo_wait_play:
    stream.stream_done = true;
    if (stream.play_task) xTaskNotify(stream.play_task, MIMO_NOTIFY_DONE, eSetBits);

    /* Wait for playback task to finish */
    ESP_LOGI(TAG, "MiMo waiting for playback to finish...");
    int wait_ticks = 0;
    while (stream.play_task != NULL && wait_ticks < 30000 / portTICK_PERIOD_MS) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_ticks++;
    }
    if (stream.play_task) {
        ESP_LOGW(TAG, "MiMo play task still running, forcing stop");
        xTaskNotify(stream.play_task, MIMO_NOTIFY_STREAM_END, eSetBits);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "MiMo total played: %zu/%zu bytes", stream.pcm_read_pos, stream.pcm_write_pos);
    free(stream.pcm_buf);

    /* TCB and stack are shared across sessions — do NOT free them here.
     * FreeRTOS idle task still holds a reference to the TCB on the
     * xTasksWaitingTermination list until it runs. Freeing now would
     * cause a use-after-free crash on the next scheduler iteration. */

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "MiMo TTS session ended");
}

/* ── Public API ── */

esp_err_t tts_init(const tts_config_t *config)
{
    memset(&s_tts, 0, sizeof(s_tts));
    s_tts.cfg = *config;
    ESP_LOGI(TAG, "TTS init: MiMo model=%s voice=%s",
             s_tts.cfg.mimo_model ? s_tts.cfg.mimo_model : "(null)",
             s_tts.cfg.mimo_voice ? s_tts.cfg.mimo_voice : "(null)");
    return ESP_OK;
}

esp_err_t tts_speak(const char *text)
{
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    if (s_tts.playing) {
        ESP_LOGW(TAG, "Already playing, stopping first");
        tts_stop();
    }

    s_tts.playing = true;
    s_tts.stop_requested = false;

    run_mimo_session(text);

    s_tts.playing = false;
    return ESP_OK;
}

void tts_stop(void)
{
    s_tts.stop_requested = true;
    int timeout = 50;
    while (s_tts.playing && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    /* Reset state for next TTS session */
    if (!s_tts.playing) {
        s_tts.stop_requested = false;
    }
}

bool tts_is_playing(void)
{
    return s_tts.playing;
}
