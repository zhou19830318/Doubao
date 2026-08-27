/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * doubao_protocol — Doubao (Volcengine) realtime duplex dialogue protocol:
 * upstream JSON construction + downstream streaming parse with WS fragment
 * reassembly. Task 5 (M2).
 *
 * Protocol reference: PRD/Project_Resources/豆包语音_端到端实时语音-全双工版本
 * (输入输出示例) + design doc §4.3/§4.4.
 *
 * Upstream: static-buffer JSON serialization (append helpers +
 * mbedtls_base64_encode), zero dynamic allocation.
 *
 * Downstream: fragments accumulate in a static 64KB PSRAM buffer; a
 * quote-aware brace scanner finds complete JSON objects (handles escaped
 * \" inside strings and braces inside string values), each is cJSON-parsed
 * and dispatched by `type` to a doubao_event_cb_t. Audio deltas are
 * Base64-decoded into a static 64KB PSRAM PCM buffer.
 */

#include "doubao_protocol.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"

#include "cJSON.h"
#include "mbedtls/base64.h"

static const char *TAG = "doubao_proto";

/* ── Static state ─────────────────────────────────────────────────────── */
/* All buffers static (heap_caps_malloc'd once, lazily) — no per-call
 * allocation. PSRAM for the big buffers (CLAUDE.md 内存铁律 4). */

#define PROTO_FRAG_CAP (64 * 1024)   /* WS fragment accumulation */
#define PROTO_PCM_CAP  (64 * 1024)   /* decoded audio delta PCM (24k/16bit) */
#define PROTO_SESSION_ID_CAP 64
#define PROTO_ERR_MSG_CAP 128

static char   *s_frag_buf;           /* PSRAM */
static size_t  s_frag_len;
static char   *s_pcm_buf;            /* PSRAM */
static doubao_audio_chunk_t s_audio_chunk;
static int     s_status_code;
static char    s_session_id[PROTO_SESSION_ID_CAP];
static char    s_err_msg[PROTO_ERR_MSG_CAP];
static uint32_t s_evt_seq;           /* upstream event_id counter */
static uint32_t s_audio_delta_cnt;   /* total audio delta events received */
static uint32_t s_total_events;       /* total events dispatched */

/* cJSON global hooks → PSRAM. Every downlink JSON parse allocates hundreds
 * of small cJSON nodes (<256B each → internal heap under ALWAYSINTERNAL),
 * and at ~2× audio burst rate that transient churn starved the SPI DMA
 * allocator mid-conversation ("setup_dma_priv_buffer: Failed to allocate
 * priv TX buffer" — display/SD stalls during playback). PSRAM is
 * plentiful; cJSON is fine with it (parse fails cleanly on OOM). */
static bool s_cjson_hooks_set = false;

static void *cjson_psram_malloc(size_t sz)
{
    return heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void cjson_psram_free(void *p)
{
    heap_caps_free(p);
}

static void cjson_hooks_ensure(void)
{
    if (s_cjson_hooks_set) return;
    cJSON_Hooks hooks = { .malloc_fn = cjson_psram_malloc,
                          .free_fn = cjson_psram_free };
    cJSON_InitHooks(&hooks);
    s_cjson_hooks_set = true;
}

static esp_err_t ensure_buffers(void)
{
    cjson_hooks_ensure();
    if (s_frag_buf == NULL) {
        s_frag_buf = heap_caps_malloc(PROTO_FRAG_CAP, MALLOC_CAP_SPIRAM);
        if (s_frag_buf == NULL) {
            ESP_LOGE(TAG, "no PSRAM for %d-byte fragment buffer", PROTO_FRAG_CAP);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "fragment buffer %dKB PSRAM ready", PROTO_FRAG_CAP / 1024);
    }
    if (s_pcm_buf == NULL) {
        s_pcm_buf = heap_caps_malloc(PROTO_PCM_CAP, MALLOC_CAP_SPIRAM);
        if (s_pcm_buf == NULL) {
            ESP_LOGE(TAG, "no PSRAM for %d-byte PCM buffer", PROTO_PCM_CAP);
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

void proto_reset(void)
{
    s_frag_len = 0;
    /* buffers stay allocated (static hold); session id deliberately kept:
     * it belongs to the dialogue and must survive a reconnect reset. */
}

const char *proto_get_session_id(void)
{
    return (s_session_id[0] != '\0') ? s_session_id : NULL;
}

/* ── Upstream builders ────────────────────────────────────────────────── */

/* Append helpers: always keep the buffer NUL-terminated, report failure on
 * capacity overflow. */

static bool append_raw(char *buf, size_t cap, size_t *off, const char *s)
{
    size_t n = strlen(s);
    if (*off + n + 1 > cap) {
        return false;
    }
    memcpy(buf + *off, s, n);
    *off += n;
    buf[*off] = '\0';
    return true;
}

static bool append_fmt(char *buf, size_t cap, size_t *off, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (need < 0 || (size_t)need >= cap - *off) {
        return false;
    }
    *off += (size_t)need;
    return true;
}

static bool append_json_escape(char *buf, size_t cap, size_t *off, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        char esc[16];  /* \uXXXX = 6, surrogate pair \uXXXX\uXXXX = 12 */
        size_t n;
        unsigned char c = *p;
        if (c == '"')  { esc[0] = '\\'; esc[1] = '"';  n = 2; }
        else if (c == '\\') { esc[0] = '\\'; esc[1] = '\\'; n = 2; }
        else if (c == '\n') { esc[0] = '\\'; esc[1] = 'n';  n = 2; }
        else if (c == '\r') { esc[0] = '\\'; esc[1] = 'r';  n = 2; }
        else if (c == '\t') { esc[0] = '\\'; esc[1] = 't';  n = 2; }
        else if (c < 0x20) {
            /* Control character → \u00XX */
            esc[0] = '\\'; esc[1] = 'u';
            n = 2 + snprintf(esc + 2, sizeof(esc) - 2, "%04x", (unsigned)c);
        }
        else if (c >= 0x80) {
            /* Non-ASCII byte → decode UTF-8 and emit \uXXXX.
             * This ensures the server's JSON parser handles it
             * regardless of its UTF-8 support. */
            unsigned int cp = 0;
            int extra = 0;
            if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; extra = 1; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
            else { esc[0] = '?'; n = 1; goto write; }
            for (int i = 0; i < extra; i++) {
                p++;
                if ((*p & 0xC0) != 0x80) { esc[0] = '?'; n = 1; goto write; }
                cp = (cp << 6) | (*p & 0x3F);
            }
            /* Supplementary plane (>U+FFFF) needs surrogate pair */
            /* Cannot use \u in C string literal (triggers UCN parsing),
             * so build the \uXXXX escape manually. */
            esc[0] = '\\'; esc[1] = 'u';  /* \u prefix */
            if (cp > 0xFFFF) {
                unsigned int hi = 0xD800 + ((cp - 0x10000) >> 10);
                unsigned int lo = 0xDC00 + ((cp - 0x10000) & 0x3FF);
                n = 2 + snprintf(esc + 2, sizeof(esc) - 2, "%04x", hi);
                esc[n] = '\\'; esc[n+1] = 'u'; n += 2;
                n += snprintf(esc + n, sizeof(esc) - n, "%04x", lo);
            } else {
                n = 2 + snprintf(esc + 2, sizeof(esc) - 2, "%04x", cp);
            }
        }
        else {
            esc[0] = (char)c; n = 1;
        }
write:
        if (*off + n + 1 > cap) {
            return false;
        }
        memcpy(buf + *off, esc, n);
        *off += n;
        buf[*off] = '\0';
    }
    return true;
}

/* RFC 4122 version-4 UUID from hardware RNG, formatted as 36 chars.
 * One per session.create — mirrors the official Python demo, whose
 * client-generated id is echoed back by the server as the dialog id. */
static void gen_client_uuid(char out[37])
{
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40);  /* version 4 */
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);  /* variant 10xx */
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

esp_err_t proto_build_session_create(char *buf, size_t cap, const doubao_cfg_t *cfg)
{
    if (buf == NULL || cfg == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Client-generated session id — the official Python demo sends a
     * fresh UUID on every session.create and the server echoes it back
     * as the dialog id in session.created. Omitting it worked for the
     * handshake but not, on our device, for the dialogue. */
    char client_id[37];
    gen_client_uuid(client_id);

    size_t off = 0;
    if (!append_fmt(buf, cap, &off,
                    "{\"type\":\"session.create\",\"event_id\":\"event_%u\","
                    "\"session\":{\"type\":\"realtime\",\"id\":\"%s\","
                    "\"model\":\"1.2.6.1\","
                    "\"instructions\":\"", (unsigned)s_evt_seq++, client_id) ||
        !append_json_escape(buf, cap, &off, cfg->instructions ? cfg->instructions : "") ||
        !append_raw(buf, cap, &off,
                    "\",\"audio\":{\"input\":{\"format\":{\"type\":\"pcm\",\"rate\":16000}},"
                    "\"output\":{\"format\":{\"type\":\"pcm_s16le\",\"rate\":24000},\"voice\":\"") ||
        !append_json_escape(buf, cap, &off, cfg->voice ? cfg->voice : "") ||
        /* closes audio.output then audio — session stays open for the
         * optional id field below (a "session.id" outside the session
         * object was silently sent to the server for a long time) */
        !append_fmt(buf, cap, &off, "\",\"speed\":%d,\"loudness\":%d}}",
                    (int)cfg->speed, (int)cfg->loudness)) {
        return ESP_ERR_NO_MEM;
    }
    if (!append_raw(buf, cap, &off, "}")) {
        return ESP_ERR_NO_MEM;
    }
    /* The official example sends the extension object (all-empty asr/tts/
     * dialog) at the TOP level, as a sibling of "session". Our payload
     * omitted it entirely and the server accepted everything while never
     * producing a single ASR/response event — the engine was never engaged.
     * See the 输入输出示例 doc, example #1.
     * dialog.extra mirrors the demo's config (realtime_client.py):
     * enable_loudness_norm keeps TTS output level steady so loud
     * utterances don't clip the DAC (audible crackle).
     * enable_music: true → 歌唱功能（服务端 TTS 切换为歌唱模式）。
     * tools: web_search → 联网搜索（服务端自动调用搜索引擎）。 */
    if (!append_fmt(buf, cap, &off,
                    ",\"extension\":{\"asr\":{},\"tts\":{\"audio_config\":{"
                    "\"format\":\"pcm_s16le\",\"sample_rate\":24000,\"channel\":1,"
                    "\"bits\":16}},\"dialog\":{\"extra\":{\"enable_loudness_norm\":true,"
                    "\"enable_music\":%s}}}}",
                    cfg->enable_music ? "true" : "false")) {
        return ESP_ERR_NO_MEM;
    }
    /* 联网搜索：添加 tool_search 工具，服务端在需要时自动调用搜索引擎 */
    if (cfg->enable_search) {
        if (!append_raw(buf, cap, &off,
                        ",\"tools\":[{\"type\":\"function\",\"name\":\"tool_search\","
                        "\"description\":\"Search the internet for real-time information.\","
                        "\"parameters\":{\"type\":\"object\",\"properties\":{"
                        "\"query\":{\"type\":\"string\",\"description\":\"Search query.\"}},"
                        "\"required\":[\"query\"]}}]")) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t proto_build_audio_append(char *buf, size_t cap, const int16_t *pcm16,
                                   size_t samples, uint32_t event_id)
{
    if (buf == NULL || pcm16 == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t off = 0;
    if (!append_fmt(buf, cap, &off,
                    "{\"event_id\":\"event_%u\",\"type\":\"input_audio_buffer.append\","
                    "\"audio\":\"", (unsigned)event_id)) {
        return ESP_ERR_NO_MEM;
    }
    /* Base64 straight into the remaining space (no temp buffer):
     * reserve 3 bytes for closing "\"}" + NUL */
    if (cap - off < 3) {
        return ESP_ERR_NO_MEM;
    }
    size_t enc_cap = cap - off - 3;
    size_t olen = 0;
    int rc = mbedtls_base64_encode((unsigned char *)buf + off, enc_cap, &olen,
                                   (const unsigned char *)pcm16, samples * sizeof(int16_t));
    if (rc != 0) {
        ESP_LOGW(TAG, "base64 encode failed rc=%d (need %u bytes)", rc, (unsigned)olen);
        return ESP_ERR_NO_MEM;
    }
    off += olen;
    if (!append_raw(buf, cap, &off, "\"}")) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t proto_build_commit(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t off = 0;
    return append_fmt(buf, cap, &off,
                      "{\"event_id\":\"event_%u\",\"type\":\"input_audio_buffer.commit\"}",
                      (unsigned)s_evt_seq++)
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

esp_err_t proto_build_cancel(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t off = 0;
    return append_raw(buf, cap, &off, "{\"type\":\"response.cancel\"}") ? ESP_OK
                                                                        : ESP_ERR_NO_MEM;
}

esp_err_t proto_build_close(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t off = 0;
    return append_fmt(buf, cap, &off,
                      "{\"event_id\":\"event_%u\",\"type\":\"session.close\"}",
                      (unsigned)s_evt_seq++)
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

esp_err_t proto_build_text_push(char *buf, size_t cap, const char *text)
{
    if (buf == NULL || text == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t off = 0;
    if (!append_fmt(buf, cap, &off,
                    "{\"event_id\":\"event_%u\",\"type\":\"speech_text_buffer."
                    "replacement.append\",\"text\":\"", (unsigned)s_evt_seq++) ||
        !append_json_escape(buf, cap, &off, text) ||
        !append_raw(buf, cap, &off, "\"}")) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t proto_build_text_commit(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t off = 0;
    return append_fmt(buf, cap, &off,
                      "{\"event_id\":\"event_%u\",\"type\":\"speech_text_buffer."
                      "replacement.commit\"}",
                      (unsigned)s_evt_seq++)
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

/* ── Downstream stream parser ─────────────────────────────────────────── */

/* string field helper: value or "" if missing/non-string */
static const char *json_str_field(cJSON *obj, const char *name)
{
    cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, name);
    return (it != NULL && cJSON_IsString(it)) ? it->valuestring : "";
}

/* Locate the first complete JSON object in buf[0..len).
 * Quote-aware: string contents (incl. escaped \" and braces inside strings)
 * never affect brace depth. Returns index of '{', sets *obj_len to the
 * object length including the closing '}'; -1 if no complete object. */
static int find_json_object(const char *buf, size_t len, size_t *obj_len)
{
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    int start = -1;

    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        switch (c) {
        case '"':
            in_str = true;
            break;
        case '{':
            if (depth == 0) {
                start = (int)i;
            }
            depth++;
            break;
        case '}':
            if (depth > 0) {
                depth--;
                if (depth == 0) {
                    *obj_len = i - (size_t)start + 1;
                    return start;
                }
            }
            break;
        default:
            break;
        }
    }
    return -1;
}

/* Parse one complete JSON object and dispatch to cb. Returns false if the
 * object was malformed (caller must proto_reset() per design). */
static bool handle_json(const char *json, doubao_event_cb_t cb)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGW(TAG, "cJSON parse failed, dropping partial state");
        return false;
    }

    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *type = (type_item != NULL && cJSON_IsString(type_item)) ? type_item->valuestring : "";

    /* Downlink boundary evidence. audio.delta arrives at ~40 events/s so it
     * would drown the log at INFO; log it at DEBUG with a counter. Everything
     * else is rare enough to print at INFO. */
    s_total_events++;
    if (strcmp(type, "response.output_audio.delta") == 0) {
        s_audio_delta_cnt++;
        if ((s_audio_delta_cnt % 50) == 1) {
            ESP_LOGD(TAG, "<< audio.delta #%lu",
                     (unsigned long)s_audio_delta_cnt);
        }
    } else {
        ESP_LOGD(TAG, "<< %s (event#%lu)", type, (unsigned long)s_total_events);
        ESP_LOGD(TAG, "raw: %.768s", json);
    }

    if (strcmp(type, "session.created") == 0) {
        cJSON *sess = cJSON_GetObjectItemCaseSensitive(root, "session");
        cJSON *id_it = sess ? cJSON_GetObjectItemCaseSensitive(sess, "id") : NULL;
        if (id_it != NULL && cJSON_IsString(id_it) && id_it->valuestring[0] != '\0') {
            snprintf(s_session_id, sizeof(s_session_id), "%s", id_it->valuestring);
            ESP_LOGI(TAG, "session created: id=%.40s", s_session_id);
        }
        cb(DOUBAO_EVT_SESSION_CREATED, NULL, 0);
    } else if (strcmp(type, "session.closed") == 0) {
        /* Server closed the session (e.g. after our session.close, design
         * doc §4.4). The stored session.id must NOT be resumed — clear it
         * so the next connect starts a fresh session. No DOUBAO_EVT_*
         * fits this (locked enum); the ws layer observes the cleared id
         * via proto_get_session_id()==NULL. */
        s_session_id[0] = '\0';
        ESP_LOGI(TAG, "session closed — session id cleared");
        cb(DOUBAO_EVT_SESSION_CLOSED, NULL, 0);
    } else if (strcmp(type, "input_audio_buffer.committed") == 0) {
        /* ACK of our commit — pure acknowledgement, no action needed.
         * Handled explicitly so it does not hit the unhandled-event WARN. */
        ESP_LOGD(TAG, "commit acked");
    } else if (strcmp(type, "conversation.item.input_audio_transcription.started") == 0) {
        /* server confirms new user utterance → interrupt local playback */
        cb(DOUBAO_EVT_INTERRUPTED, NULL, 0);
    } else if (strcmp(type, "conversation.item.input_audio_transcription.delta") == 0) {
        const char *delta = json_str_field(root, "delta");
        cb(DOUBAO_EVT_TRANSCRIPT_DELTA, delta, strlen(delta));
    } else if (strcmp(type, "conversation.item.input_audio_transcription.completed") == 0) {
        const char *transcript = json_str_field(root, "transcript");
        cb(DOUBAO_EVT_TRANSCRIPT_DONE, transcript, strlen(transcript));
    } else if (strcmp(type, "conversation.item.input_audio_transcription.failed") == 0) {
        /* ASR failed — same error shape as `error` (design doc §4.4) */
        cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
        snprintf(s_err_msg, sizeof(s_err_msg), "%s",
                 err ? json_str_field(err, "message") : "");
        cb(DOUBAO_EVT_ERROR, s_err_msg, strlen(s_err_msg));
    } else if (strcmp(type, "response.output_text.delta") == 0) {
        const char *delta = json_str_field(root, "delta");
        cb(DOUBAO_EVT_OUTPUT_TEXT_DELTA, delta, strlen(delta));
    } else if (strcmp(type, "response.output_text.done") == 0) {
        const char *text = json_str_field(root, "text");
        cb(DOUBAO_EVT_OUTPUT_TEXT_DONE, text, strlen(text));
    } else if (strcmp(type, "response.output_audio.started") == 0) {
        const char *tts_type = json_str_field(root, "tts_type");
        cb(DOUBAO_EVT_AUDIO_STARTED, tts_type, strlen(tts_type));
    } else if (strcmp(type, "response.output_audio.delta") == 0) {
        const char *delta = json_str_field(root, "delta");
        size_t b64len = strlen(delta);
        size_t olen = 0;
        int rc = mbedtls_base64_decode((unsigned char *)s_pcm_buf, PROTO_PCM_CAP, &olen,
                                       (const unsigned char *)delta, b64len);
        if (rc != 0 || olen == 0) {
            ESP_LOGW(TAG, "audio base64 decode failed rc=%d len=%u", rc, (unsigned)olen);
        } else {
            s_audio_chunk.pcm24   = (const int16_t *)s_pcm_buf;
            s_audio_chunk.samples = olen / 2;   /* 16-bit PCM */
            cb(DOUBAO_EVT_AUDIO_DELTA, &s_audio_chunk, sizeof(s_audio_chunk));
        }
    } else if (strcmp(type, "response.output_audio.done") == 0) {
        /* status_code may arrive as JSON number or string ("20000002") */
        cJSON *sc = cJSON_GetObjectItemCaseSensitive(root, "status_code");
        s_status_code = 0;
        if (sc != NULL) {
            if (cJSON_IsNumber(sc)) {
                s_status_code = sc->valueint;
            } else if (cJSON_IsString(sc)) {
                s_status_code = atoi(sc->valuestring);
            }
        }
        cb(DOUBAO_EVT_AUDIO_DONE, &s_status_code, sizeof(s_status_code));
    } else if (strcmp(type, "response.done") == 0) {
        cb(DOUBAO_EVT_RESPONSE_DONE, NULL, 0);
    } else if (strcmp(type, "response.canceled") == 0) {
        cb(DOUBAO_EVT_INTERRUPTED, NULL, 0);
    } else if (strcmp(type, "error") == 0) {
        cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
        const char *msg = err ? json_str_field(err, "message") : "";
        if (msg[0] != '\0') {
            snprintf(s_err_msg, sizeof(s_err_msg), "%s", msg);
        } else {   /* no message: fall back to code/type so it is never empty */
            const char *code  = err ? json_str_field(err, "code") : "";
            const char *etype = err ? json_str_field(err, "type") : "";
            if (code[0] != '\0' && etype[0] != '\0') {
                snprintf(s_err_msg, sizeof(s_err_msg), "%s/%s", code, etype);
            } else if (code[0] != '\0') {
                snprintf(s_err_msg, sizeof(s_err_msg), "%s", code);
            } else if (etype[0] != '\0') {
                snprintf(s_err_msg, sizeof(s_err_msg), "%s", etype);
            } else {
                snprintf(s_err_msg, sizeof(s_err_msg), "unknown error");
            }
        }
        cb(DOUBAO_EVT_ERROR, s_err_msg, strlen(s_err_msg));
    } else {
        /* WARN, not DEBUG: a downlink event we do not recognise is
         * indistinguishable from silence at INFO level, which makes "the
         * server said nothing" impossible to tell from "the server said
         * something we ignored". */
        ESP_LOGW(TAG, "unhandled downlink event type: %s", type);
    }

    cJSON_Delete(root);
    return true;
}

void proto_get_stats(uint32_t *audio_deltas, uint32_t *total_events)
{
    if (audio_deltas) *audio_deltas = s_audio_delta_cnt;
    if (total_events) *total_events = s_total_events;
}

esp_err_t proto_feed(ws_frag_t frag, doubao_event_cb_t cb)
{
    if (frag.data == NULL || cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_buffers();
    if (err != ESP_OK) {
        return err;
    }

    if (frag.len >= PROTO_FRAG_CAP - s_frag_len) {
        ESP_LOGW(TAG, "fragment overflow (%u+%u > %d), resetting",
                 (unsigned)s_frag_len, (unsigned)frag.len, PROTO_FRAG_CAP);
        proto_reset();
        return ESP_ERR_NO_MEM;
    }
    memcpy(s_frag_buf + s_frag_len, frag.data, frag.len);
    s_frag_len += frag.len;

    size_t obj_len = 0;
    int start = find_json_object(s_frag_buf, s_frag_len, &obj_len);
    while (start >= 0) {
        /* NUL-terminate the object in place (parse window), then restore */
        size_t obj_end = (size_t)start + obj_len;
        char saved = s_frag_buf[obj_end];
        s_frag_buf[obj_end] = '\0';
        bool ok = handle_json(s_frag_buf + start, cb);
        s_frag_buf[obj_end] = saved;
        if (!ok) {
            proto_reset();   /* malformed JSON: drop partial state, recover */
            return ESP_OK;
        }
        /* consume the object; keep any trailing partial bytes at front */
        size_t rest = s_frag_len - obj_end;
        if (rest > 0) {
            memmove(s_frag_buf, s_frag_buf + obj_end, rest);
        }
        s_frag_len = rest;
        start = find_json_object(s_frag_buf, s_frag_len, &obj_len);
    }
    return ESP_OK;
}
