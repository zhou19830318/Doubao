/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * doubao_binary_protocol — O2.0 binary framing protocol implementation.
 *
 * Outgoing frames: sent WITHOUT compression (NO_COMPRESSION) to avoid
 * needing deflate (not available via miniz on ESP-IDF due to
 * MINIZ_NO_ZLIB_APIS).
 *
 * Incoming frames: server may send gzip-compressed payloads. We decompress
 * using tinfl_decompress_mem_to_mem (available in ESP-ROM).
 */

#include "doubao_binary_protocol.h"

#include <string.h>
#include "esp_log.h"
#include "miniz.h"

static const char *TAG = "bin_proto";

/* ── Helpers ────────────────────────────────────────────────────────── */

static void write_u32_be(uint8_t *buf, uint32_t val)
{
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val);
}

static uint32_t read_u32_be(const uint8_t *buf)
{
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8)  |
           ((uint32_t)buf[3]);
}

/* Decompress gzip payload using tinfl (ROM API).
 * Strips gzip header, decompresses raw deflate stream. */
static int gzip_decompress(const uint8_t *in, size_t in_len,
                            uint8_t *out, size_t out_cap)
{
    /* Minimum gzip header is 10 bytes */
    if (in_len < 10) return -1;

    size_t offset = 10;

    /* FLG byte is in[3] */
    if (in[3] & 0x04) {  /* FEXTRA */
        if (offset + 2 > in_len) return -1;
        uint16_t xlen = (uint16_t)in[offset] | ((uint16_t)in[offset + 1] << 8);
        offset += 2 + xlen;
    }
    if (in[3] & 0x08) {  /* FNAME */
        while (offset < in_len && in[offset] != 0) offset++;
        offset++;
    }
    if (in[3] & 0x10) {  /* FCOMMENT */
        while (offset < in_len && in[offset] != 0) offset++;
        offset++;
    }
    if (in[3] & 0x02) {  /* FHCRC */
        offset += 2;
    }

    if (offset >= in_len) return -1;

    size_t comp_len = in_len - offset;
    size_t dec_len = tinfl_decompress_mem_to_mem(out, out_cap,
                                                  in + offset, comp_len,
                                                  TINFL_FLAG_PARSE_ZLIB_HEADER);
    return (dec_len != (size_t)-1) ? (int)dec_len : -1;
}

/* ── Frame builder ───────────────────────────────────────────────────── */

/* Build a binary frame. Outgoing frames use NO_COMPRESSION. The
 * serialization nibble is caller-supplied: JSON (0b0001) for text/event
 * frames, NONE (0b0000) for raw audio payloads per the API doc. */
static esp_err_t build_frame(
    uint8_t *out,
    size_t *out_len,
    size_t cap,
    uint8_t msg_type,
    uint8_t flags,
    uint8_t serialization,
    uint32_t event_id,
    const char *session_id,
    const uint8_t *raw_payload,
    size_t raw_payload_len
)
{
    if (out == NULL || out_len == NULL || cap < 8) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t off = 0;

    /* Header: 4 bytes */
    out[off++] = (BIN_PROTO_VERSION << 4) | BIN_PROTO_HEADER_SIZE;
    out[off++] = (msg_type << 4) | flags;
    out[off++] = (serialization << 4) | BIN_COMP_NONE; /* no compression */
    out[off++] = 0x00;

    /* Event ID (4 bytes, big-endian) */
    if (flags & BIN_FLAG_EVENT) {
        if (off + 4 > cap) return ESP_ERR_NO_MEM;
        write_u32_be(out + off, event_id);
        off += 4;
    }

    /* Session ID (if provided) */
    if (session_id != NULL && session_id[0] != '\0') {
        size_t sid_len = strlen(session_id);
        if (sid_len > 64) sid_len = 64;
        if (off + 4 + sid_len > cap) return ESP_ERR_NO_MEM;
        write_u32_be(out + off, sid_len);
        off += 4;
        memcpy(out + off, session_id, sid_len);
        off += sid_len;
    }

    /* Payload length + data (uncompressed) */
    if (raw_payload != NULL && raw_payload_len > 0) {
        if (off + 4 + raw_payload_len > cap) return ESP_ERR_NO_MEM;
        write_u32_be(out + off, raw_payload_len);
        off += 4;
        memcpy(out + off, raw_payload, raw_payload_len);
        off += raw_payload_len;
    } else {
        if (off + 4 > cap) return ESP_ERR_NO_MEM;
        write_u32_be(out + off, 0);
        off += 4;
    }

    *out_len = off;
    return ESP_OK;
}

/* Helper: build frame from JSON string */
static esp_err_t build_json_frame(
    uint8_t *out,
    size_t *out_len,
    size_t cap,
    uint8_t msg_type,
    uint8_t flags,
    uint32_t event_id,
    const char *session_id,
    const char *json
)
{
    const uint8_t *payload = (const uint8_t *)(json ? json : "{}");
    size_t payload_len = strlen((const char *)payload);
    return build_frame(out, out_len, cap, msg_type, flags, BIN_SERIAL_JSON,
                        event_id, session_id, payload, payload_len);
}

esp_err_t bin_proto_build_start_connection(
    uint8_t *out,
    size_t *out_len,
    size_t cap
)
{
    return build_json_frame(out, out_len, cap,
                             BIN_MSG_CLIENT_FULL,
                             BIN_FLAG_EVENT,
                             BIN_EVT_START_CONNECTION,
                             NULL, "{}");
}

esp_err_t bin_proto_build_start_session(
    uint8_t *out,
    size_t *out_len,
    size_t cap,
    const char *session_id,
    const char *config
)
{
    return build_json_frame(out, out_len, cap,
                             BIN_MSG_CLIENT_FULL,
                             BIN_FLAG_EVENT,
                             BIN_EVT_START_SESSION,
                             session_id,
                             config ? config : "{}");
}

esp_err_t bin_proto_build_chat_text_query(
    uint8_t *out,
    size_t *out_len,
    size_t cap,
    const char *session_id,
    const char *text
)
{
    char json[512];
    if (text == NULL) text = "";
    /* Simple JSON escape */
    char escaped[384];
    size_t ei = 0;
    for (const char *p = text; *p && ei < sizeof(escaped) - 2; p++) {
        if (*p == '"') { escaped[ei++] = '\\'; escaped[ei++] = '"'; }
        else if (*p == '\\') { escaped[ei++] = '\\'; escaped[ei++] = '\\'; }
        else if (*p == '\n') { escaped[ei++] = '\\'; escaped[ei++] = 'n'; }
        else if (*p == '\r') { escaped[ei++] = '\\'; escaped[ei++] = 'r'; }
        else { escaped[ei++] = *p; }
    }
    escaped[ei] = '\0';
    snprintf(json, sizeof(json), "{\"content\":\"%s\"}", escaped);

    return build_json_frame(out, out_len, cap,
                             BIN_MSG_CLIENT_FULL,
                             BIN_FLAG_EVENT,
                             BIN_EVT_CHAT_TEXT_QUERY,
                             session_id, json);
}

esp_err_t bin_proto_build_chat_tts_text(
    uint8_t *out,
    size_t *out_len,
    size_t cap,
    const char *session_id,
    bool start,
    bool end,
    const char *content
)
{
    char json[512];
    snprintf(json, sizeof(json),
             "{\"start\":%s,\"end\":%s,\"content\":\"%s\"}",
             start ? "true" : "false",
             end ? "true" : "false",
             content ? content : "");

    return build_json_frame(out, out_len, cap,
                             BIN_MSG_CLIENT_FULL,
                             BIN_FLAG_EVENT,
                             BIN_EVT_CHAT_TTS_TEXT,
                             session_id, json);
}

esp_err_t bin_proto_build_finish_session(
    uint8_t *out,
    size_t *out_len,
    size_t cap,
    const char *session_id
)
{
    return build_json_frame(out, out_len, cap,
                             BIN_MSG_CLIENT_FULL,
                             BIN_FLAG_EVENT,
                             BIN_EVT_FINISH_SESSION,
                             session_id, "{}");
}

esp_err_t bin_proto_build_finish_connection(
    uint8_t *out,
    size_t *out_len,
    size_t cap
)
{
    return build_json_frame(out, out_len, cap,
                             BIN_MSG_CLIENT_FULL,
                             BIN_FLAG_EVENT,
                             BIN_EVT_FINISH_CONNECTION,
                             NULL, "{}");
}

esp_err_t bin_proto_build_task_request_audio(
    uint8_t *out,
    size_t *out_len,
    size_t cap,
    const char *session_id,
    const uint8_t *audio_data,
    size_t audio_len
)
{
    /* Audio payload is raw PCM — serialization must be NONE (0b0000),
     * not JSON (the previous hardcoded JSON nibble was spec-invalid). */
    return build_frame(out, out_len, cap,
                        BIN_MSG_CLIENT_AUDIO,
                        BIN_FLAG_EVENT,
                        BIN_SERIAL_NONE,
                        BIN_EVT_TASK_REQUEST,
                        session_id,
                        audio_data, audio_len);
}

/* ── Frame parser ────────────────────────────────────────────────────── */

static uint8_t decomp_buf[32 * 1024]; /* decompression buffer */

esp_err_t bin_proto_parse(
    const uint8_t *data,
    size_t len,
    bin_proto_parsed_t *result
)
{
    if (data == NULL || len < 4 || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(result, 0, sizeof(*result));

    /* Parse header */
    uint8_t header_size = data[0] & 0x0F;
    result->message_type = data[1] >> 4;
    result->flags = data[1] & 0x0F;
    result->serialization = data[2] >> 4;
    result->compression = data[2] & 0x0F;

    size_t off = header_size * 4;
    if (off > len) return ESP_ERR_INVALID_SIZE;

    /* Parse event ID (if flag set) */
    if (result->flags & BIN_FLAG_EVENT) {
        if (off + 4 > len) return ESP_ERR_INVALID_SIZE;
        result->event_id = read_u32_be(data + off);
        off += 4;
    }

    /* Parse session ID (if present in server messages) */
    if (result->message_type == BIN_MSG_SERVER_FULL ||
        result->message_type == BIN_MSG_SERVER_ACK) {
        if (result->flags & BIN_FLAG_NEG_SEQ) {
            if (off + 4 > len) return ESP_ERR_INVALID_SIZE;
            off += 4;  /* Skip sequence */
        }
        if (off + 4 <= len) {
            uint32_t sid_len = read_u32_be(data + off);
            off += 4;
            if (sid_len > 0 && sid_len <= 64 && off + sid_len <= len) {
                memcpy(result->session_id, data + off, sid_len);
                if (sid_len < 64) result->session_id[sid_len] = '\0';
                off += sid_len;
            }
        }
    }

    /* Parse payload length */
    if (off + 4 > len) return ESP_ERR_INVALID_SIZE;
    uint32_t payload_len = read_u32_be(data + off);
    off += 4;

    /* Check if we have the complete payload */
    if (off + payload_len > len) return ESP_ERR_INVALID_SIZE;

    const uint8_t *payload = data + off;

    if (payload_len > 0) {
        if (result->compression == BIN_COMP_GZIP) {
            int dec_len = gzip_decompress(payload, payload_len,
                                           decomp_buf, sizeof(decomp_buf));
            if (dec_len < 0) {
                ESP_LOGE(TAG, "gzip decompress failed");
                return ESP_FAIL;
            }
            result->json_payload = (const char *)decomp_buf;
            result->payload_len = dec_len;
        } else if (result->serialization == BIN_SERIAL_JSON) {
            result->json_payload = (const char *)payload;
            result->payload_len = payload_len;
        } else {
            /* Raw binary data (audio) */
            result->json_payload = (const char *)payload;
            result->payload_len = payload_len;
        }
    }

    return ESP_OK;
}
