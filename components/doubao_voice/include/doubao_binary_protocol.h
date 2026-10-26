/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * doubao_binary_protocol — O2.0 binary framing protocol for Volcengine
 * realtime dialogue endpoint.
 *
 * Protocol reference: 1.2.1.1豆包语音_端到端实时语音大模型API接入文档
 *
 * Frame format (big-endian):
 *   Header (4 bytes):
 *     Byte 0: (protocol_version << 4) | header_size
 *     Byte 1: (message_type << 4) | message_type_specific_flags
 *     Byte 2: (serialization_method << 4) | compression_type
 *     Byte 3: reserved (0x00)
 *   Payload:
 *     [optional 4 bytes] event ID
 *     [optional 4 bytes] session ID length
 *     [optional N bytes] session ID
 *     [4 bytes] payload length
 *     [N bytes] payload (gzip compressed JSON)
 */

#ifndef DOUBAO_BINARY_PROTOCOL_H
#define DOUBAO_BINARY_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Protocol constants ─────────────────────────────────────────────── */

#define BIN_PROTO_VERSION           0x01
#define BIN_PROTO_HEADER_SIZE       0x01

/* Message types */
#define BIN_MSG_CLIENT_FULL         0x01
#define BIN_MSG_CLIENT_AUDIO        0x02
#define BIN_MSG_SERVER_FULL         0x09
#define BIN_MSG_SERVER_ACK          0x0B
#define BIN_MSG_SERVER_ERROR        0x0F

/* Message flags */
#define BIN_FLAG_NO_SEQ             0x00
#define BIN_FLAG_POS_SEQ            0x01
#define BIN_FLAG_NEG_SEQ            0x02
#define BIN_FLAG_EVENT              0x04

/* Serialization */
#define BIN_SERIAL_NONE             0x00
#define BIN_SERIAL_JSON             0x01

/* Compression */
#define BIN_COMP_NONE               0x00
#define BIN_COMP_GZIP               0x01

/* Event IDs (client → server) */
#define BIN_EVT_START_CONNECTION    1
#define BIN_EVT_START_SESSION       100
#define BIN_EVT_FINISH_SESSION      102
#define BIN_EVT_FINISH_CONNECTION   2
#define BIN_EVT_CHAT_TEXT_QUERY     501
#define BIN_EVT_CHAT_TTS_TEXT       500
#define BIN_EVT_TASK_REQUEST        200

/* Event IDs (server → client). Numbering follows the 1.2.1.1 API doc
 * (豆包语音_端到端实时语音大模型API接入文档) and the official Python demo:
 *   50/51/52     ConnectionStarted / ConnectionFailed / ConnectionFinished
 *   150/152/153  SessionStarted / SessionFinished / SessionFailed
 *   154          UsageResponse (per-interaction usage — informational)
 *   350/351/352/359  TTSSentenceStart / TTSSentenceEnd / TTSResponse
 *                   (binary audio payload) / TTSEnded (final, status_code)
 *   450/451/459  ASRInfo (first char → barge-in) / ASRResponse
 *                   (results[].text) / ASREnded (user finished speaking)
 *   550/553/559  ChatResponse (model reply text) / ChatTextQueryConfirmed
 *                   (ack) / ChatEnded (reply text finished)
 */
#define BIN_EVT_CONN_STARTED        50
#define BIN_EVT_CONN_FAILED         51
#define BIN_EVT_CONN_FINISHED       52
#define BIN_EVT_SESSION_STARTED     150
#define BIN_EVT_SESSION_FINISHED    152
#define BIN_EVT_SESSION_FAILED      153
#define BIN_EVT_USAGE_RESPONSE      154
#define BIN_EVT_TTS_SENTENCE_START  350
#define BIN_EVT_TTS_SENTENCE_END    351
#define BIN_EVT_TTS_RESPONSE        352
#define BIN_EVT_TTS_ENDED           359
#define BIN_EVT_ASR_INFO            450
#define BIN_EVT_ASR_RESPONSE        451
#define BIN_EVT_ASR_ENDED           459
#define BIN_EVT_CHAT_RESPONSE       550
#define BIN_EVT_CHAT_QUERY_CONFIRMED 553
#define BIN_EVT_CHAT_ENDED          559
#define BIN_EVT_ERROR               1000

/* ── Binary frame builder ────────────────────────────────────────────── */

/**
 * @brief Build a binary frame for the O2.0 protocol.
 *
 * @param[out] out       Output buffer (must be large enough)
 * @param[out] out_len   Length of output frame
 * @param[in]  cap       Capacity of output buffer
 * @param[in]  msg_type  Message type (BIN_MSG_CLIENT_FULL, etc.)
 * @param[in]  flags     Message flags (BIN_FLAG_EVENT, etc.)
 * @param[in]  event_id  Event ID (0 if no event)
 * @param[in]  session_id Session ID string (NULL if no session)
 * @param[in]  json_payload JSON payload (NULL for empty payload)
 * @return ESP_OK on success
 */
esp_err_t bin_proto_build_frame(
    uint8_t *out,
    size_t *out_len,
    size_t cap,
    uint8_t msg_type,
    uint8_t flags,
    uint32_t event_id,
    const char *session_id,
    const char *json_payload
);

/**
 * @brief Build a StartConnection frame (event 1, empty payload).
 */
esp_err_t bin_proto_build_start_connection(
    uint8_t *out,
    size_t *out_len,
    size_t cap
);

/**
 * @brief Build a StartSession frame (event 100).
 *
 * @param[out] out       Output buffer
 * @param[out] out_len   Length of output
 * @param[in]  cap       Capacity
 * @param[in]  session_id Session ID
 * @param[in]  config    JSON configuration payload
 */
esp_err_t bin_proto_build_start_session(
    uint8_t *out,
    size_t *out_len,
    size_t cap,
    const char *session_id,
    const char *config
);

/**
 * @brief Build a ChatTextQuery frame (event 501).
 */
esp_err_t bin_proto_build_chat_text_query(
    uint8_t *out,
    size_t *out_len,
    size_t cap,
    const char *session_id,
    const char *text
);

/**
 * @brief Build a ChatTTSText frame (event 500).
 */
esp_err_t bin_proto_build_chat_tts_text(
    uint8_t *out,
    size_t *out_len,
    size_t cap,
    const char *session_id,
    bool start,
    bool end,
    const char *content
);

/**
 * @brief Build a FinishSession frame (event 102).
 */
esp_err_t bin_proto_build_finish_session(
    uint8_t *out,
    size_t *out_len,
    size_t cap,
    const char *session_id
);

/**
 * @brief Build a FinishConnection frame (event 2).
 */
esp_err_t bin_proto_build_finish_connection(
    uint8_t *out,
    size_t *out_len,
    size_t cap
);

/**
 * @brief Build a TaskRequest frame (event 200, audio only).
 */
esp_err_t bin_proto_build_task_request_audio(
    uint8_t *out,
    size_t *out_len,
    size_t cap,
    const char *session_id,
    const uint8_t *audio_data,
    size_t audio_len
);

/* ── Binary frame parser ─────────────────────────────────────────────── */

typedef struct {
    uint8_t  message_type;
    uint8_t  flags;
    uint8_t  serialization;
    uint8_t  compression;
    uint32_t event_id;
    char     session_id[64];
    const char *json_payload;   /* points into input buffer (after decompress) */
    size_t   payload_len;
    /* Error-specific fields */
    uint32_t error_code;
} bin_proto_parsed_t;

/**
 * @brief Parse a binary frame from the server.
 *
 * @param[in]  data   Raw frame data
 * @param[in]  len    Length of data
 * @param[out] result Parsed result (session_id buffer owned by caller)
 * @return ESP_OK on success, ESP_ERR_INVALID_DATA on malformed frame
 */
esp_err_t bin_proto_parse(
    const uint8_t *data,
    size_t len,
    bin_proto_parsed_t *result
);

#ifdef __cplusplus
}
#endif

#endif /* DOUBAO_BINARY_PROTOCOL_H */
