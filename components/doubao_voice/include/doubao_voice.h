/*
 * SPDX-FileCopyrightText: 2024-2026 AIWatch Contributors
 * SPDX-License-Identifier: MIT
 *
 * doubao_voice — Doubao (Volcengine) realtime voice robot client.
 *
 * Public API locked at Task 4 (M2). All later doubao tasks build on this.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Events ──────────────────────────────────────────────────────────── */

typedef enum {
    DOUBAO_EVT_CONNECTED, DOUBAO_EVT_DISCONNECTED,
    DOUBAO_EVT_SESSION_CREATED,      // data: 无（用 doubao_get_session_id()）
    DOUBAO_EVT_TRANSCRIPT_DELTA,     // data: const char* 用户文本片段
    DOUBAO_EVT_TRANSCRIPT_DONE,      // data: const char* 完整识别文本
    DOUBAO_EVT_OUTPUT_TEXT_DELTA,    // data: const char* 回复文本片段
    DOUBAO_EVT_OUTPUT_TEXT_DONE,     // data: const char* 完整回复
    DOUBAO_EVT_AUDIO_STARTED,        // data: const char* tts_type
    DOUBAO_EVT_AUDIO_DELTA,          // data: doubao_audio_chunk_t*（PCM 24k/16bit，PSRAM 缓冲，回调返回后即失效）
    DOUBAO_EVT_AUDIO_DONE,           // data: int* status_code（20000002=退出意图）
    DOUBAO_EVT_RESPONSE_DONE,        // data: 无（本轮结束）
    DOUBAO_EVT_INTERRUPTED,          // data: 无（服务端确认打断）
    DOUBAO_EVT_ERROR,                // data: const char* 错误描述
} doubao_event_type_t;

typedef struct { const int16_t *pcm24; size_t samples; } doubao_audio_chunk_t;

typedef struct {
    const char *api_key;      // X-Api-Key
    const char *voice;        // 如 "zh_female_vv_jupiter_bigtts"
    const char *instructions; // 系统提示词
    int8_t speed;             // [-50,100]
    int8_t loudness;          // [-50,100]
} doubao_cfg_t;

typedef void (*doubao_event_cb_t)(doubao_event_type_t type, const void *data, size_t len);

/* ── API ─────────────────────────────────────────────────────────────── */

esp_err_t doubao_init(const doubao_cfg_t *cfg, doubao_event_cb_t cb);
esp_err_t doubao_connect(void);
esp_err_t doubao_disconnect(void);
bool doubao_is_connected(void);
esp_err_t doubao_send_audio(const int16_t *pcm16, size_t samples); // 上行帧，异步入队
esp_err_t doubao_commit_audio(void);
esp_err_t doubao_interrupt(void);
esp_err_t doubao_clear_session(void);
esp_err_t doubao_push_text(const char *text); // speech_text_buffer 直推（say 命令）
const char *doubao_get_session_id(void);

#ifdef __cplusplus
}
#endif
