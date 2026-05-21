/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * STT (Speech-to-Text) client — DashScope Fun-ASR Realtime via WebSocket
 *
 * Streams PCM audio to Alibaba DashScope (百炼) ASR service using WebSocket,
 * supports real-time streaming transcription with VAD on server side.
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum length of recognized text buffer */
#define STT_TEXT_MAX  512

/**
 * @brief STT configuration
 */
typedef struct {
    char        api_key[64];   /**< DashScope API key */
    char        model[64];     /**< Model name (e.g., "fun-asr-realtime-2026-02-28") */
    char        endpoint[128]; /**< WebSocket URL (e.g., "wss://dashscope.aliyuncs.com/api-ws/v1/inference/") */
    uint32_t    sample_rate;   /**< Audio sample rate (16000) */
    uint32_t    timeout_ms;    /**< HTTP/WS timeout */
    uint32_t    silence_ms;    /**< Max sentence silence in ms (server-side VAD, default 1300) */
} stt_config_t;

/**
 * @brief Initialize STT client with DashScope configuration.
 * @param cfg  Pointer to STT configuration
 */
esp_err_t stt_init(const stt_config_t *cfg);

/**
 * @brief Start STT session (Connect WebSocket to DashScope).
 * Call this before uploading audio chunks.
 */
esp_err_t stt_start(void);

/**
 * @brief Push PCM audio data to STT stream.
 * Audio is buffered in a ring buffer and streamed via WebSocket.
 *
 * @param pcm  Raw 16-bit mono PCM samples
 * @param len  Number of samples (not bytes)
 */
esp_err_t stt_upload_chunk(const int16_t *pcm, size_t len);

/**
 * @brief Stop STT session and get final transcription result.
 * Blocks until transcription is complete or timeout.
 *
 * @param out_text  Output buffer for transcribed text
 * @param out_len   Size of output buffer
 * @return ESP_OK on success with non-empty text
 */
esp_err_t stt_finalize(char *out_text, size_t out_len);

/**
 * @brief Check if STT has a final result ready.
 */
bool stt_has_result(void);

/**
 * @brief Abort current STT session and reset state.
 */
void stt_reset(void);

/**
 * @brief Pause audio upload (e.g., during system beep playback).
 *        Audio chunks will be silently discarded until resume is called.
 */
void stt_pause_upload(void);

/**
 * @brief Resume audio upload after pause.
 */
void stt_resume_upload(void);

#ifdef __cplusplus
}
#endif
