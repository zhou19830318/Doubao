/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
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
    DOUBAO_EVT_SESSION_CLOSED,       // data: 无（服务端确认 session.close）
    DOUBAO_EVT_ERROR,                // data: const char* 错误描述
} doubao_event_type_t;

typedef struct { const int16_t *pcm24; size_t samples; } doubao_audio_chunk_t;

typedef struct {
    const char *api_key;      // X-Api-Key
    const char *voice;        // 如 "zh_female_vv_jupiter_bigtts"
    const char *instructions; // 系统提示词
    int8_t speed;             // [-50,100]
    int8_t loudness;          // [-50,100]
    bool enable_search;       // 联网搜索（tool_search）
    bool enable_music;        // 歌唱功能（dialog.extra.enable_music）
} doubao_cfg_t;

typedef void (*doubao_event_cb_t)(doubao_event_type_t type, const void *data, size_t len);

/* Per-frame callback: called by send task for each captured PCM frame.
 * Return true to trigger commit_audio (VAD silence detected). */
typedef bool (*doubao_frame_cb_t)(const int16_t *pcm, size_t samples);

/* ── API ─────────────────────────────────────────────────────────────── */

esp_err_t doubao_init(const doubao_cfg_t *cfg, doubao_event_cb_t cb);
void doubao_set_frame_cb(doubao_frame_cb_t cb);

/* Uplink mute: substitute zero PCM for mic audio while keeping the frame
 * cadence unbroken.
 *
 * The protocol requires a live session to be fed audio continuously —
 * stopping the stream trips the server's audio-idle timeout (the
 * 52000033 / AudioServerNoAudioInputTooLongError teardown). So the
 * capture+send path runs from session.created to session.close and is
 * never halted; muting only changes what the frames *carry*.
 *
 * Muted while idle, so the model does not transcribe and answer ambient
 * room conversation, and during playback, so it does not hear its own
 * TTS through the speaker (no AEC on this board yet). */
void doubao_set_uplink_muted(bool muted);
bool doubao_uplink_is_muted(void);
esp_err_t doubao_connect(void);
esp_err_t doubao_disconnect(void);
bool doubao_is_connected(void);

/* Per-conversation session lifecycle: open on wake, close when the
 * conversation ends. The WSS connection itself stays up across sessions
 * (the server supports session.create after session.close on one
 * connection — verified). An idle session must not exist: ~60s of pure
 * silence puts the server-side ASR to sleep for the rest of the session. */
esp_err_t doubao_ensure_session(void);
esp_err_t doubao_close_session(void);
bool doubao_session_active(void);
esp_err_t doubao_send_audio(const int16_t *pcm16, size_t samples); // 上行帧，异步入队
esp_err_t doubao_commit_audio(void);
esp_err_t doubao_interrupt(void);
esp_err_t doubao_clear_session(void);
esp_err_t doubao_push_text(const char *text); // speech_text_buffer 直推（say 命令）
const char *doubao_get_session_id(void);

#ifdef __cplusplus
}
#endif
