/*
 * SPDX-FileCopyrightText: 2024-2026 AIWatch Contributors
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
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "doubao_protocol.h"

static const char *TAG = "doubao_ws";

#define DOUBAO_WSS_URI "wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue"

/* Reconnect backoff: 2s → 4s → … → 60s cap (design doc §4.2). */
#define DBWS_BACKOFF_MIN_S    2
#define DBWS_BACKOFF_MAX_S    60

/* How long the connect phase waits for a CONNECTED/DISCONNECTED event.
 * The ws internal task blocks up to network_timeout_ms=10s per attempt. */
#define DBWS_CONNECT_WAIT_MS  (15 * 1000)

/* Connected-phase poll interval (max latency to react to a drop, and the
 * TX-drain cadence when the queue stays empty). */
#define DBWS_TX_POLL_MS       100

/* TX queue: 4 slots × 5120B = 20KB static PSRAM (frames are copied in).
 * 5120 comfortably fits a worst-case session.create (instructions ≤1024
 * chars, escaped 4× + JSON overhead) and audio frames (~1.8KB). */
#define DBWS_TX_SLOTS         4
#define DBWS_TX_SLOT_CAP      5120

#define DBWS_TASK_STACK       8192   /* 内部 RAM (铁律 3) */
#define DBWS_TASK_PRIO        5      /* 播放 > 采集 > WS 网络 > UI (铁律 7) */

/* Event group bits. CONN/DISC are set by the ws event handler (ws client
 * internal task context); STOP/RECONNECT are set by public API callers.
 * The dbws task consumes them. */
#define DBWS_BIT_CONN        (1 << 0)   /* WSS session up (handler) */
#define DBWS_BIT_DISC        (1 << 1)   /* WSS session down (handler) */
#define DBWS_BIT_STOP        (1 << 2)   /* stop requested (dbws_stop) */
#define DBWS_BIT_RECONNECT   (1 << 3)   /* immediate reconnect (dbws_request_reconnect) */

typedef struct {
    char   data[DBWS_TX_SLOT_CAP];
    size_t len;
} dbws_tx_item_t;

/* ── Static state ─────────────────────────────────────────────────────── */

/* Deep-copied config (dbws_start contract: caller pointers may be
 * stack/transient). */
static char s_api_key[64];
static char s_voice[64];
static char s_instructions[1024];
static int8_t s_speed;
static int8_t s_loudness;
static doubao_event_cb_t s_cb;

static char s_headers[96];          /* "X-Api-Key: <key>\r\n" (strdup'd by the client) */

static volatile bool s_connected;   /* session up (handler-owned) */
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
    if (xQueueSend(s_tx_q, &item, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "TX queue full — frame dropped (%u bytes)", (unsigned)len);
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

/* Send all queued frames (dbws task context). A stalled socket or a
 * mid-drain disconnect stops the drain; remaining frames are stale and
 * dropped (the queue is reset on disconnect). */
static void dbws_drain_tx(void)
{
    if (s_ws == NULL) {
        return;
    }
    dbws_tx_item_t item;
    while (xQueueReceive(s_tx_q, &item, 0) == pdTRUE) {
        if (!s_connected || s_stop) {
            break;
        }
        int sent = esp_websocket_client_send_text(s_ws, item.data, (int)item.len,
                                                  pdMS_TO_TICKS(100));
        if (sent < 0) {
            ESP_LOGW(TAG, "send failed — dropping %u queued frame(s)",
                     (unsigned)(uxQueueMessagesWaiting(s_tx_q) + 1));
            break;
        }
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
        /* session.create — with the previous session.id if one exists so
         * the server restores the dialogue context (design doc §4.4). */
        doubao_cfg_t cfg = { .api_key = s_api_key, .voice = s_voice,
                             .instructions = s_instructions,
                             .speed = s_speed, .loudness = s_loudness };
        const char *sid = proto_get_session_id();
        esp_err_t ret = proto_build_session_create(s_session_buf, DBWS_TX_SLOT_CAP,
                                                   &cfg, sid);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "session.create build failed: %s", esp_err_to_name(ret));
        } else {
            if (sid != NULL) {
                ESP_LOGI(TAG, "reconnect with session.id=%.40s", sid);
            }
            /* F2: enqueue via the internal bypass BEFORE s_connected is
             * set — a concurrent dbws_send_frame() that passes the gate
             * only after this enqueue completes can never overtake the
             * first frame. */
            if (dbws_tx_enqueue(s_session_buf, strlen(s_session_buf)) != ESP_OK) {
                ESP_LOGW(TAG, "session.create queueing failed");
            }
        }
        /* Handshake over — relax ping from 1s (only needed during
         * connect/handshake) to 30s to save power. */
        if (ws != NULL) {
            esp_websocket_client_set_ping_interval_sec(ws, 30);
        }
        /* Open the gate LAST: from here on user frames may queue, and the
         * dbws task wakes to drain — session.create is already queued, so
         * it is always the first frame sent on this connection. */
        s_connected = true;
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
        if (d->op_code != 0x01 || d->data_len <= 0) {
            break;   /* only text frames carry protocol JSON */
        }
        {
            ws_frag_t frag = { .data = (char *)d->data_ptr,
                               .len = (size_t)d->data_len };
            esp_err_t ret = proto_feed(frag, s_cb);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "proto_feed: %s", esp_err_to_name(ret));
            }
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (s_connected) {
            s_cb(DOUBAO_EVT_DISCONNECTED, NULL, 0);
        }
        s_connected = false;
        proto_reset();   /* drop partial fragments of the dead connection */
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

static esp_err_t dbws_create_client(void)
{
    if (s_ws != NULL) {
        return ESP_OK;
    }
    esp_websocket_client_config_t cfg = {
        .uri = DOUBAO_WSS_URI,
        .headers = s_headers,                    /* "X-Api-Key: <key>\r\n" */
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
        .task_prio = DBWS_TASK_PRIO,
        .task_stack = 8192,                      /* proto_feed/cJSON runs in
                                                    this task's context */
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
        /* destroy() stops the client and deletes its private event loop —
         * the handler registration dies with the loop, no dangling
         * handler. Joins the ws internal task, which may take up to
         * network_timeout_ms if a connect attempt is in flight. */
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
}

/* Backoff wait; returns on the first elapsed wait, on BIT_STOP or on an
 * immediate-reconnect request (BIT_RECONNECT cancels the wait — the
 * backoff level is only doubled after a wait that actually elapsed). */
static void dbws_backoff_delay(void)
{
    uint32_t wait_ms = s_backoff_s * 1000;
    ESP_LOGW(TAG, "reconnect in %us...", (unsigned)s_backoff_s);
    uint32_t bits = xEventGroupWaitBits(s_evt, DBWS_BIT_STOP | DBWS_BIT_RECONNECT,
                                        pdTRUE, pdFALSE, pdMS_TO_TICKS(wait_ms));
    if (bits & DBWS_BIT_RECONNECT) {
        ESP_LOGI(TAG, "backoff cancelled — reconnecting now");
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
    ESP_LOGI(TAG, "task started (endpoint %s)", DOUBAO_WSS_URI);

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
                xQueueReset(s_tx_q);
                dbws_destroy_client();
                dbws_backoff_delay();
                continue;
            }
            if (bits & DBWS_BIT_CONN) {
                s_backoff_s = DBWS_BACKOFF_MIN_S;
                continue;                   /* → connected phase */
            }
            /* No event within the wait — attempt stalled, tear down. */
            ESP_LOGW(TAG, "no connection event within %d s — retrying",
                     DBWS_CONNECT_WAIT_MS / 1000);
            dbws_destroy_client();
            dbws_backoff_delay();
        } else {
            /* ── connected phase ── */
            dbws_drain_tx();
            if (s_stop) {
                break;
            }
            uint32_t bits = xEventGroupWaitBits(s_evt,
                    DBWS_BIT_DISC | DBWS_BIT_STOP | DBWS_BIT_RECONNECT,
                    pdTRUE, pdFALSE, pdMS_TO_TICKS(DBWS_TX_POLL_MS));
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
                xQueueReset(s_tx_q);        /* frames of the dead connection
                                               are stale */
                dbws_destroy_client();
                dbws_backoff_delay();
            }
        }
    }

    /* teardown */
    s_connected = false;
    xQueueReset(s_tx_q);
    dbws_destroy_client();
    s_task = NULL;
    ESP_LOGI(TAG, "task exited");
    vTaskDelete(NULL);
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

    /* deep-copy the config */
    strncpy(s_api_key, cfg->api_key ? cfg->api_key : "", sizeof(s_api_key) - 1);
    s_api_key[sizeof(s_api_key) - 1] = '\0';
    strncpy(s_voice, cfg->voice ? cfg->voice : "", sizeof(s_voice) - 1);
    s_voice[sizeof(s_voice) - 1] = '\0';
    strncpy(s_instructions, cfg->instructions ? cfg->instructions : "",
            sizeof(s_instructions) - 1);
    s_instructions[sizeof(s_instructions) - 1] = '\0';
    s_speed = cfg->speed;
    s_loudness = cfg->loudness;
    s_cb = cb;

    snprintf(s_headers, sizeof(s_headers), "X-Api-Key: %s\r\n", s_api_key);

    /* lazily allocate PSRAM queue storage + session.create buffer */
    if (s_tx_q == NULL) {
        s_tx_items = heap_caps_malloc(DBWS_TX_SLOTS * sizeof(dbws_tx_item_t),
                                      MALLOC_CAP_SPIRAM);
        s_session_buf = heap_caps_malloc(DBWS_TX_SLOT_CAP, MALLOC_CAP_SPIRAM);
        if (s_tx_items == NULL || s_session_buf == NULL) {
            ESP_LOGE(TAG, "no PSRAM for TX queue / session buffer");
            /* partial failure must not leak the surviving allocation */
            if (s_tx_items != NULL) {
                heap_caps_free(s_tx_items);
                s_tx_items = NULL;
            }
            if (s_session_buf != NULL) {
                heap_caps_free(s_session_buf);
                s_session_buf = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
        s_tx_q = xQueueCreateStatic(DBWS_TX_SLOTS, sizeof(dbws_tx_item_t),
                                    (uint8_t *)s_tx_items, &s_tx_q_ctrl);
    }
    xQueueReset(s_tx_q);

    s_stop = false;
    s_connected = false;
    s_backoff_s = DBWS_BACKOFF_MIN_S;
    /* Clear every bit incl. a sticky BIT_STOP left over from a previous
     * stop that broke out of a wait without consuming it (s_stop bool is
     * the durable stop signal; the bit is only a wake-up). */
    xEventGroupClearBits(s_evt, DBWS_BIT_CONN | DBWS_BIT_DISC |
                                DBWS_BIT_RECONNECT | DBWS_BIT_STOP);

    if (xTaskCreatePinnedToCore(dbws_task, "dbws", DBWS_TASK_STACK, NULL,
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

const char *dbws_get_session_id(void)
{
    return proto_get_session_id();
}
