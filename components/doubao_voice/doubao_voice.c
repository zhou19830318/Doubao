/*
 * SPDX-FileCopyrightText: 2024-2026 AIWatch Contributors
 * SPDX-License-Identifier: MIT
 *
 * doubao_voice — Doubao (Volcengine) realtime voice robot client.
 *
 * Task 4 skeleton: public API locked, stub bodies only.
 * Real logic lands in later tasks:
 *   - Task 5:  protocol.c (Doubao binary protocol / JSON payloads)
 *   - Task 6:  ws_client.c (esp_websocket_client wiring)
 *   - Task 10: resampler.c (24k/16bit TTS PCM -> codec rate)
 */

#include "doubao_voice.h"

#include "esp_log.h"
#include <string.h>

static const char *TAG = "doubao_voice";

/* ── Static state ────────────────────────────────────────────────────── */
/* cfg strings are deep-copied into fixed-size buffers on init so the
 * caller's pointers can never dangle (cfg may be stack-allocated). */

static char s_api_key[64];
static char s_voice[64];
static char s_instructions[1024];
static int8_t s_speed;
static int8_t s_loudness;

static doubao_event_cb_t s_cb;
static bool s_connected = false;

/* ── Event dispatch ──────────────────────────────────────────────────── */
/* Stub: no events are generated yet; forwarding hook ready for later tasks. */

static void dispatch(doubao_event_type_t type, const void *data, size_t len)
{
    if (s_cb != NULL) {
        s_cb(type, data, len);
    }
}

/* ── Public API (stubs) ──────────────────────────────────────────────── */

esp_err_t doubao_init(const doubao_cfg_t *cfg, doubao_event_cb_t cb)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_api_key, cfg->api_key ? cfg->api_key : "", sizeof(s_api_key) - 1);
    s_api_key[sizeof(s_api_key) - 1] = '\0';
    strncpy(s_voice, cfg->voice ? cfg->voice : "", sizeof(s_voice) - 1);
    s_voice[sizeof(s_voice) - 1] = '\0';
    strncpy(s_instructions, cfg->instructions ? cfg->instructions : "",
            sizeof(s_instructions) - 1);
    s_instructions[sizeof(s_instructions) - 1] = '\0';
    s_speed    = cfg->speed;
    s_loudness = cfg->loudness;
    s_cb       = cb;

    ESP_LOGW(TAG, "not implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t doubao_connect(void)
{
    ESP_LOGW(TAG, "not implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t doubao_disconnect(void)
{
    ESP_LOGW(TAG, "not implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}

bool doubao_is_connected(void)
{
    return false;
}

esp_err_t doubao_send_audio(const int16_t *pcm16, size_t samples)
{
    ESP_LOGW(TAG, "not implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t doubao_commit_audio(void)
{
    ESP_LOGW(TAG, "not implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t doubao_interrupt(void)
{
    ESP_LOGW(TAG, "not implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t doubao_clear_session(void)
{
    ESP_LOGW(TAG, "not implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t doubao_push_text(const char *text)
{
    ESP_LOGW(TAG, "not implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}

const char *doubao_get_session_id(void)
{
    return NULL;
}
