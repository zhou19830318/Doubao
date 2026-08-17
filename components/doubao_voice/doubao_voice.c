/*
 * SPDX-FileCopyrightText: 2024-2026 AIWatch Contributors
 * SPDX-License-Identifier: MIT
 *
 * doubao_voice — Doubao (Volcengine) realtime voice robot client.
 *
 * Task 4 skeleton: public API locked, stub bodies only.
 * Real logic lands in later tasks:
 *   - Task 5:  protocol.c (Doubao binary protocol / JSON payloads)
 *   - Task 6:  ws_client.c (esp_websocket_client wiring) — lifecycle
 *              wired here (init/connect/disconnect/status/session id)
 *   - Task 8:  doubao_send_audio/commit (upstream frames via dbws_send_frame)
 *   - Task 10: resampler.c (24k/16bit TTS PCM -> codec rate)
 */

#include "doubao_voice.h"

#include "esp_log.h"
#include <string.h>

#include "doubao_ws_client.h"
#include "doubao_protocol.h"

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

/* ── Event dispatch ──────────────────────────────────────────────────── */
/* Forwarding hook: ws_client (via proto_feed and connection events) calls
 * this from the websocket client task context. */

static void dispatch(doubao_event_type_t type, const void *data, size_t len)
{
    if (s_cb != NULL) {
        s_cb(type, data, len);
    }
}

/* Build a cfg view over the deep-copied statics (stable pointers). */
static void cfg_view(doubao_cfg_t *out)
{
    out->api_key = s_api_key;
    out->voice = s_voice;
    out->instructions = s_instructions;
    out->speed = s_speed;
    out->loudness = s_loudness;
}

/* ── Public API ──────────────────────────────────────────────────────── */

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
    s_cb       = cb;   /* NULL allowed: connection-only mode (CLI test) */

    doubao_cfg_t view;
    cfg_view(&view);
    return dbws_start(&view, dispatch);
}

esp_err_t doubao_connect(void)
{
    if (s_cb == NULL && s_api_key[0] == '\0') {
        /* init() was never called with a key */
        return ESP_ERR_INVALID_STATE;
    }
    doubao_cfg_t view;
    cfg_view(&view);
    esp_err_t ret = dbws_start(&view, dispatch);   /* restart after stop; no-op if running */
    if (ret != ESP_OK) {
        return ret;
    }
    if (!dbws_is_connected()) {
        dbws_request_reconnect();   /* cancel backoff → connect now */
    }
    return ESP_OK;
}

esp_err_t doubao_disconnect(void)
{
    return dbws_stop();
}

bool doubao_is_connected(void)
{
    return dbws_is_connected();
}

esp_err_t doubao_send_audio(const int16_t *pcm16, size_t samples)
{
    (void)pcm16;
    (void)samples;
    ESP_LOGW(TAG, "not implemented yet (Task 8)");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t doubao_commit_audio(void)
{
    ESP_LOGW(TAG, "not implemented yet (Task 8)");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t doubao_interrupt(void)
{
    ESP_LOGW(TAG, "not implemented yet (Task 8)");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t doubao_clear_session(void)
{
    ESP_LOGW(TAG, "not implemented yet (Task 8)");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t doubao_push_text(const char *text)
{
    /* Task 7 (M2): text E2E — say <文本> 直推。协议（PRD PDF"干预模型回复"）：
     * speech_text_buffer.replacement.append（流式文本）→
     * speech_text_buffer.replacement.commit（结束包）→ 服务端开始生成。 */
    if (text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!dbws_is_connected()) {
        ESP_LOGW(TAG, "push_text: not connected — 先 doubao_connect() 再重试");
        return ESP_ERR_INVALID_STATE;
    }
    /* 上行 JSON 帧静态缓冲：文本 4x 转义 + 帧头开销，256B 文本需 ~1.1KB */
    char frame[2048];
    esp_err_t ret = proto_build_text_push(frame, sizeof(frame), text);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = dbws_send_frame(frame, strlen(frame));
    if (ret != ESP_OK) {
        return ret;
    }
    ret = proto_build_text_commit(frame, sizeof(frame));
    if (ret != ESP_OK) {
        return ret;
    }
    return dbws_send_frame(frame, strlen(frame));
}

const char *doubao_get_session_id(void)
{
    return dbws_get_session_id();
}
