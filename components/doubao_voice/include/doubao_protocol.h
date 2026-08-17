/*
 * SPDX-FileCopyrightText: 2024-2026 AIWatch Contributors
 * SPDX-License-Identifier: MIT
 *
 * doubao_protocol — Doubao (Volcengine) realtime duplex dialogue protocol:
 * upstream JSON construction + downstream streaming parse with WS fragment
 * reassembly. Task 5 (M2).
 *
 * Wire format: one JSON object per WSS text frame, all audio as Base64
 * inside JSON (no binary frames). Downlink events arrive split across WS
 * fragments (esp_websocket_client buffer_size=1024B), so proto_feed()
 * accumulates fragments in a static PSRAM buffer until a complete JSON
 * object is found, then dispatches one doubao_event_cb_t per event.
 *
 * Thread safety: proto_* state is static and NOT thread-safe; call from a
 * single task (the ws_client task in Task 6; proto_test from the console
 * REPL task).
 *
 * Callback data lifetime: data pointers passed to the callback are only
 * valid during the call (they point into parse state that the next event
 * overwrites). Callers must copy synchronously inside the callback.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "doubao_voice.h"   /* doubao_cfg_t, doubao_event_cb_t, doubao_audio_chunk_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Upstream builders ──────────────────────────────────────────────────
 * All builders serialize into a caller-provided buffer (static 4KB array
 * or task stack is fine) with no dynamic allocation. On success the buffer
 * is NUL-terminated and ESP_OK returned; ESP_ERR_NO_MEM if the buffer is
 * too small (check `ret` and enlarge the buffer).
 *
 * Recommend `cap` >= 4096 (session.create carries instructions; worst-case
 * escaped text is 4x input length).
 */

/* session.create. `session_id` is NULL for the first handshake, non-NULL
 * on reconnect to restore context (id field only then). */
esp_err_t proto_build_session_create(char *buf, size_t cap, const doubao_cfg_t *cfg, const char *session_id);

/* input_audio_buffer.append. PCM 16k/16bit little-endian; encoded Base64
 * straight into `buf`. 640 samples (40ms) need cap >= ~1.8KB. */
esp_err_t proto_build_audio_append(char *buf, size_t cap, const int16_t *pcm16, size_t samples, uint32_t event_id);

/* input_audio_buffer.commit — VAD judgement / forced stop. */
esp_err_t proto_build_commit(char *buf, size_t cap);

/* response.cancel — client interrupt. */
esp_err_t proto_build_cancel(char *buf, size_t cap);

/* session.close — end of dialogue. */
esp_err_t proto_build_close(char *buf, size_t cap);

/* speech_text_buffer.replacement.append — text pushed directly into the
 * TTS speech buffer (say command / intervention reply). */
esp_err_t proto_build_text_push(char *buf, size_t cap, const char *text);

/* ── Downstream stream parser ─────────────────────────────────────────── */

/* One WS text fragment (payload; NOT NUL-terminated, len is authoritative). */
typedef struct {
    char *data;
    size_t len;
} ws_frag_t;

/* Feed one WS fragment. Fragments are accumulated (static 64KB PSRAM
 * buffer) until complete JSON object(s) are available; each complete
 * object is parsed with cJSON and dispatched to `cb` as DOUBAO_EVT_*.
 * A single fragment may contain zero, one, or many events.
 *
 * On malformed JSON, ESP_LOGW + proto_reset() (recovery, no crash);
 * ESP_ERR_NO_MEM on accumulation overflow (also resets).
 */
esp_err_t proto_feed(ws_frag_t frag, doubao_event_cb_t cb);

/* Drop all accumulated fragment state (e.g. on reconnect). Does not clear
 * the session id (that belongs to the dialogue, survives reconnects). */
void proto_reset(void);

/* Session id extracted from `session.created` (for reconnect session.create),
 * NULL if no session has been created yet. */
const char *proto_get_session_id(void);

#ifdef __cplusplus
}
#endif
