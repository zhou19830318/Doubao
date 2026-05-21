/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * TTS client — MiMo (Xiaomi) streaming text-to-speech via SSE
 *
 * Uses Xiaomi MiMo-V2-TTS API (OpenAI chat/completions compatible)
 * with SSE streaming for real-time PCM16 audio output.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum text length per synthesis request (bytes) */
#define TTS_TEXT_MAX  2048

/**
 * @brief TTS configuration
 */
typedef struct {
    const char *mimo_api_key;  /**< MiMo API key */
    const char *mimo_url;      /**< MiMo endpoint URL */
    const char *mimo_model;    /**< MiMo model name (e.g., "mimo-v2-tts") */
    const char *mimo_voice;    /**< MiMo voice name (e.g., "mimo_default") */
} tts_config_t;

/**
 * @brief Initialize TTS client with MiMo configuration.
 * @param config  Pointer to TTS configuration
 */
esp_err_t tts_init(const tts_config_t *config);

/**
 * @brief Speak text: fetches audio from MiMo TTS server and plays through speaker.
 *
 * This is a blocking call — it streams and plays audio in chunks.
 * Returns ESP_OK when playback is complete.
 *
 * @param text  Text to synthesize
 * @return ESP_OK on success
 */
esp_err_t tts_speak(const char *text);

/**
 * @brief Stop any current playback.
 */
void tts_stop(void);

/**
 * @brief Check if TTS is currently playing.
 */
bool tts_is_playing(void);

#ifdef __cplusplus
}
#endif
