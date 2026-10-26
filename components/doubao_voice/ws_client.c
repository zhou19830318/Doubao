/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * doubao_ws_client — esp_websocket_client wiring (Task 6, M2).
 *
 * Connection lifecycle (CLAUDE.md 铁律 21/22/23):
 *  - uri = wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue,
 *    auth = X-Api-Key header only (no body auth).
 *  - ping_interval_sec = 1 at client creation (handshake phase); relaxed
 *    to 30s in WEBSOCKET_EVENT_CONNECTED right after session.create is
 *    queued; reset back to 1s on DISCONNECTED/CLOSED/ERROR (铁律 21 —
 *    every reconnect must re-enter with a fast ping).
 *  - The event handler is dispatched INLINE from the websocket client's
 *    internal task (esp_event_post_to + esp_event_loop_run(0) in this
 *    component version), so it must stay lightweight: no stop/close/
 *    destroy (铁律 22 — teardown runs in the dbws task), no blocking
 *    waits. It only flips event-group bits, queues session.create and
 *    feeds proto_feed().
 *  - Downstream: text frames arrive split across fragments (buffer_size
 *    4096). Each WEBSOCKET_EVENT_DATA carries one fragment at data_ptr
 *    with data_len bytes (payload_offset/payload_len describe its
 *    position inside the whole message); proto_feed() accumulates
 *    fragments until complete JSON objects are available (铁律 23 — no
 *    manual reassembly here).
 *  - Reconnect: disable_auto_reconnect = true — the dbws task drives an
 *    exponential backoff 2→60s (doubled after each elapsed wait, reset on
 *    success) and creates a FRESH client handle per attempt. destroy()
 *    deletes the client's private event loop, so the handler registration
 *    dies with the loop — no dangling handler.
 *  - dbws_request_reconnect() (user wake / CLI) cancels the backoff wait
 *    for an immediate retry; a failed immediate retry re-enters backoff
 *    (the level is only doubled after a wait that actually elapsed).
 *
 * Upstream: dbws_send_frame() copies the JSON frame into a static PSRAM
 * queue; the dbws task drains it while connected. Frames are rejected
 * while disconnected, and the queue is reset when a connection drops, so
 * the session.create queued by WEBSOCKET_EVENT_CONNECTED (before the
 * DBWS_BIT_CONN wake-up) is always the first frame on a fresh connection.
 */

#include "doubao_ws_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "doubao_task_mem.h"

#include "doubao_protocol.h"
#include "doubao_binary_protocol.h"

static const char *TAG = "doubao_ws";

/* Default JSON WebSocket endpoint */
#define DOUBAO_WSS_URI "wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue"
/* O2.0 binary protocol endpoint */
#define DOUBAO_O20_WSS_URI "wss://openspeech.bytedance.com/api/v3/realtime/dialogue"

/* Reconnect backoff: 2s → 4s → … → 60s cap (design doc §4.2). */
#define DBWS_BACKOFF_MIN_S    2
#define DBWS_BACKOFF_MAX_S    60

/* How long the connect phase waits for a CONNECTED/DISCONNECTED event.
 * The ws internal task blocks up to network_timeout_ms=10s per attempt. */
#define DBWS_CONNECT_WAIT_MS  (15 * 1000)

/* Connected-phase poll interval (max latency to react to a drop, and the
 * TX-drain cadence when the queue stays empty). */
#define DBWS_TX_POLL_MS       20

/* TX queue: 4 slots × 5120B = 20KB static PSRAM (frames are copied in).
 * 8192 comfortably fits a worst-case session.create (instructions ≤4096
 * chars, Chinese text escaped ~2× + JSON overhead) and audio frames (~1.8KB). */
#define DBWS_TX_SLOTS         64
#define DBWS_TX_SLOT_CAP      8192

/* WS client internal task stack. The library runs TLS handshake + frame I/O;
 * heavy proto_feed/cJSON runs in our dbws task. With CONFIG_MBEDTLS_DYNAMIC_BUFFER
 * the in/out bufs are on heap, not stack — 8KB is sufficient (verified on field).
 * 16KB causes permanent "Error create websocket task" after boot: heap_4
 * fragmentation from TLS/transport alloc-free cycles leaves no 64KB contiguous
 * block despite 2.9MB total internal free (field-confirmed on AMOLED-2.06). */
#define DBWS_TASK_STACK        12288
/* Our dbws task runs ABOVE the ws client's internal task: that task holds
 * the client recursive lock across every received message, and during
 * continuous RX bursts its give→poll(1s)→take cycle has no yield point —
 * a same-priority dbws task starved on send_text() ("Could not lock
 * ws-client within 50 timeout" + "TX queue full" floods). Priority 7 lets
 * the TX drain grab the lock the moment it is released; still below
 * capture (9) and playback (10) per 铁律 7. */
#define DBWS_TASK_PRIO        9
/* Priority 9 (was 7): the dbws task runs proto_feed → resample → ring push
 * on every AUDIO_DELTA.  At prio 7 it was preempted by the prio-10 play
 * task's PSRAM reads and the prio-9 send task, delaying ring pushes and
 * causing "ring empty" output gaps.  At prio 9 it shares the level with
 * the send task (both feed the audio pipeline) and only yields to the
 * prio-10 play task (which must not be delayed — it drains the ring to
 * I2S).  Core 0 placement avoids core-1 contention with play/capture. */
#define DBWS_WS_CLIENT_PRIO   7      /* ws client internal task — raised from 5
                                         to reduce RX overflow: at prio 5 the ws
                                         client was starved by prio-9 dbws + prio-10
                                         play tasks, delaying fragment copies into
                                         the RX ring.  At prio 7 it runs between UI
                                         (prio 5) and dbws (prio 9), fast enough to
                                         drain incoming fragments before the ring
                                         fills during audio delta bursts. */

/* Event group bits. CONN/DISC are set by the ws event handler (ws client
 * internal task context); STOP/RECONNECT are set by public API callers.
 * The dbws task consumes them. */
#define DBWS_BIT_CONN        (1 << 0)   /* WSS session up (handler) */
#define DBWS_BIT_DISC        (1 << 1)   /* WSS session down (handler) */
#define DBWS_BIT_STOP        (1 << 2)   /* stop requested (dbws_stop) */
#define DBWS_BIT_RECONNECT   (1 << 3)   /* immediate reconnect (dbws_request_reconnect) */
#define DBWS_BIT_DATA        (1 << 4)   /* rx fragment buffered (handler → task) */

/* Receive handoff ring: the ws event handler only copies the fragment
 * into a slot; the dbws task runs proto_feed (parse/decode/resample) on
 * its own context. 32 slots absorb the fragment bursts of large audio
 * deltas (the client fragments >4096-byte text frames; one ~22KB audio
 * delta = ~6 fragments, and bursts arrive several deltas back-to-back —
 * 8 slots overflowed in the field, corrupting the fragment stream).
 * ALLOCATED FROM PSRAM — a static array here lands in .bss, i.e. internal
 * DRAM, and 8×4.2KB was enough to abort the boot sequence's console REPL
 * task creation (serial_cmd ESP_ERROR_CHECK). */
#define DBWS_RX_CAP          4200   /* ws client buffer_size 4096 + margin */
#define DBWS_RX_SLOTS        256  /* 1072KB PSRAM — field log showed 78 fragments
                                     dropped in one turn with 128 slots; audio
                                     deltas arrive in bursts of 6-10 fragments
                                     each, and dbws processing (base64 decode +
                                     resample) can't keep up during back-to-back
                                     deltas. 256 slots absorbs ~40 audio deltas
                                     worth of fragments. */
static char (*s_rx_buf)[DBWS_RX_CAP];  /* PSRAM, allocated once in dbws_start */
static size_t s_rx_len[DBWS_RX_SLOTS];
static volatile bool s_rx_msg_start[DBWS_RX_SLOTS];  /* payload_offset==0:
                                                        first fragment of a
                                                        new WS message */
static volatile size_t s_rx_head, s_rx_tail;
static uint32_t s_rx_drops;

typedef struct {
    char   data[DBWS_TX_SLOT_CAP];
    size_t len;
} dbws_tx_item_t;

/* ── Static state ─────────────────────────────────────────────────────── */

/* Deep-copied config (dbws_start contract: caller pointers may be
 * stack/transient).
 * NOTE: s_instructions is a POINTER, not a buffer — it references
 * doubao_voice.c's s_instructions to avoid duplicating 4KB of internal
 * RAM. The pointer is valid because doubao_init() sets it before calling
 * dbws_start(), and doubao_disconnect() tears down the task before any
 * re-init. */
static const char *s_api_key;    /* points to doubao_voice.c's buffer */
static const char *s_voice;      /* points to doubao_voice.c's buffer */
static const char *s_instructions;/* points to doubao_voice.c's buffer */
static int8_t s_speed;
static int8_t s_loudness;
static bool s_enable_search;
static bool s_enable_music;
static doubao_event_cb_t s_cb;

static char s_headers[320];         /* "X-Api-Key: <key>\r\n" or O2.0 headers */

/* O2.0 StartSession config scratch (PSRAM, allocated once on first use).
 * 4KB holds the JSON-escaped system prompt (escaping can double the size
 * of quotes/newlines; an oversized prompt truncates at a character
 * boundary), 5KB the assembled config JSON. Both stay well inside
 * DBWS_TX_SLOT_CAP (8KB). */
static char *s_o20_role_esc;
static char *s_o20_config_json;

/* ── O2.0 Binary Protocol Mode ──────────────────────────────────────── */
static bool s_o20_mode = false;
static char s_o20_app_id[32];
static char s_o20_access_key[64];
/* O2.0 connection readiness lives in protocol.c (proto_o20_conn_ready):
 * armed when StartConnection (event 1) is acked (event 50) and only
 * invalidated by proto_o20_conn_invalidate() on a real disconnect or
 * before a fresh StartConnection. It must survive WS message boundaries.
 */

static volatile bool s_connected;   /* session up (handler-owned) */
/* CONNECTED/DISCONNECTED dispatch flags: the ws event handler runs INLINE
 * in the ws client's internal task — including during its teardown path
 * after destroy(). Calling s_cb() there (the chat handler runs go_idle()
 * with 400ms of blocking stops) kept the dying task alive for seconds
 * and raced the library's deferred resource free: 3 field crashes
 * (LoadProhibited / assert / LoadStoreAlignment in esp_websocket_client_
 * task, freed-lock access). The handler only SETS these flags now; the
 * dbws task (our own, no teardown restrictions) dispatches the events. */
static volatile bool s_evt_connected_pending = false;
static volatile bool s_evt_disconnected_pending = false;
/* Health check: track last successful send to detect dead TCP connections.
 * transport_poll_write(0) means the TCP write buffer is full and the
 * server has stopped acknowledging — the ws task spins on retries but
 * our code doesn't know the connection is dead. After 10s without a
 * successful send, force a reconnect. */
static int64_t s_last_send_ok_ms;
#define SEND_STALE_TIMEOUT_MS  10000
static volatile bool s_session_active;   /* session.created received, not yet closed */
static volatile bool s_session_pending;  /* session.create sent, created not yet received */
static volatile bool s_session_closing;  /* session.close sent, closed not yet received */
static volatile bool s_stop;        /* task exit request */
static uint32_t s_backoff_s;        /* current backoff seconds (task-owned) */

static esp_websocket_client_handle_t s_ws;   /* current client (task-owned) */
static TaskHandle_t s_task;

static EventGroupHandle_t s_evt;
static StaticEventGroup_t s_evt_ctrl;
static QueueHandle_t s_tx_q;
static StaticQueue_t s_tx_q_ctrl;
static dbws_tx_item_t *s_tx_items;  /* PSRAM, allocated once */
static char *s_session_buf;         /* PSRAM build buffer for session.create */

/* ── TX queue ─────────────────────────────────────────────────────────── */

/* Raw TX enqueue — no s_connected gate. Used by the CONNECTED handler to
 * queue session.create BEFORE s_connected becomes visible (F2: nothing may
 * overtake the first frame) and by dbws_send_frame() after the gate. */
static esp_err_t dbws_tx_enqueue(const char *frame, size_t len)
{
    if (len == 0) {
        len = strlen(frame);
    }
    if (len == 0 || len > DBWS_TX_SLOT_CAP) {
        ESP_LOGW(TAG, "frame too big for TX slot (%u > %d)",
                 (unsigned)len, DBWS_TX_SLOT_CAP);
        return ESP_ERR_NO_MEM;
    }
    dbws_tx_item_t item;
    memcpy(item.data, frame, len);
    item.len = len;
    if (uxQueueMessagesWaiting(s_tx_q) == 0) {
        /* Queue transitions idle→busy: restart the stale-send baseline.
         * Without this, a frame queued after >10s of healthy idleness
         * (e.g. the session.create of a new turn) trips the health check
         * instantly — "send stale for Ns" — and kills the connection the
         * moment the turn starts (field-confirmed). */
        s_last_send_ok_ms = esp_timer_get_time() / 1000;
    }
    /* Short enqueue timeout: when the queue is full (drain lagging behind
     * a receive burst) the send task must NOT stall long per frame —
     * drop fast so the pump cadence recovers as soon as the burst ends.
     * The queue holds ~640ms of pump frames; gaps in a silence stream
     * are harmless server-side. */
    if (xQueueSend(s_tx_q, &item, pdMS_TO_TICKS(10)) != pdTRUE) {
        /* Drop is expected during RX bursts; log at most one per 20 drops
         * (a per-frame warning is itself a blocking UART write in the
         * uplink task, compounding the backlog it reports). */
        static uint32_t s_tx_full_cnt = 0;
        if ((++s_tx_full_cnt % 20) == 1) {
            ESP_LOGW(TAG, "TX queue full — frame dropped (%u bytes, %u total)",
                     (unsigned)len, (unsigned)s_tx_full_cnt);
        }
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t dbws_send_frame(const char *frame, size_t len)
{
    if (frame == NULL || s_tx_q == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_connected) {
        /* No valid session right now — the frame could never be sent and
         * must not shadow the session.create of the next connection. */
        return ESP_ERR_INVALID_STATE;
    }
    return dbws_tx_enqueue(frame, len);
}

/* Escape a UTF-8 string for embedding inside a JSON string literal.
 * Truncation stops on a character boundary, never mid-sequence. */
static void json_escape_into(char *dst, size_t dst_cap, const char *src)
{
    if (!dst || dst_cap == 0) return;
    dst[0] = '\0';
    if (!src) return;

    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        const char *rep = NULL;
        char tmp[8];
        size_t need = 0;

        switch (*p) {
        case '"':  rep = "\\\""; need = 2; break;
        case '\\': rep = "\\\\"; need = 2; break;
        case '\n': rep = "\\n";  need = 2; break;
        case '\r': rep = "\\r";  need = 2; break;
        case '\t': rep = "\\t";  need = 2; break;
        default:
            if (*p < 0x20) {
                snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
                rep = tmp;
                need = 6;
            } else if (*p < 0x80) {
                tmp[0] = (char)*p;
                tmp[1] = '\0';
                rep = tmp;
                need = 1;
            } else {
                int len = (*p >= 0xF0) ? 4 : (*p >= 0xE0) ? 3 : 2;
                bool complete = true;
                for (int i = 1; i < len; i++) {
                    if (p[i] == 0) { complete = false; break; }
                }
                if (!complete || o + (size_t)len + 1 > dst_cap) break;
                memcpy(dst + o, p, (size_t)len);
                o += (size_t)len;
                p += len - 1;
                continue;
            }
        }
        if (o + need + 1 > dst_cap) break;
        memcpy(dst + o, rep, need);
        o += need;
    }
    dst[o] = '\0';
}

/* ── Per-conversation session lifecycle ────────────────────────────────
 * A session is opened on wake and closed when the conversation ends; an
 * idle session fed pure silence dies ASR-wise on the server after ~60s
 * (measured), so it must never be left open while idle. Multiple sessions
 * on one WS connection are supported by the server (verified against the
 * live endpoint). */

esp_err_t dbws_session_open(void)
{
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_session_active || s_session_pending) {
        return ESP_OK;   /* idempotent */
    }
    /* A previous session.close is still being processed server-side. The
     * server rejects a new session.create with 45000000 "previous session
     * is running" until it has answered session.closed — wait for that
     * ack (bounded; a stuck close is recovered by giving up). */
    if (s_session_closing) {
        for (int i = 0; i < 40 && s_session_closing && s_connected; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (s_session_closing) {
            ESP_LOGW(TAG, "session.closed not received in 2s — proceeding anyway");
        }
    }
    doubao_cfg_t cfg = { .api_key = s_api_key, .voice = s_voice,
                         .instructions = s_instructions,
                         .speed = s_speed, .loudness = s_loudness,
                         .enable_search = s_enable_search,
                         .enable_music = s_enable_music };

    if (s_o20_mode) {
        /* O2.0: must wait for StartConnection ack before creating session */
        if (!proto_o20_conn_ready()) {
            ESP_LOGW(TAG, "O2.0 connection not ready yet — waiting");
            return ESP_ERR_INVALID_STATE;
        }

        /* O2.0 binary protocol: build StartSession frame.
         * Generate a fresh session id (UUID v4) per conversation. */
        char o20_session_id[40];
        {
            uint8_t b[16];
            esp_fill_random(b, sizeof(b));
            b[6] = (uint8_t)((b[6] & 0x0F) | 0x40);  /* version 4 */
            b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);  /* variant 10xx */
            snprintf(o20_session_id, sizeof(o20_session_id),
                     "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                     b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                     b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        }

        /* JSON config payload — matches the Python demo format.
         * `model` belongs inside dialog.extra per the 1.2.1.1 API doc
         * (a root-level key is ignored); asr.extra mirrors the demo's
         * 1500ms end-of-speech smoothing. dialog.system_role carries the
         * persona plus the device-control rules — without it the model
         * has no idea it can adjust volume/brightness.
         * NOTE: enable_music requires enterprise cert, disabled for now. */
        static const char *const default_voice = "zh_female_xiaohe_jupiter_bigtts";
        const char *voice = s_voice ? s_voice : default_voice;
        const char *config_json = NULL;

        if (!s_o20_role_esc) {
            s_o20_role_esc    = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
            s_o20_config_json = heap_caps_malloc(5120, MALLOC_CAP_SPIRAM);
            if (!s_o20_role_esc || !s_o20_config_json) {
                ESP_LOGW(TAG, "O2.0 config buffers unavailable — system_role dropped");
                free(s_o20_role_esc);
                free(s_o20_config_json);
                s_o20_role_esc = NULL;
                s_o20_config_json = NULL;
            }
        }
        if (s_o20_role_esc) {
            json_escape_into(s_o20_role_esc, 4096,
                             (s_instructions && s_instructions[0]) ? s_instructions : "");
            int n = snprintf(s_o20_config_json, 5120,
                     "{\"asr\":{\"extra\":{\"end_smooth_window_ms\":1500}},"
                     "\"tts\":{\"speaker\":\"%s\","
                     "\"audio_config\":{\"channel\":1,\"format\":\"pcm_s16le\",\"sample_rate\":24000}},"
                     "\"dialog\":{\"bot_name\":\"豆包\",\"system_role\":\"%s\","
                     "\"extra\":{"
                     "\"input_mod\":\"audio\","
                     "\"enable_loudness_norm\":true,"
                     "\"recv_timeout\":30,"
                     "\"model\":\"1.2.1.1\"}}}",
                     voice, s_o20_role_esc);
            if (n > 0 && (size_t)n < 5120) {
                config_json = s_o20_config_json;
            } else {
                ESP_LOGW(TAG, "O2.0 config too long (%d bytes) — system_role dropped", n);
            }
        }
        if (!config_json) {
            static char minimal_json[768];
            snprintf(minimal_json, sizeof(minimal_json),
                     "{\"asr\":{\"extra\":{\"end_smooth_window_ms\":1500}},"
                     "\"tts\":{\"speaker\":\"%s\","
                     "\"audio_config\":{\"channel\":1,\"format\":\"pcm_s16le\",\"sample_rate\":24000}},"
                     "\"dialog\":{\"extra\":{"
                     "\"input_mod\":\"audio\","
                     "\"enable_loudness_norm\":true,"
                     "\"recv_timeout\":30,"
                     "\"model\":\"1.2.1.1\"}}}",
                     voice);
            config_json = minimal_json;
        }

        size_t frame_len = 0;
        esp_err_t ret = bin_proto_build_start_session((uint8_t *)s_session_buf, &frame_len,
                                                       DBWS_TX_SLOT_CAP,
                                                       o20_session_id,
                                                       config_json);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "O2.0 StartSession build failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "O2.0 StartSession frame (%u bytes) session=%s",
                 (unsigned)frame_len, o20_session_id);
        if (dbws_tx_enqueue(s_session_buf, frame_len) != ESP_OK) {
            ESP_LOGW(TAG, "O2.0 StartSession queueing failed");
            return ESP_ERR_TIMEOUT;
        }
    } else {
        /* JSON WebSocket protocol: build session.create */
        esp_err_t ret = proto_build_session_create(s_session_buf, DBWS_TX_SLOT_CAP,
                                                   &cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "session.create build failed: %s", esp_err_to_name(ret));
            return ret;
        }
        /* Wire-level evidence: the exact session.create JSON we send (the value
         * is \u-escaped, so it is ASCII-safe on the console). */
        ESP_LOGI(TAG, "session.create payload (%u bytes): %.512s",
                 (unsigned)strlen(s_session_buf), s_session_buf);
        if (dbws_tx_enqueue(s_session_buf, strlen(s_session_buf)) != ESP_OK) {
            ESP_LOGW(TAG, "session.create queueing failed");
            return ESP_ERR_TIMEOUT;
        }
    }
    /* Pending, NOT active: audio must not flow before session.created is
     * received — measured against the live endpoint, frames sent in that
     * window are silently dropped by the server. dbws_session_mark_created()
     * (called when session.created is parsed) opens the gate. */
    s_session_pending = true;
    ESP_LOGI(TAG, "session.create queued (qdepth=%d)",
             (int)uxQueueMessagesWaiting(s_tx_q));
    return ESP_OK;
}

void dbws_session_mark_created(void)
{
    s_session_pending = false;
    s_session_active = true;
}

esp_err_t dbws_session_close(void)
{
    if (!s_session_active && !s_session_pending) {
        return ESP_OK;   /* idempotent */
    }
    s_session_active = false;
    s_session_pending = false;
    s_session_closing = true;

    if (s_o20_mode) {
        /* O2.0 binary protocol: build FinishSession frame */
        uint8_t frame[128];
        size_t frame_len = 0;
        esp_err_t ret = bin_proto_build_finish_session(frame, &frame_len,
                                                        sizeof(frame),
                                                        proto_get_session_id());
        if (ret != ESP_OK) {
            return ret;
        }
        /* Send raw binary frame */
        if (s_ws == NULL || !s_connected) {
            return ESP_ERR_INVALID_STATE;
        }
        int sent = esp_websocket_client_send_bin(s_ws, (const char *)frame,
                                                  (int)frame_len,
                                                  pdMS_TO_TICKS(200));
        return (sent >= 0) ? ESP_OK : ESP_FAIL;
    } else {
        /* JSON WebSocket protocol: build session.close */
        char frame[128];
        esp_err_t ret = proto_build_close(frame, sizeof(frame));
        if (ret != ESP_OK) {
            return ret;
        }
        /* The session.closed ack clears s_session_closing (via
         * dbws_session_mark_closed) — the next session.create waits for it. */
        return dbws_send_frame(frame, strlen(frame));
    }
}

void dbws_session_mark_closed(void)
{
    /* Clear everything: the ack may arrive for OUR session.close, or the
     * server may have closed the session on its own (error / idle
     * timeout). Either way the session no longer exists — if s_session_
     * active survived a server-initiated close, the next wake would skip
     * session.create and upload into a dead session (everything the user
     * says silently ignored). */
    s_session_active = false;
    s_session_pending = false;
    s_session_closing = false;
}

bool dbws_session_active(void)
{
    return s_session_active;
}

/* Send all queued frames (dbws task context). A stalled socket or a
 * mid-drain disconnect stops the drain; remaining frames are stale and
 * dropped (the queue is reset on disconnect). */
/* Maximum consecutive send failures before aborting the drain. Transient
 * TLS errors (e.g. -0x6C00 CONN_RESET during network blip) recover on
 * retry; only persistent failures indicate a dead connection. */
#define DRAIN_MAX_SEND_RETRIES  3

/* Per-call drain budget. Sending and receiving share the ws client lock, so
 * an unbounded drain (up to 64 queued frames) can occupy this task for
 * seconds while every send waits 200ms for the lock — during which RX
 * fragments pile up and the 256-slot RX ring overflows. That overflow drops
 * audio frames (and sometimes tts.ended), which is exactly the
 * "播放不连贯/不完整" symptom. Bound the work and yield back to RX. */
#define DRAIN_MAX_FRAMES_PER_CALL  4
#define DRAIN_RX_BACKLOG_STOP      32   /* fragments — RX has priority above */
#define DRAIN_MAX_WALL_US          250000  /* hard per-call cap: a send that
                                            * can't get the ws lock costs up
                                            * to 200ms, and RX fragments are
                                            * dropped while this task blocks
                                            * here. One pass must never hold
                                            * the task for seconds. */

static void dbws_drain_tx(void)
{
    if (s_ws == NULL || !s_connected) {
        return;
    }
    dbws_tx_item_t item;
    int drained = 0;
    int64_t drain_start = esp_timer_get_time();
    while (xQueuePeek(s_tx_q, &item, 0) == pdTRUE) {
        if (drained >= DRAIN_MAX_FRAMES_PER_CALL) {
            break;
        }
        if ((size_t)(s_rx_head - s_rx_tail) > DRAIN_RX_BACKLOG_STOP) {
            break;
        }
        if (esp_timer_get_time() - drain_start > DRAIN_MAX_WALL_US) {
            break;
        }
        drained++;
        if (!s_connected || s_stop) {
            break;
        }
        /* Take a local snapshot of s_ws. The ws client's internal task may
         * tear down the TLS context (setting s_ws = NULL via the DISC path)
         * between our s_connected check and the actual send_text call. A
         * stale s_ws pointer → LoadProhibited in ssl_check_ctr_renegotiate
         * (field crash #1). Taking a local copy and re-checking s_connected
         * after the send closes this TOCTOU window. */
        esp_websocket_client_handle_t ws = s_ws;
        if (ws == NULL) {
            break;
        }
        /* Send timeout 200ms: the ws client's internal task holds the
         * recursive lock across esp_websocket_client_recv() — that call
         * blocks on esp_transport_read() for up to 1s per WS fragment.
         * With the old 50ms timeout, the dbws task almost always failed to
         * acquire the lock ("Could not lock ws-client within 50 timeout"
         * — 100+ times per conversation turn in field logs). 200ms lets
         * the send succeed most of the time: the ws task's recv loop
         * releases the lock between fragments, giving a 200ms window.
         * If the lock is still held (large multi-fragment message), the
         * send fails gracefully and retries next pass. */
        int retries = 0;
        int sent = 0;
        while (retries < DRAIN_MAX_SEND_RETRIES) {
            /* Re-read s_ws each retry: the handle may have been set to NULL
             * by the ws client's internal task during a concurrent disconnect. */
            ws = s_ws;
            if (ws == NULL || !s_connected) {
                break;
            }
            /* In O2.0 mode, send as binary; otherwise as text */
            if (s_o20_mode) {
                sent = esp_websocket_client_send_bin(ws, item.data, (int)item.len,
                                                     pdMS_TO_TICKS(200));
            } else {
                sent = esp_websocket_client_send_text(ws, item.data, (int)item.len,
                                                      pdMS_TO_TICKS(200));
            }
            if (sent >= 0) {
                break;  /* success */
            }
            retries++;
            if (!s_connected) {
                /* Connection died during send — stop immediately.
                 * Without this check the next retry re-enters send_text
                 * with a potentially corrupted TLS session (field crash:
                 * tlsf_free double-free in esp_mbedtls_free_tx_buffer). */
                ESP_LOGW(TAG, "tx send failed and connection lost — aborting drain");
                return;
            }
            if (retries < DRAIN_MAX_SEND_RETRIES) {
                /* RX is arriving — leave this frame queued and let the
                 * outer loop process the downlink first. Retrying now
                 * would only queue more RX behind another lock wait. */
                if ((size_t)(s_rx_head - s_rx_tail) > 8) {
                    break;
                }
                /* Brief yield: lets the ws client internal task process
                 * pending RX and release its lock. Without this, the next
                 * send_text immediately re-enters the same contention. */
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        if (sent < 0) {
            static uint32_t s_send_fail_cnt = 0;
            if ((++s_send_fail_cnt % 100) == 1) {
                ESP_LOGW(TAG, "tx send failing %u times — lock/transport contention",
                         (unsigned)s_send_fail_cnt);
            }
            break;   /* head frame retried next pass */
        }
        xQueueReceive(s_tx_q, &item, 0);   /* consume on success */
        s_last_send_ok_ms = esp_timer_get_time() / 1000;
    }
}

/* ── WS event handler (runs inline in the ws client's internal task) ──── */

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id,
                             void *data)
{
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)data;
    esp_websocket_client_handle_t ws = d ? d->client : s_ws;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        /* No auto session.create here any more: sessions are opened on
         * demand (wake word) and closed when the conversation ends. An
         * idle session starves on pure silence — ~60s of zero input puts
         * the server's ASR to sleep for the rest of the session (measured
         * against the live endpoint), so the only safe idle session is no
         * session. */
        s_session_active = false;
        s_session_pending = false;
        s_session_closing = false;
        /* Handshake over — relax ping from 1s (only needed during
         * connect/handshake) to 30s to save power. */
        if (ws != NULL) {
            esp_websocket_client_set_ping_interval_sec(ws, 30);
        }
        s_connected = true;
        s_last_send_ok_ms = esp_timer_get_time() / 1000;

        /* O2.0 mode: send StartConnection (event 1) automatically.
         * The server must acknowledge this (event 50 = ConnStarted)
         * before any session can be created. Readiness is re-armed when
         * that ack is parsed; invalidate now so a session cannot open on
         * stale readiness between connect and the ack. */
        if (s_o20_mode && s_session_buf != NULL) {
            proto_o20_conn_invalidate();
            size_t frame_len = 0;
            esp_err_t ret = bin_proto_build_start_connection(
                (uint8_t *)s_session_buf, &frame_len, DBWS_TX_SLOT_CAP);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "O2.0 StartConnection frame (%u bytes)", (unsigned)frame_len);
                dbws_tx_enqueue(s_session_buf, frame_len);
            } else {
                ESP_LOGE(TAG, "O2.0 StartConnection build failed: %s", esp_err_to_name(ret));
            }
        }

        /* The app's CONNECTING→IDLE transition is dispatched by the dbws
         * task (light handler rule — see s_evt_connected_pending). */
        s_evt_connected_pending = true;
        xEventGroupSetBits(s_evt, DBWS_BIT_CONN);
        break;
    }
    case WEBSOCKET_EVENT_DATA:
        if (d == NULL) {
            break;
        }
        if (d->op_code == 0x08) {   /* close frame — client dispatches
                                       DISCONNECTED/CLOSED right after */
            uint16_t code = 0;
            if (d->data_len >= 2) {
                code = (uint16_t)(((uint8_t)d->data_ptr[0] << 8) |
                                  (uint8_t)d->data_ptr[1]);
            }
            ESP_LOGW(TAG, "WS close frame: code=%u", code);
            break;
        }
        /* In O2.0 mode, accept binary frames (0x02); otherwise only text (0x01) */
        if (s_o20_mode) {
            if (d->op_code != 0x02 || d->data_len <= 0) {
                break;   /* only binary frames carry O2.0 protocol data */
            }
        } else {
            if (d->op_code != 0x01 || d->data_len <= 0) {
                break;   /* only text frames carry protocol JSON */
            }
        }
        /* Copy the fragment out and hand the heavy work (proto_feed →
         * cJSON parse → base64 decode → resample → ring push) to the dbws
         * task. Doing it here — inside the ws client's internal task while
         * it holds its lock — stalled send_text() past its 100ms lock
         * timeout on every big audio delta: "TX queue full" frame drops,
         * playback underruns, and the whole downlink stuttering. */
        if (d->data_len > DBWS_RX_CAP) {
            ESP_LOGW(TAG, "fragment too big (%u > %d)", (unsigned)d->data_len,
                     DBWS_RX_CAP);
            break;
        }
        size_t h = s_rx_head;
        if (h - s_rx_tail >= DBWS_RX_SLOTS) {
            s_rx_drops++;
            /* Rate-limit: an overflow burst otherwise emits one blocking
             * UART line per dropped fragment, inside the ws lock. */
            if ((s_rx_drops % 50) == 1) {
                ESP_LOGW(TAG, "rx overflow — fragment dropped (%u bytes, %u total)",
                         (unsigned)d->data_len, (unsigned)s_rx_drops);
            }
            break;
        }
        size_t slot = h % DBWS_RX_SLOTS;
        memcpy(s_rx_buf[slot], d->data_ptr, d->data_len);
        s_rx_len[slot] = d->data_len;
        s_rx_msg_start[slot] = (d->payload_offset == 0);
        s_rx_head = h + 1;

        /* Log every RX fragment at INFO level for pipeline timing analysis.
         * Shows fragment size, whether it starts a new message, and the
         * queue depth so we can see how fast fragments arrive vs. how fast
         * they are consumed by proto_feed. */
        {
            static uint32_t s_frag_cnt = 0;
            static int64_t  s_first_frag_us = 0;
            int64_t now_us = esp_timer_get_time();
            if (s_frag_cnt == 0) s_first_frag_us = now_us;
            s_frag_cnt++;
            /* Every 50th fragment: this runs in the ws client task while it
             * holds its lock, so per-fragment logging stalls TX sends. */
            if ((s_frag_cnt % 50) == 1) {
                int qdepth = (int)(s_rx_head - s_rx_tail);
                ESP_LOGI(TAG, "RX#%lu: len=%u start=%d qdepth=%d Δ=%lldms",
                         (unsigned long)s_frag_cnt,
                         (unsigned)d->data_len,
                         (int)s_rx_msg_start[slot],
                         qdepth,
                         (long long)(now_us - s_first_frag_us) / 1000);
            }
        }

        xEventGroupSetBits(s_evt, DBWS_BIT_DATA);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (s_connected) {
            /* Dispatched by the dbws task (light handler rule — see
             * s_evt_disconnected_pending). */
            s_evt_disconnected_pending = true;
        }
        s_connected = false;
        s_session_active = false;
        s_session_pending = false;
        s_session_closing = false;
        /* Connection is gone: StartConnection handshake must be repeated
         * on the next connect. proto_reset() only drops fragment state;
         * connection readiness is invalidated explicitly. */
        proto_o20_conn_invalidate();
        proto_reset();   /* drop partial fragments of the dead connection */
        s_rx_head = s_rx_tail;   /* drop buffered rx fragments */
        if (ws != NULL) {
            esp_websocket_client_set_ping_interval_sec(ws, 1);   /* 铁律 21 */
        }
        xEventGroupSetBits(s_evt, DBWS_BIT_DISC);
        break;
    case WEBSOCKET_EVENT_ERROR:
        if (d != NULL) {
            ESP_LOGE(TAG, "WS error: type=%d esp_err=%s errno=%d",
                     d->error_handle.error_type,
                     esp_err_to_name(d->error_handle.esp_tls_last_esp_err),
                     d->error_handle.esp_transport_sock_errno);
        }
        /* abort_connection() dispatches DISCONNECTED right after this;
         * waking early keeps a failed first connect from being waited out. */
        xEventGroupSetBits(s_evt, DBWS_BIT_DISC);
        break;
    default:
        break;
    }
}

/* ── Client lifecycle (dbws task context) ─────────────────────────────── */

static void dbws_destroy_client(void);   /* forward: create_client's stale-guard uses it */



static esp_err_t dbws_create_client(void)
{
    if (s_ws != NULL) {
        /* Stale handle from a raced teardown — the client's task may
         * still be alive. Destroy it (rather than returning OK, which
         * made esp_websocket_client_start() fail with "The client has
         * started" and wasted a reconnect cycle). */
        ESP_LOGW(TAG, "stale ws client handle — destroying before create");
        dbws_destroy_client();
    }
    esp_websocket_client_config_t cfg = {
        .uri = s_o20_mode ? DOUBAO_O20_WSS_URI : DOUBAO_WSS_URI,
        .headers = s_headers,                    /* "X-Api-Key: <key>\r\n" or O2.0 headers */
        .cert_pem = NULL,                        /* bundle below — server cert
                                                    verification stays ON */
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* The wss:// URI scheme overrides this field in
         * esp_websocket_client_set_uri() — TLS is always used. */
        .transport = WEBSOCKET_TRANSPORT_OVER_TCP,
        .disable_auto_reconnect = true,          /* backoff driven by dbws task */
        .buffer_size = 4096,                     /* fragments reassembled by
                                                    proto_feed (铁律 23) */
        .network_timeout_ms = 10000,
        .task_prio = DBWS_WS_CLIENT_PRIO,
        .task_stack = DBWS_TASK_STACK,
        .ping_interval_sec = 1,                  /* fast ping during handshake
                                                    (铁律 21 / ws-auth-ping-fix) */
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (s_ws == NULL) {
        ESP_LOGE(TAG, "esp_websocket_client_init failed");
        return ESP_FAIL;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    return ESP_OK;
}

static void dbws_destroy_client(void)
{
    if (s_ws != NULL) {
        /* CRITICAL: clear s_ws BEFORE destroy() to prevent the ws event
         * handler (running in the ws client's internal task) from using
         * a stale handle during teardown. The handler is dispatched
         * INLINE from the ws task, so it may fire after destroy() starts
         * freeing resources. */
        esp_websocket_client_handle_t ws = s_ws;
        s_ws = NULL;
        esp_websocket_client_destroy(ws);
        /* Wait for the ws internal task to fully exit. destroy() should
         * have joined it, but in practice the task may still be in its
         * final cleanup (freeing TLS context, etc.). A short delay
         * prevents the next create_client() from racing with the dying
         * task's final accesses.
         * Field evidence: 200ms was insufficient — the ws client's internal
         * task was still printing "Error receive data" when the next
         * create_client() found a stale handle. 500ms gives the dying task
         * time to finish its final mbedTLS free + transport close. */
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* Backoff wait; returns on the first elapsed wait, on BIT_STOP or on an
 * immediate-reconnect request (BIT_RECONNECT cancels the wait — the
 * backoff level is only doubled after a wait that actually elapsed).
 *
 * Also waits for BIT_CONN: if the ws client's internal task establishes a
 * connection during our backoff (e.g. it auto-reconnected despite
 * disable_auto_reconnect, or a stale task from the previous cycle raced
 * the destroy), we must NOT sleep through it — the CONN event means the
 * connection is ready and the dbws task should enter the connected phase
 * immediately. Field evidence: sleeping through CONN left session.create
 * stuck in the TX queue for 5s → "session.created not received". */
static void dbws_backoff_delay(void)
{
    uint32_t wait_ms = s_backoff_s * 1000;
    ESP_LOGW(TAG, "reconnect in %us...", (unsigned)s_backoff_s);
    uint32_t bits = xEventGroupWaitBits(
            s_evt,
            DBWS_BIT_STOP | DBWS_BIT_RECONNECT | DBWS_BIT_CONN,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(wait_ms));
    if (bits & DBWS_BIT_RECONNECT) {
        ESP_LOGI(TAG, "backoff cancelled — reconnecting now");
        return;
    }
    if (bits & DBWS_BIT_CONN) {
        /* Connection established during backoff — enter connected phase
         * immediately. The CONNECTED handler already set s_connected=true
         * and queued DBWS_BIT_CONN. */
        ESP_LOGI(TAG, "connection arrived during backoff — proceeding");
        s_backoff_s = DBWS_BACKOFF_MIN_S;
        if (s_evt_connected_pending) {
            s_evt_connected_pending = false;
            s_cb(DOUBAO_EVT_CONNECTED, NULL, 0);
        }
        return;
    }
    if (!(bits & DBWS_BIT_STOP)) {
        /* clamp at doubling time — 32→64 must land on 60, not overshoot */
        s_backoff_s *= 2;
        if (s_backoff_s > DBWS_BACKOFF_MAX_S) {
            s_backoff_s = DBWS_BACKOFF_MAX_S;
        }
    }
}

/* ── Background task ──────────────────────────────────────────────────── */

static void dbws_task(void *arg)
{
    ESP_LOGI(TAG, "task started (endpoint %s, o20=%d)",
             s_o20_mode ? DOUBAO_O20_WSS_URI : DOUBAO_WSS_URI,
             s_o20_mode ? 1 : 0);

    while (!s_stop) {
        if (!s_connected) {
            /* ── connect phase ── */
            if (dbws_create_client() != ESP_OK) {
                dbws_backoff_delay();
                continue;
            }
            if (esp_websocket_client_start(s_ws) != ESP_OK) {
                ESP_LOGE(TAG, "esp_websocket_client_start failed");
                dbws_destroy_client();
                dbws_backoff_delay();
                continue;
            }
            uint32_t bits = xEventGroupWaitBits(s_evt,
                    DBWS_BIT_CONN | DBWS_BIT_DISC | DBWS_BIT_STOP | DBWS_BIT_RECONNECT,
                    pdTRUE, pdFALSE, pdMS_TO_TICKS(DBWS_CONNECT_WAIT_MS));
            if (bits & DBWS_BIT_STOP || s_stop) {
                break;
            }
            if (bits & DBWS_BIT_RECONNECT) {
                /* may race a CONNECTED event that already set s_connected —
                 * the client is being torn down, so back to connect phase */
                s_connected = false;
                dbws_destroy_client();      /* immediate retry, no backoff */
                continue;
            }
            if (bits & DBWS_BIT_DISC) {
                s_connected = false;        /* defensive: handler clears it too */
                if (s_evt_disconnected_pending) {
                    s_evt_disconnected_pending = false;
                    s_cb(DOUBAO_EVT_DISCONNECTED, NULL, 0);
                }
                xQueueReset(s_tx_q);
                dbws_destroy_client();
                dbws_backoff_delay();
                continue;
            }
            if (bits & DBWS_BIT_CONN) {
                s_backoff_s = DBWS_BACKOFF_MIN_S;
                if (s_evt_connected_pending) {
                    s_evt_connected_pending = false;
                    s_cb(DOUBAO_EVT_CONNECTED, NULL, 0);
                }
                continue;                   /* → connected phase */
            }
            /* No event within the wait — attempt stalled, tear down. */
            ESP_LOGW(TAG, "no connection event within %d s — retrying",
                     DBWS_CONNECT_WAIT_MS / 1000);
            dbws_destroy_client();
            dbws_backoff_delay();
        } else {
            /* ── connected phase ── */
        if (s_stop) {
            break;
        }
        /* Periodic connection health log — every 10s while idle */
        {
            static uint32_t s_conn_tick = 0;
            if (++s_conn_tick >= (10000 / DBWS_TX_POLL_MS)) {
                s_conn_tick = 0;
                int qd = (int)uxQueueMessagesWaiting(s_tx_q);
                int64_t now_ms = esp_timer_get_time() / 1000;
                ESP_LOGI(TAG, "conn health: qdepth=%d stale=%llds session=%d drops=%u",
                         qd, (now_ms - s_last_send_ok_ms) / 1000,
                         (int)s_session_active, (unsigned)s_rx_drops);
            }
        }
            uint32_t bits = xEventGroupWaitBits(s_evt,
                    DBWS_BIT_DISC | DBWS_BIT_STOP | DBWS_BIT_RECONNECT |
                    DBWS_BIT_DATA,
                    pdTRUE, pdFALSE, pdMS_TO_TICKS(DBWS_TX_POLL_MS));
            /* Process RX fragments in batches of 8 to avoid starving TX
             * for the entire duration of a burst.  Each proto_feed call
             * does cJSON parse + base64 decode + resample + ring push —
             * ~5-15ms per audio delta fragment.  A burst of 20+ fragments
             * would block TX for 100-300ms, causing the silence pump to
             * miss its 20ms cadence and the server to kill the session
             * on audio-idle timeout.  Batching 8 fragments then yielding
             * to TX keeps both paths alive. */
            {
                uint32_t drops = s_rx_drops;
                int batch = 0;
                while (s_rx_tail < s_rx_head && batch < 8) {
                    size_t slot = s_rx_tail % DBWS_RX_SLOTS;
                    if (s_rx_msg_start[slot] || drops != s_rx_drops) {
                        proto_reset();
                        drops = s_rx_drops;
                    }
                    ws_frag_t frag = { .data = s_rx_buf[slot],
                                       .len = s_rx_len[slot] };
                    s_rx_tail++;

                    int64_t feed_start = esp_timer_get_time();
                    esp_err_t ret = proto_feed(frag, s_cb);
                    int64_t feed_us = esp_timer_get_time();

                    if (ret != ESP_OK) {
                        ESP_LOGW(TAG, "proto_feed: %s", esp_err_to_name(ret));
                    }
                    /* Sample every 20th feed: per-fragment logging costs
                     * ~5ms of blocking UART per 4KB fragment. */
                    static uint32_t s_feed_cnt = 0;
                    if ((++s_feed_cnt % 20) == 1) {
                        ESP_LOGI(TAG, "feed[%d]: len=%u start=%d → %s (%lldms)",
                                 batch, (unsigned)frag.len,
                                 (int)s_rx_msg_start[slot],
                                 esp_err_to_name(ret),
                                 (long long)(feed_us - feed_start) / 1000);
                    }
                    batch++;
                }
            }
            /* TX drain — always attempt, even if RX ring has more data.
             * The silence pump must maintain its 20ms cadence to keep the
             * server session alive. */
            dbws_drain_tx();
            if (bits & DBWS_BIT_STOP || s_stop) {
                break;
            }
            if (bits & DBWS_BIT_RECONNECT) {
                ESP_LOGI(TAG, "forced reconnect requested");
                s_connected = false;
                xQueueReset(s_tx_q);
                dbws_destroy_client();
                continue;                   /* immediate, no backoff */
            }
            if (bits & DBWS_BIT_DISC) {
                s_connected = false;
                if (s_evt_disconnected_pending) {
                    s_evt_disconnected_pending = false;
                    s_cb(DOUBAO_EVT_DISCONNECTED, NULL, 0);
                }
                xQueueReset(s_tx_q);        /* frames of the dead connection
                                               are stale */
                dbws_destroy_client();
                dbws_backoff_delay();
            }
            /* Health check: only trigger when there are frames QUEUED but
             * none successfully sent. An idle connection (empty TX queue)
             * is perfectly healthy — pings keep it alive. False "stale"
             * detection on idle connections caused repeated reconnect
             * cycles that fragmented internal DRAM → crash. */
            if (s_connected && s_last_send_ok_ms > 0) {
                int qdepth = (int)uxQueueMessagesWaiting(s_tx_q);
                if (qdepth > 0) {
                    int64_t now_ms = esp_timer_get_time() / 1000;
                    if (now_ms - s_last_send_ok_ms > SEND_STALE_TIMEOUT_MS) {
                        ESP_LOGW(TAG, "send stale for %llds (%d frames stuck) — forcing reconnect",
                                 (now_ms - s_last_send_ok_ms) / 1000, qdepth);
                        s_connected = false;
                        xQueueReset(s_tx_q);
                        dbws_destroy_client();
                        dbws_backoff_delay();
                    }
                }
            }
        }
    }

    /* teardown */
    s_connected = false;
    xQueueReset(s_tx_q);
    dbws_destroy_client();
    s_task = NULL;
    ESP_LOGI(TAG, "task exited");
    vTaskDeleteWithCaps(NULL);  /* PSRAM stack — see doubao_task_mem.h */
}

/* ── Public API ───────────────────────────────────────────────────────── */

esp_err_t dbws_start(const doubao_cfg_t *cfg, doubao_event_cb_t cb)
{
    if (cfg == NULL || cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task != NULL) {
        if (!s_stop) {
            return ESP_OK;   /* already running — idempotent */
        }
        /* F1: a previous dbws_stop() returned before the task fully
         * exited — it may still be joining the ws client task (up to
         * network_timeout_ms if a connect attempt is in flight, since
         * destroy→stop waits portMAX_DELAY for STOPPED_BIT). s_stop is
         * set, so the task is exiting on its own; join it here (bounded)
         * so the restart below begins from clean state. Otherwise the
         * request_reconnect() bit set right after this call would be
         * swallowed by the dying task's `s_stop` break → silent no-op. */
        ESP_LOGW(TAG, "previous task still shutting down — joining...");
        for (int i = 0; i < 150 && s_task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (s_task != NULL) {
            ESP_LOGE(TAG, "previous task failed to exit in 15s");
            return ESP_ERR_TIMEOUT;
        }
    }
    if (s_evt == NULL) {
        s_evt = xEventGroupCreateStatic(&s_evt_ctrl);
    }

    /* Store pointers to doubao_voice.c's buffers (avoids duplicating
     * api_key/voice/instructions — saves ~4.2KB internal RAM).
     * Valid because doubao_disconnect() tears down the task before any
     * re-init overwrites these buffers. */
    s_api_key = cfg->api_key ? cfg->api_key : "";
    s_voice = cfg->voice ? cfg->voice : "";
    s_instructions = cfg->instructions ? cfg->instructions : "";
    s_speed = cfg->speed;
    s_loudness = cfg->loudness;
    s_enable_search = cfg->enable_search;
    s_enable_music = cfg->enable_music;
    s_cb = cb;

    /* Build headers based on protocol mode */
    if (s_o20_mode) {
        /* O2.0 binary protocol uses different authentication headers.
         * X-Api-Connect-Id is required by the server. */
        char connect_id[48];
        snprintf(connect_id, sizeof(connect_id), "%08lx-%04x-%04x",
                 (unsigned long)(esp_random() & 0xFFFFFFFF),
                 (unsigned)(esp_random() & 0xFFFF),
                 (unsigned)(esp_random() & 0xFFFF));
        snprintf(s_headers, sizeof(s_headers),
                 "X-Api-App-ID: %s\r\n"
                 "X-Api-Access-Key: %s\r\n"
                 "X-Api-Resource-Id: volc.speech.dialog\r\n"
                 "X-Api-App-Key: PlgvMymc7f3tQnJ6\r\n"
                 "X-Api-Connect-Id: %s\r\n",
                 s_o20_app_id, s_o20_access_key, connect_id);
        ESP_LOGI(TAG, "O2.0 mode: app_id=%s connect_id=%s", s_o20_app_id, connect_id);
        proto_set_o20_mode(true);
    } else {
        /* JSON WebSocket protocol uses X-Api-Key */
        snprintf(s_headers, sizeof(s_headers), "X-Api-Key: %s\r\n", s_api_key);
        proto_set_o20_mode(false);
    }

    /* The library logs every transient send-lock timeout as ERROR
     * ("Could not lock ws-client...") — that is EXPECTED contention with
     * the RX path, not a failure (frames retry). Real connection errors
     * still surface through our own ws_event_handler logs, so demote the
     * library tag to WARN to keep field logs clean. */
    esp_log_level_set("websocket_client", ESP_LOG_WARN);

    /* lazily allocate PSRAM queue storage + session.create buffer + rx ring */
    if (s_tx_q == NULL) {
        s_tx_items = heap_caps_malloc(DBWS_TX_SLOTS * sizeof(dbws_tx_item_t),
                                      MALLOC_CAP_SPIRAM);
        s_session_buf = heap_caps_malloc(DBWS_TX_SLOT_CAP, MALLOC_CAP_SPIRAM);
        s_rx_buf = heap_caps_malloc(DBWS_RX_SLOTS * DBWS_RX_CAP, MALLOC_CAP_SPIRAM);
        if (s_tx_items == NULL || s_session_buf == NULL || s_rx_buf == NULL) {
            ESP_LOGE(TAG, "no PSRAM for TX queue / session buffer / rx ring");
            /* partial failure must not leak the surviving allocations */
            if (s_tx_items != NULL) {
                heap_caps_free(s_tx_items);
                s_tx_items = NULL;
            }
            if (s_session_buf != NULL) {
                heap_caps_free(s_session_buf);
                s_session_buf = NULL;
            }
            if (s_rx_buf != NULL) {
                heap_caps_free(s_rx_buf);
                s_rx_buf = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
        s_tx_q = xQueueCreateStatic(DBWS_TX_SLOTS, sizeof(dbws_tx_item_t),
                                    (uint8_t *)s_tx_items, &s_tx_q_ctrl);
    }
    xQueueReset(s_tx_q);

    s_stop = false;
    s_connected = false;
    s_evt_connected_pending = false;
    s_evt_disconnected_pending = false;
    s_backoff_s = DBWS_BACKOFF_MIN_S;
    /* Clear every bit incl. a sticky BIT_STOP left over from a previous
     * stop that broke out of a wait without consuming it (s_stop bool is
     * the durable stop signal; the bit is only a wake-up). */
    s_rx_head = s_rx_tail = 0;
    xEventGroupClearBits(s_evt, DBWS_BIT_CONN | DBWS_BIT_DISC |
                                DBWS_BIT_RECONNECT | DBWS_BIT_STOP |
                                DBWS_BIT_DATA);

    if (DB_TASK_CREATE_PSRAM(dbws_task, "dbws", DBWS_TASK_STACK, NULL,
                             DBWS_TASK_PRIO, &s_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t dbws_stop(void)
{
    if (s_task == NULL) {
        return ESP_OK;   /* idempotent */
    }
    s_stop = true;
    xEventGroupSetBits(s_evt, DBWS_BIT_STOP);
    /* bounded wait for the task to exit (it may join the ws client task
     * for up to network_timeout_ms if a connect attempt is in flight) */
    for (int i = 0; i < 30 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_task != NULL) {
        ESP_LOGW(TAG, "task still shutting down — continuing");
    }
    return ESP_OK;
}

void dbws_request_reconnect(void)
{
    if (s_task == NULL) {
        ESP_LOGW(TAG, "reconnect requested but task not running");
        return;
    }
    xEventGroupSetBits(s_evt, DBWS_BIT_RECONNECT);
}

bool dbws_is_connected(void)
{
    return s_connected;
}

int dbws_tx_queue_depth(void)
{
    return (s_tx_q != NULL) ? (int)uxQueueMessagesWaiting(s_tx_q) : 0;
}

const char *dbws_get_session_id(void)
{
    return proto_get_session_id();
}

void dbws_set_o20_mode(bool enable, const char *app_id, const char *access_key)
{
    s_o20_mode = enable;
    if (app_id) snprintf(s_o20_app_id, sizeof(s_o20_app_id), "%s", app_id);
    if (access_key) snprintf(s_o20_access_key, sizeof(s_o20_access_key), "%s", access_key);
}

bool dbws_is_o20(void)
{
    return s_o20_mode;
}
