/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * doubao_test — Quick API key validation for web UI.
 *
 * Creates a temporary WSS client, connects with the given API key,
 * and returns success/failure within ~5 seconds.  Used by the
 * /api/doubao/test web endpoint so users can verify their key before
 * committing it to NVS.
 *
 * Must NOT interfere with the production dbws task — the test client
 * is completely independent (separate handle, separate event loop).
 */

#include "doubao_test.h"

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "doubao_test";

#define TEST_WSS_URI  "wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue"
#define TEST_TIMEOUT_MS  8000   /* 8s — covers TLS + WS handshake + server idle kick */

#define BIT_CONNECTED  (1 << 0)
#define BIT_DISCONNECTED (1 << 1)
#define BIT_STOP       (1 << 2)

static void test_ws_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *data)
{
    EventGroupHandle_t eg = (EventGroupHandle_t)arg;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "test: connected");
        xEventGroupSetBits(eg, BIT_CONNECTED);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "test: disconnected/closed");
        xEventGroupSetBits(eg, BIT_DISCONNECTED);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "test: error");
        xEventGroupSetBits(eg, BIT_DISCONNECTED);
        break;
    default:
        break;
    }
}

doubao_test_result_t doubao_test_api_key(const char *api_key,
                                          char *msg, size_t msg_len)
{
    if (!api_key || !api_key[0]) {
        snprintf(msg, msg_len, "API key is empty");
        return DOUBAO_TEST_EMPTY;
    }

    EventGroupHandle_t eg = xEventGroupCreate();
    if (!eg) {
        snprintf(msg, msg_len, "Internal error (no memory)");
        return DOUBAO_TEST_ERROR;
    }

    /* Build X-Api-Key header */
    char headers[96];
    snprintf(headers, sizeof(headers), "X-Api-Key: %s\r\n", api_key);

    /* This is a SECOND full TLS client alongside the production one, and
     * internal DRAM is the scarce resource on this board — check before
     * attempting to avoid crashing the httpd task. */
    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t largest_before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "test: internal free=%u largest=%u before second client",
             (unsigned)free_before, (unsigned)largest_before);
    /* The ws client needs ~8KB internal DRAM (6KB task stack + 2KB buffers).
     * If the largest block is < 10KB, skip to avoid httpd stack overflow
     * or ws task creation failure. */
    if (largest_before < 10240) {
        ESP_LOGW(TAG, "test: insufficient internal RAM (%u bytes) — skipping",
                 (unsigned)largest_before);
        vEventGroupDelete(eg);
        snprintf(msg, msg_len, "Insufficient internal RAM (free=%u, largest=%u). "
                 "Reboot and test immediately, before using other features.",
                 (unsigned)free_before, (unsigned)largest_before);
        return DOUBAO_TEST_FAIL;
    }

    esp_websocket_client_config_t cfg = {
        .uri = TEST_WSS_URI,
        .headers = headers,
        .cert_pem = NULL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .transport = WEBSOCKET_TRANSPORT_OVER_TCP,
        .disable_auto_reconnect = true,
        /* Minimal footprint: this client only waits for CONNECTED, it
         * never reassembles payloads (no fragmentation, no cJSON). */
        .buffer_size = 1024,
        .network_timeout_ms = 5000,
        .task_stack = 4096,  /* Minimal: only waits for CONNECTED, no payload decode */
        .ping_interval_sec = 1,
    };

    esp_websocket_client_handle_t client = esp_websocket_client_init(&cfg);
    if (!client) {
        vEventGroupDelete(eg);
        snprintf(msg, msg_len, "Internal error (ws init failed)");
        return DOUBAO_TEST_ERROR;
    }

    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY,
                                  test_ws_handler, eg);

    esp_err_t err = esp_websocket_client_start(client);
    if (err != ESP_OK) {
        /* start() collapses transport-creation failure and task-creation
         * failure into one ESP_FAIL — disambiguate with the heap state so
         * the message tells the user what to do instead of a bare code. */
        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        ESP_LOGE(TAG, "test: start failed: %s (internal free=%u largest=%u)",
                 esp_err_to_name(err), (unsigned)free_internal, (unsigned)largest);
        esp_websocket_client_destroy(client);
        vEventGroupDelete(eg);
        snprintf(msg, msg_len, "Connection failed: %s (internal RAM free=%u, "
                 "largest block=%u — reboot and test before use)",
                 esp_err_to_name(err), (unsigned)free_internal, (unsigned)largest);
        return DOUBAO_TEST_FAIL;
    }

    /* Wait for CONNECTED or DISCONNECTED with timeout */
    int wait_ms = TEST_TIMEOUT_MS;
    int step_ms = 200;
    uint32_t bits = 0;
    while (wait_ms > 0) {
        bits = xEventGroupWaitBits(eg, BIT_CONNECTED | BIT_DISCONNECTED | BIT_STOP,
                                   pdFALSE, pdFALSE, pdMS_TO_TICKS(step_ms));
        if (bits & (BIT_CONNECTED | BIT_DISCONNECTED)) {
            break;
        }
        wait_ms -= step_ms;
    }

    doubao_test_result_t result;

    if (bits & BIT_CONNECTED) {
        /* Connected — API key is accepted at the transport level.
         * We don't send session.create here (avoid racing the production
         * client).  A successful WSS + TLS handshake with the key means
         * the key format is valid and the server accepted the auth header. */
        ESP_LOGI(TAG, "test: SUCCESS — key accepted");
        snprintf(msg, msg_len, "Connected successfully — API key is valid");
        result = DOUBAO_TEST_OK;

        /* Disconnect cleanly */
        esp_websocket_client_close(client, pdMS_TO_TICKS(2000));
        vTaskDelay(pdMS_TO_TICKS(500));
    } else if (bits & BIT_DISCONNECTED) {
        /* Disconnected during handshake — likely auth failure */
        ESP_LOGW(TAG, "test: FAILED — disconnected during handshake");
        snprintf(msg, msg_len, "Connection rejected — check API key");
        result = DOUBAO_TEST_FAIL;
    } else {
        /* Timeout */
        ESP_LOGW(TAG, "test: TIMEOUT after %dms", TEST_TIMEOUT_MS);
        snprintf(msg, msg_len, "Connection timeout — check network");
        result = DOUBAO_TEST_TIMEOUT;
    }

    /* Always destroy the test client (never leave it running) */
    esp_websocket_client_destroy(client);
    vEventGroupDelete(eg);

    return result;
}
