/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * proto_test — temporary on-target self-test for the doubao protocol codec
 * (Task 5 Step 4). Command: `proto test`  → prints "proto test: PASS/FAIL".
 * M2 结束可保留至 M5 再删。
 *
 * The 5 canonical downlink samples are taken verbatim from
 * PRD/Project_Resources/豆包语音_端到端实时语音-全双工版本输入输出示例_1786094957.md
 * (transcript.delta / output_text.delta / audio.delta / audio.done
 * status_code=20000002 / error). Extra edge cases on top:
 *   - fragment reassembly (one event across 3 WS fragments)
 *   - escaped \" inside a JSON string value (quote-aware scanner)
 *   - session.created → session id extraction
 *   - multiple events in a single fragment
 *   - upstream builders round-trip check
 */

#include "proto_test.h"

#include <stdio.h>
#include <string.h>

#include "esp_console.h"

#include "cJSON.h"
#include "mbedtls/base64.h"

#include "doubao_protocol.h"
#include "doubao_voice.h"

/* ── Test callback: copies data synchronously (valid only during cb) ──── */

typedef struct {
    bool got;
    doubao_event_type_t type;
    char str[128];
    int status_code;
    int16_t pcm[64];   /* AUDIO_DELTA samples snapshot */
    size_t pcm_n;
} test_evt_t;

static test_evt_t s_evt;
static int s_cb_count;   /* total callbacks fired (multi-event fragments) */

static void test_cb(doubao_event_type_t type, const void *data, size_t len)
{
    s_evt.got = true;
    s_evt.type = type;
    s_cb_count++;
    s_evt.str[0] = '\0';
    s_evt.status_code = 0;
    s_evt.pcm_n = 0;

    switch (type) {
    case DOUBAO_EVT_AUDIO_DELTA: {
        const doubao_audio_chunk_t *chunk = (const doubao_audio_chunk_t *)data;
        if (chunk != NULL && chunk->samples <= sizeof(s_evt.pcm) / sizeof(s_evt.pcm[0])) {
            memcpy(s_evt.pcm, chunk->pcm24, chunk->samples * sizeof(int16_t));
            s_evt.pcm_n = chunk->samples;
        }
        break;
    }
    case DOUBAO_EVT_AUDIO_DONE:
        if (data != NULL) {
            s_evt.status_code = *(const int *)data;
        }
        break;
    case DOUBAO_EVT_TRANSCRIPT_DELTA:
    case DOUBAO_EVT_TRANSCRIPT_DONE:
    case DOUBAO_EVT_OUTPUT_TEXT_DELTA:
    case DOUBAO_EVT_OUTPUT_TEXT_DONE:
    case DOUBAO_EVT_AUDIO_STARTED:
    case DOUBAO_EVT_ERROR:
        if (data != NULL) {
            snprintf(s_evt.str, sizeof(s_evt.str), "%s", (const char *)data);
        }
        break;
    default:
        break;
    }
    (void)len;
}

/* ── Check helpers ────────────────────────────────────────────────────── */

static int s_fails;

#define CHECK(cond, msg)                                                         \
    do {                                                                         \
        if (!(cond)) {                                                           \
            printf("      FAIL: %s (line %d)\n", msg, __LINE__);                 \
            s_fails++;                                                           \
        }                                                                        \
    } while (0)

/* Feed one complete JSON event, assert dispatched type + data. */
static void case_feed(const char *name, const char *json, doubao_event_type_t exp_type,
                      const char *exp_str, int exp_code, const int16_t *exp_pcm,
                      size_t exp_pcm_n)
{
    int fails_before = s_fails;
    memset(&s_evt, 0, sizeof(s_evt));
    s_cb_count = 0;

    ws_frag_t frag = { .data = (char *)json, .len = strlen(json) };
    esp_err_t rc = proto_feed(frag, test_cb);

    CHECK(rc == ESP_OK, "proto_feed return");
    CHECK(s_evt.got, "callback fired");
    CHECK(s_evt.type == exp_type, "event type");
    if (exp_str != NULL) {
        CHECK(strcmp(s_evt.str, exp_str) == 0, "string data");
    }
    if (exp_type == DOUBAO_EVT_AUDIO_DONE) {
        CHECK(s_evt.status_code == exp_code, "status_code");
    }
    if (exp_pcm != NULL && exp_pcm_n > 0) {
        CHECK(s_evt.pcm_n == exp_pcm_n, "pcm sample count");
        CHECK(memcmp(s_evt.pcm, exp_pcm, exp_pcm_n * sizeof(int16_t)) == 0, "pcm data");
    }
    printf("  [%s] %s\n", s_fails == fails_before ? "ok " : "FAIL", name);
}

/* ── Canonical samples (verbatim from the Doubao I/O example doc) ──────── */

/* 4 int16 samples {100, -100, 200, -200} → Base64 "ZACc/8gAOP8=" */
static const int16_t s_test_pcm[4] = { 100, -100, 200, -200 };

static const char *s_transcript_delta =
    "{\"type\":\"conversation.item.input_audio_transcription.delta\","
    "\"event_id\":\"event_CCXGRxsAimPAs8kS2Wc7Z\","
    "\"item_id\":\"item_CCXGQ4e1ht4cOraEYcuR2\","
    "\"content_index\":0,"
    "\"delta\":\"Hey\"}";

static const char *s_output_text_delta =
    "{\"event_id\":\"event_4142\","
    "\"type\":\"response.output_text.delta\","
    "\"question_id\":\"question_001\","
    "\"response_id\":\"resp_001\","
    "\"delta\":\"Sure, I can h\"}";

static const char *s_audio_delta =
    "{\"event_id\":\"event_4950\","
    "\"type\":\"response.output_audio.delta\","
    "\"question_id\":\"question_001\","
    "\"response_id\":\"resp_001\","
    "\"delta\":\"ZACc/8gAOP8=\"}";

static const char *s_audio_done =
    "{\"event_id\":\"event_5152\","
    "\"type\":\"response.output_audio.done\","
    "\"question_id\":\"question_001\","
    "\"response_id\":\"resp_001\","
    "\"status_code\":\"20000002\"}";

static const char *s_error =
    "{\"type\":\"error\","
    "\"error\":{\"type\":\"invalid_request_error\",\"code\":\"invalid_api_key\","
    "\"message\":\"Invalid API key provided\"}}";

/* ── Edge cases ───────────────────────────────────────────────────────── */

static void case_fragmented(void)
{
    int fails_before = s_fails;
    memset(&s_evt, 0, sizeof(s_evt));
    const char *p1 = "{\"event_id\":\"event_4142\",\"type\":\"resp";
    const char *p2 = "onse.output_text.delta\",\"question_id\":\"question_001\",\"response_id\":\"resp_001\",\"delta\":\"Su";
    const char *p3 = "re, I can h\"}";

    CHECK(proto_feed((ws_frag_t){ .data = (char *)p1, .len = strlen(p1) }, test_cb) == ESP_OK, "frag1");
    CHECK(!s_evt.got, "no event before complete object");
    CHECK(proto_feed((ws_frag_t){ .data = (char *)p2, .len = strlen(p2) }, test_cb) == ESP_OK, "frag2");
    CHECK(!s_evt.got, "still no event mid-object");
    CHECK(proto_feed((ws_frag_t){ .data = (char *)p3, .len = strlen(p3) }, test_cb) == ESP_OK, "frag3");
    CHECK(s_evt.got && s_evt.type == DOUBAO_EVT_OUTPUT_TEXT_DELTA, "event type after reassembly");
    CHECK(strcmp(s_evt.str, "Sure, I can h") == 0, "reassembled string data");
    printf("  [%s] fragmented reassembly (1 event / 3 fragments)\n", s_fails == fails_before ? "ok " : "FAIL");
}

static void case_escaped_quote(void)
{
    /* delta contains escaped \" — scanner must not miscount braces/quotes */
    const char *json =
        "{\"type\":\"conversation.item.input_audio_transcription.delta\","
        "\"delta\":\"He said \\\"hi\\\" to me\"}";
    case_feed("escaped \\\" in string value", json, DOUBAO_EVT_TRANSCRIPT_DELTA,
              "He said \"hi\" to me", 0, NULL, 0);
}

static void case_session_created(void)
{
    int fails_before = s_fails;
    memset(&s_evt, 0, sizeof(s_evt));
    const char *json =
        "{\"type\":\"session.created\","
        "\"event_id\":\"event_C9G5RJeJ2gF77mV7f2B1j\","
        "\"session\":{\"id\":\"dlg_test_1234567890\"}}";
    CHECK(proto_feed((ws_frag_t){ .data = (char *)json, .len = strlen(json) }, test_cb) == ESP_OK, "feed");
    CHECK(s_evt.got && s_evt.type == DOUBAO_EVT_SESSION_CREATED, "session.created type");
    const char *sid = proto_get_session_id();
    CHECK(sid != NULL && strcmp(sid, "dlg_test_1234567890") == 0, "session id extracted");
    printf("  [%s] session.created → session id\n", s_fails == fails_before ? "ok " : "FAIL");
}

static void case_multi_event_fragment(void)
{
    int fails_before = s_fails;
    /* two events in one fragment: response.done + audio.done(status 20000002) */
    const char *json =
        "{\"type\":\"response.done\",\"event_id\":\"event_9000\",\"usage\":{\"input_tokens\":12}}"
        "{\"event_id\":\"event_5152\",\"type\":\"response.output_audio.done\","
        "\"status_code\":20000002}";
    memset(&s_evt, 0, sizeof(s_evt));
    s_cb_count = 0;
    CHECK(proto_feed((ws_frag_t){ .data = (char *)json, .len = strlen(json) }, test_cb) == ESP_OK, "feed");
    CHECK(s_cb_count == 2, "both events dispatched from one fragment");
    CHECK(s_evt.type == DOUBAO_EVT_AUDIO_DONE, "last event is audio.done");
    CHECK(s_evt.status_code == 20000002, "last event status (numeric form)");
    printf("  [%s] multiple events in one fragment\n", s_fails == fails_before ? "ok " : "FAIL");
}

static void case_unhandled_type(void)
{
    int fails_before = s_fails;
    memset(&s_evt, 0, sizeof(s_evt));
    const char *json =
        "{\"type\":\"conversation.item.added\",\"event_id\":\"event_C9G8pjSJCfRNEhMEnYAVy\","
        "\"items\":[{\"id\":\"item_1\",\"type\":\"message\",\"role\":\"user\"}]}";
    CHECK(proto_feed((ws_frag_t){ .data = (char *)json, .len = strlen(json) }, test_cb) == ESP_OK, "feed");
    CHECK(!s_evt.got, "no callback for unhandled type");
    printf("  [%s] unhandled type ignored safely\n", s_fails == fails_before ? "ok " : "FAIL");
}

/* ── Upstream builder round-trip ──────────────────────────────────────── */

static void case_builders(void)
{
    int fails_before = s_fails;
    char buf[4096];

    /* session.create with a session_id (reconnect path) */
    doubao_cfg_t cfg = {
        .api_key = "test_key",
        .voice = "zh_female_vv_jupiter_bigtts",
        .instructions = "You are a creative assistant with a \"quote\".",
        .speed = 0,
        .loudness = 0,
    };
    CHECK(proto_build_session_create(buf, sizeof(buf), &cfg) == ESP_OK, "build session.create");
    cJSON *root = cJSON_Parse(buf);
    CHECK(root != NULL, "session.create parses");
    if (root != NULL) {
        cJSON *sess = cJSON_GetObjectItemCaseSensitive(root, "session");
        cJSON *stype = sess ? cJSON_GetObjectItemCaseSensitive(sess, "type") : NULL;
        CHECK(stype != NULL && cJSON_IsString(stype) && strcmp(stype->valuestring, "realtime") == 0, "session.type=realtime");
        cJSON *ext = cJSON_GetObjectItemCaseSensitive(root, "extension");
        CHECK(ext != NULL && cJSON_GetObjectItemCaseSensitive(ext, "asr") != NULL &&
              cJSON_GetObjectItemCaseSensitive(ext, "tts") != NULL &&
              cJSON_GetObjectItemCaseSensitive(ext, "dialog") != NULL,
              "top-level extension asr/tts/dialog");
        cJSON *model = sess ? cJSON_GetObjectItemCaseSensitive(sess, "model") : NULL;
        CHECK(model != NULL && cJSON_IsString(model) && strcmp(model->valuestring, "1.2.6.1") == 0, "model");
        /* Client-generated UUID, always present — mirrors the official
         * demo (server echoes it back as the dialog id). */
        cJSON *id = sess ? cJSON_GetObjectItemCaseSensitive(sess, "id") : NULL;
        CHECK(id != NULL && cJSON_IsString(id) && strlen(id->valuestring) == 36 &&
              id->valuestring[8] == '-' && id->valuestring[13] == '-', "client uuid session.id");
        cJSON *evid = cJSON_GetObjectItemCaseSensitive(root, "event_id");
        CHECK(evid != NULL && cJSON_IsString(evid), "event_id on session.create");
        cJSON *instr = sess ? cJSON_GetObjectItemCaseSensitive(sess, "instructions") : NULL;
        CHECK(instr != NULL && cJSON_IsString(instr) && strstr(instr->valuestring, "\"quote\"") != NULL, "instructions escaped");
        cJSON *voice = sess ? cJSON_GetObjectItemCaseSensitive(sess, "audio") : NULL;
        voice = voice ? cJSON_GetObjectItemCaseSensitive(voice, "output") : NULL;
        voice = voice ? cJSON_GetObjectItemCaseSensitive(voice, "voice") : NULL;
        CHECK(voice != NULL && cJSON_IsString(voice) && strcmp(voice->valuestring, "zh_female_vv_jupiter_bigtts") == 0, "voice");
        cJSON_Delete(root);
    }

    /* audio_append: Base64 round-trip back to identical PCM */
    CHECK(proto_build_audio_append(buf, sizeof(buf), s_test_pcm, 4, 456) == ESP_OK, "build audio append");
    root = cJSON_Parse(buf);
    CHECK(root != NULL, "audio append parses");
    if (root != NULL) {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
        CHECK(type != NULL && cJSON_IsString(type) && strcmp(type->valuestring, "input_audio_buffer.append") == 0, "append type");
        cJSON *audio = cJSON_GetObjectItemCaseSensitive(root, "audio");
        CHECK(audio != NULL && cJSON_IsString(audio), "audio base64 string");
        if (audio != NULL && cJSON_IsString(audio)) {
            int16_t dec[8];
            size_t olen = 0;
            int rc = mbedtls_base64_decode((unsigned char *)dec, sizeof(dec), &olen,
                                           (const unsigned char *)audio->valuestring,
                                           strlen(audio->valuestring));
            CHECK(rc == 0 && olen == sizeof(s_test_pcm), "decode length");
            CHECK(memcmp(dec, s_test_pcm, sizeof(s_test_pcm)) == 0, "pcm round-trip");
        }
        cJSON_Delete(root);
    }

    /* commit / cancel / close / text_push all parse with expected type */
    CHECK(proto_build_commit(buf, sizeof(buf)) == ESP_OK, "build commit");
    root = cJSON_Parse(buf);
    CHECK(root != NULL, "commit parses");
    if (root != NULL) {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
        CHECK(type != NULL && cJSON_IsString(type) && strcmp(type->valuestring, "input_audio_buffer.commit") == 0, "commit type");
        cJSON_Delete(root);
    }
    CHECK(proto_build_cancel(buf, sizeof(buf)) == ESP_OK, "build cancel");
    root = cJSON_Parse(buf);
    CHECK(root != NULL, "cancel parses");
    if (root != NULL) {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
        CHECK(type != NULL && cJSON_IsString(type) && strcmp(type->valuestring, "response.cancel") == 0, "cancel type");
        cJSON_Delete(root);
    }
    CHECK(proto_build_close(buf, sizeof(buf)) == ESP_OK, "build close");
    root = cJSON_Parse(buf);
    CHECK(root != NULL, "close parses");
    if (root != NULL) {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
        CHECK(type != NULL && cJSON_IsString(type) && strcmp(type->valuestring, "session.close") == 0, "close type");
        cJSON_Delete(root);
    }
    CHECK(proto_build_text_push(buf, sizeof(buf), "你好！说 \"hi\"") == ESP_OK, "build text push");
    root = cJSON_Parse(buf);
    CHECK(root != NULL, "text push parses");
    if (root != NULL) {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
        CHECK(type != NULL && cJSON_IsString(type) && strcmp(type->valuestring, "speech_text_buffer.replacement.append") == 0, "text push type");
        cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        CHECK(text != NULL && cJSON_IsString(text) && strcmp(text->valuestring, "你好！说 \"hi\"") == 0, "text push content");
        cJSON_Delete(root);
    }
    CHECK(proto_build_text_commit(buf, sizeof(buf)) == ESP_OK, "build text commit");
    root = cJSON_Parse(buf);
    CHECK(root != NULL, "text commit parses");
    if (root != NULL) {
        cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
        CHECK(type != NULL && cJSON_IsString(type) && strcmp(type->valuestring, "speech_text_buffer.replacement.commit") == 0, "text commit type");
        cJSON_Delete(root);
    }

    printf("  [%s] upstream builders round-trip\n", s_fails == fails_before ? "ok " : "FAIL");
}

/* ── Test runner ──────────────────────────────────────────────────────── */

static int cmd_proto(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "test") != 0) {
        printf("Usage: proto test\n");
        return 1;
    }

    proto_reset();
    s_fails = 0;

    printf("proto test — doubao protocol codec self-test\n");
    case_feed("transcript.delta", s_transcript_delta, DOUBAO_EVT_TRANSCRIPT_DELTA, "Hey", 0, NULL, 0);
    case_feed("output_text.delta", s_output_text_delta, DOUBAO_EVT_OUTPUT_TEXT_DELTA, "Sure, I can h", 0, NULL, 0);
    case_feed("output_audio.delta (base64→PCM)", s_audio_delta, DOUBAO_EVT_AUDIO_DELTA, NULL, 0,
              s_test_pcm, 4);
    case_feed("output_audio.done status=20000002", s_audio_done, DOUBAO_EVT_AUDIO_DONE, NULL, 20000002, NULL, 0);
    case_feed("error event", s_error, DOUBAO_EVT_ERROR, "Invalid API key provided", 0, NULL, 0);
    case_fragmented();
    case_escaped_quote();
    case_session_created();
    case_multi_event_fragment();
    case_unhandled_type();
    case_builders();

    proto_reset();
    if (s_fails == 0) {
        printf("proto test: PASS\n");
        return 0;
    }
    printf("proto test: FAIL (%d)\n", s_fails);
    return 1;
}

void proto_test_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "proto",
        .help = "Doubao protocol codec self-test: proto test",
        .func = &cmd_proto,
    };
    esp_console_cmd_register(&cmd);
}
