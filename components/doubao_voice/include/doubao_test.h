/* SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DOUBAO_TEST_OK = 0,       /* API key accepted, WSS connected */
    DOUBAO_TEST_FAIL,         /* Server rejected the connection */
    DOUBAO_TEST_TIMEOUT,      /* No response within timeout */
    DOUBAO_TEST_EMPTY,        /* API key is empty */
    DOUBAO_TEST_ERROR,        /* Internal error */
} doubao_test_result_t;

/**
 * Quick-test a Doubao API key by attempting a WSS connection.
 *
 * Blocks for up to ~8 seconds.  Creates a temporary WebSocket client
 * independent of the production dbws task.
 *
 * @param api_key   The API key to test (X-Api-Key header value).
 * @param msg       Output buffer for human-readable result message.
 * @param msg_len   Size of msg buffer.
 * @return Test result code.
 */
doubao_test_result_t doubao_test_api_key(const char *api_key,
                                          char *msg, size_t msg_len);

#ifdef __cplusplus
}
#endif
