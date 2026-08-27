/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * doubao_ws_client — WSS connection management for the Doubao realtime
 * dialogue API (Task 6, M2). Owns the esp_websocket_client lifecycle:
 * connect with exponential backoff (2→60s), X-Api-Key auth header, ping
 * 1s during handshake / 30s after, on-demand session.create per
 * conversation (never left open while idle), upstream JSON frames
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

/* Number of frames currently queued for sending (0 if not initialised).
 * Used by the uplink silence pump for adaptive throttling. */
int dbws_tx_queue_depth(void);

/* Current session id from the last `session.created` (NULL if none yet).
 * The pointer is valid until the next session event; use it immediately. */
const char *dbws_get_session_id(void);

/* Per-conversation session lifecycle. Sessions are opened on wake and
 * closed when the conversation ends — an idle session fed silence dies
 * ASR-wise on the server after ~60s, and multiple sessions on one
 * connection are supported (both verified against the live endpoint).
 * dbws_session_open() sends session.create (idempotent while active);
 * dbws_session_close() sends session.close (fire-and-forget). */
esp_err_t dbws_session_open(void);
esp_err_t dbws_session_close(void);
bool dbws_session_active(void);
/* Called when a session.created is parsed: opens the uplink gate. Audio
 * sent between session.create and session.created is dropped by the
 * server, so the gate must stay closed until this runs. */
void dbws_session_mark_created(void);
/* Called when session.closed is parsed: unblocks the next session.open
 * (the server rejects session.create while the previous close is still
 * being processed — 45000000). */
void dbws_session_mark_closed(void);

#ifdef __cplusplus
}
#endif
