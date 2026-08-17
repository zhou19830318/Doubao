/*
 * SPDX-FileCopyrightText: 2024-2026 AIWatch Contributors
 * SPDX-License-Identifier: MIT
 *
 * doubao_ws_client — WSS connection management for the Doubao realtime
 * dialogue API (Task 6, M2). Owns the esp_websocket_client lifecycle:
 * connect with exponential backoff (2→60s), X-Api-Key auth header, ping
 * 1s during handshake / 30s after, session.create on every connect
 * (resuming the previous session.id if one exists), upstream JSON frames
 * via a thread-safe TX queue, downstream fragments fed to proto_feed().
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "doubao_voice.h"   /* doubao_cfg_t, doubao_event_cb_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Start the WSS background task (connect → session.create → send/recv
 * loop). `cfg` is deep-copied; `cb` receives DOUBAO_EVT_* dispatched from
 * the websocket client task context (keep it fast, copy data
 * synchronously). Idempotent: calling while the task is already running
 * returns ESP_OK without restarting anything. */
esp_err_t dbws_start(const doubao_cfg_t *cfg, doubao_event_cb_t cb);

/* Stop the task and close the WSS connection. Idempotent. The previous
 * session.id is kept (in proto state) so the next dbws_start resumes it. */
esp_err_t dbws_stop(void);

/* Cancel any pending backoff wait and attempt an immediate reconnect.
 * While connected, forces a fresh connection (old socket is torn down).
 * No-op if the task is not running. */
void dbws_request_reconnect(void);

/* True while the WSS session is up (may be false before session.create is
 * answered — the handshake happens right after connect). */
bool dbws_is_connected(void);

/* Queue one JSON text frame for sending (thread-safe; the frame is
 * copied into an internal PSRAM queue, up to 5KB). Frames are rejected
 * while disconnected (ESP_ERR_INVALID_STATE) — they cannot be sent on a
 * dead session anyway — and ESP_ERR_TIMEOUT when the queue is full. */
esp_err_t dbws_send_frame(const char *frame, size_t len);

/* Current session id from the last `session.created` (NULL if none yet).
 * The pointer is valid until the next session event; use it immediately. */
const char *dbws_get_session_id(void);

#ifdef __cplusplus
}
#endif
