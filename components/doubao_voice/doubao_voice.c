/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * doubao_voice — Doubao (Volcengine) realtime voice robot client.
 *
 * Wires the full audio pipeline:
 *   Uplink:  audio_in task → send_audio_task dequeues → proto_build_audio_append
 *            → dbws_send_frame (40ms/frames, 12-frame queue absorbs jitter)
 *   Downlink: proto_feed fires AUDIO_DELTA → resample 24k→16k → audio_out push
 *             AUDIO_DONE(status) → dispatch to callback
 *
 * CLAUDE.md rules applied:
 *   7  — send_audio_task: core 1, prio 8, stack 16KB (铁律3 ≥16KB)
 *   13 — ring buffers absorb jitter; DMA safe
 *   3  — TCB internal RAM, stack PSRAM 16KB for send task (铁律3)
 */

#include "doubao_voice.h"

#include "esp_log.h"
#include <string.h>

#include "doubao_ws_client.h"
#include "doubao_protocol.h"
#include "doubao_audio_in.h"
#include "doubao_audio_out.h"
#include "doubao_resampler.h"

#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "doubao_task_mem.h"

static const char *TAG = "doubao_voice";

/* ── Static state ────────────────────────────────────────────────────── */

static char s_api_key[64];
static char s_voice[64];
static char s_instructions[4096];
static int8_t s_speed;
static int8_t s_loudness;

static doubao_event_cb_t s_cb;
static doubao_frame_cb_t s_frame_cb;

/* Uplink starts muted: capture begins at session.created, but nothing the
 * mic hears should reach the model until a turn is actually opened. */
static volatile bool s_uplink_muted = true;

/* ── Resampler (downlink 24k→16k) ───────────────────────────────────── */

/* Resampler struct in INTERNAL RAM: only 164 bytes, but the inner loop
 * (16-tap dot product × ~2700 inputs/chunk) hits coeffs[] and history[]
 * on every iteration.  PSRAM access latency (~50ns vs ~10ns internal)
 * adds up to measurable play-loop slowdown. */
static resampler_t s_resampler __attribute__((section(".dram1.data")));
static bool s_resampler_inited = false;
/* Static resample output buffer in PSRAM — allocated once, reused every
 * AUDIO_DELTA.  Avoids per-frame internal-RAM malloc/free which causes
 * fragmentation leading to SPI DMA allocation failures. */
static int16_t *s_resample_buf;
/* Must hold the FULL resampled output of one delta: PROTO_PCM_CAP is 64KB
 * = 32768 input samples, resampling to ~21846 outputs. A 1024-sample cap
 * here truncated every delta — the resampler consumes only as many inputs
 * as fit the cap and the remainder of the chunk was silently lost, which
 * shredded the TTS audio every network chunk. */
#define RESAMPLE_BUF_SAMPLES  32768

static void ensure_resampler(void)
{
    if (!s_resampler_inited) {
        memset(&s_resampler, 0, sizeof(s_resampler));
        resampler_init(&s_resampler);
        s_resampler_inited = true;
    }
    if (!s_resample_buf) {
        s_resample_buf = heap_caps_calloc(1, RESAMPLE_BUF_SAMPLES * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
}

/* ── Send audio task (uplink: dequeue → encode → send) ───────────────── */

static TaskHandle_t s_send_task;
static volatile bool s_send_running = false;
static volatile bool s_send_stop = false;

/* Uplink counters (see the periodic "uplink:" log in send_audio_task) */
static uint32_t s_tx_ok, s_tx_fail, s_tx_bytes;

/* Static buffers for the send task (PSRAM for base64 frame build) */
#define SEND_FRAME_BUF_CAP  4096

static void send_audio_task(void *arg)
{
    (void)arg;

    int16_t *pcm_buf = heap_caps_calloc(1, DBAUDIO_FRAME_SAMPLES * sizeof(int16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *frame_buf = heap_caps_calloc(1, SEND_FRAME_BUF_CAP,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm_buf || !frame_buf) {
        ESP_LOGE(TAG, "send task OOM");
        if (pcm_buf) heap_caps_free(pcm_buf);
        if (frame_buf) heap_caps_free(frame_buf);
        s_send_running = false;
        vTaskDeleteWithCaps(NULL);  /* PSRAM stack — see doubao_task_mem.h */
        return;
    }

    uint32_t frame_id = 0;
    /* 20ms per wire packet: 320 samples @ 16k/int16 = 640 bytes, the doc's
     * recommended cadence (used by both the real path and the silence pump) */
    const size_t WIRE_SAMPLES = 320;
    ESP_LOGI(TAG, "send audio task started");

    while (!s_send_stop) {
        /* Uplink only flows while a session is open: frames queued before
         * session.created are dropped here (the capture queue's
         * drop-oldest behaviour keeps the newest audio, so a user who
         * starts speaking during the ~0.2s session.create round-trip loses
         * nothing but the first instants). */
        if (!dbws_is_connected() || !dbws_session_active()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        bool muted = s_uplink_muted;

        /* Silence pump: during playback the capture task is parked
         * (sequential-phase model), so the queue goes empty and no frames
         * would flow. The server REQUIRES a continuous input stream —
         * a dead uplink stalls its TTS and kills the session after ~30s
         * (55000000/52000016 AudioTTSIdleTimeoutError, measured). Send
         * true zero-valued PCM at the normal 20ms cadence instead of
         * dequeuing; the transition paths all reset the capture queue
         * before unmuting, so no real frames linger here. */
        if (muted) {
            /* Adaptive throttle: when the TX queue is backed up (drain
             * lagging behind a receive burst), skip this frame instead of
             * piling on. The stream stays alive — frames resume as the
             * drain catches up — without the queue-full drops that
             * stalled the pump cadence. */
            if (dbws_tx_queue_depth() > 8) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            memset(pcm_buf, 0, WIRE_SAMPLES * sizeof(int16_t));
            esp_err_t ret = proto_build_audio_append(frame_buf, SEND_FRAME_BUF_CAP,
                                                      pcm_buf, WIRE_SAMPLES, frame_id++);
            if (ret == ESP_OK) {
                size_t frame_len = strlen(frame_buf);
                ret = dbws_send_frame(frame_buf, frame_len);
                if (ret != ESP_OK) {
                    s_tx_fail++;
                    ESP_LOGD(TAG, "silence frame send failed: %s", esp_err_to_name(ret));
                } else {
                    s_tx_ok++;
                    s_tx_bytes += frame_len;
                }
            }
            if ((frame_id % 250) == 0) {
                ESP_LOGI(TAG, "uplink: %lu sent / %lu failed, %luKB, muted=%d, qdepth=%d",
                         (unsigned long)s_tx_ok, (unsigned long)s_tx_fail,
                         (unsigned long)(s_tx_bytes / 1024), (int)muted,
                         dbaudio_in_queue_depth());
            }
            vTaskDelay(pdMS_TO_TICKS(20));   /* 320 samples @ 16kHz = 20ms */
            continue;
        }

        size_t samples = dbaudio_in_dequeue(pcm_buf, DBAUDIO_FRAME_SAMPLES);
        if (samples == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Capture produces 640-sample (40ms) blocks; split each block into
         * two wire frames so the server's stream alignment / VAD sees the
         * cadence it expects — the capture path stays untouched.
         * The muted case never reaches here (silence pump above), so these
         * frames are always the real mic audio. */
        bool should_commit = false;
        for (size_t base = 0; base < samples; base += WIRE_SAMPLES) {
            size_t n = (samples - base) < WIRE_SAMPLES ? (samples - base) : WIRE_SAMPLES;

            /* VAD frame callback on the real mic sub-frame: maintains RMS
             * and signals end-of-speech commits. */
            if (s_frame_cb) {
                should_commit |= s_frame_cb(pcm_buf + base, n);
            }

            esp_err_t ret = proto_build_audio_append(frame_buf, SEND_FRAME_BUF_CAP,
                                                      pcm_buf + base, n, frame_id++);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "audio frame build failed: %s", esp_err_to_name(ret));
                continue;
            }

            size_t frame_len = strlen(frame_buf);
            ret = dbws_send_frame(frame_buf, frame_len);
            if (ret != ESP_OK) {
                s_tx_fail++;
                ESP_LOGD(TAG, "audio frame send failed: %s", esp_err_to_name(ret));
            } else {
                s_tx_ok++;
                s_tx_bytes += frame_len;
            }
        }

        /* Uplink boundary evidence. Without this the send path is a black
         * box: a silent dbws_send_frame failure and a healthy uplink look
         * identical from the log, and "the server never answered" cannot be
         * told apart from "we never actually transmitted". ~5s at 50fps. */
        if ((frame_id % 250) == 0) {
            ESP_LOGI(TAG, "uplink: %lu sent / %lu failed, %luKB, muted=%d, qdepth=%d",
                     (unsigned long)s_tx_ok, (unsigned long)s_tx_fail,
                     (unsigned long)(s_tx_bytes / 1024), (int)muted,
                     dbaudio_in_queue_depth());
        }

        /* Auto-commit on VAD silence/max-timeout */
        if (should_commit) {
            ESP_LOGI(TAG, "VAD triggered commit");
            char cframe[512];
            esp_err_t ret = proto_build_commit(cframe, sizeof(cframe));
            if (ret == ESP_OK) {
                dbws_send_frame(cframe, strlen(cframe));
            }
        }
    }

    heap_caps_free(pcm_buf);
    heap_caps_free(frame_buf);
    s_send_running = false;
    ESP_LOGI(TAG, "send audio task stopped");
    vTaskDeleteWithCaps(NULL);  /* PSRAM stack — see doubao_task_mem.h */
}

/* ── Event dispatch (intercepts audio events for downlink pipeline) ──── */

/* Static counter for audio delta diagnostics */
static uint32_t s_dispatch_audio_cnt = 0;
static uint32_t s_dispatch_audio_bytes = 0;

/* Stale-audio gate (res.prd §8 response_generation 的简化版): a response
 * audio stream is OPEN only between response.output_audio.started and
 * response.output_audio.done — or until a LOCAL interrupt (response.cancel).
 * Late output_audio.delta frames of a canceled response can still arrive
 * over the network after the cancel; without this gate they land in the
 * playback ring and the OLD response keeps talking over the new turn. */
static volatile bool s_audio_stream_open = false;

static const char *evt_name(doubao_event_type_t t)
{
    switch (t) {
        case DOUBAO_EVT_CONNECTED:        return "CONNECTED";
        case DOUBAO_EVT_DISCONNECTED:     return "DISCONNECTED";
        case DOUBAO_EVT_SESSION_CREATED:  return "SESSION_CREATED";
        case DOUBAO_EVT_TRANSCRIPT_DELTA: return "TRANSCRIPT_DELTA";
        case DOUBAO_EVT_TRANSCRIPT_DONE:  return "TRANSCRIPT_DONE";
        case DOUBAO_EVT_OUTPUT_TEXT_DELTA: return "OUTPUT_TEXT_DELTA";
        case DOUBAO_EVT_OUTPUT_TEXT_DONE:  return "OUTPUT_TEXT_DONE";
        case DOUBAO_EVT_AUDIO_STARTED:    return "AUDIO_STARTED";
        case DOUBAO_EVT_AUDIO_DELTA:      return "AUDIO_DELTA";
        case DOUBAO_EVT_AUDIO_DONE:       return "AUDIO_DONE";
        case DOUBAO_EVT_RESPONSE_DONE:    return "RESPONSE_DONE";
        case DOUBAO_EVT_INTERRUPTED:      return "INTERRUPTED";
        case DOUBAO_EVT_SESSION_CLOSED:   return "SESSION_CLOSED";
        case DOUBAO_EVT_ERROR:            return "ERROR";
        default:                          return "UNKNOWN";
    }
}

static void dispatch(doubao_event_type_t type, const void *data, size_t len)
{
    ESP_LOGD(TAG, "dispatch: %s (data=%p len=%d)",
             evt_name(type), data, (int)len);

    /* session.created opens the uplink gate: audio sent in the
     * session.create→session.created window is dropped by the server
     * (measured), so the send task stays closed until this fires. */
    if (type == DOUBAO_EVT_SESSION_CREATED) {
        dbws_session_mark_created();
    }
    /* session.closed unblocks the next session.open — the server rejects
     * session.create while the previous close is still being processed
     * (45000000 "previous session is running"). */
    if (type == DOUBAO_EVT_SESSION_CLOSED) {
        dbws_session_mark_closed();
    }

    /* Downlink audio pipeline: intercept AUDIO_DELTA → resample → audio_out.
     * Gated by s_audio_stream_open: late deltas of a canceled response
     * must not reach the speaker (stale-audio discard, res.prd §8). */
    if (type == DOUBAO_EVT_AUDIO_DELTA && data != NULL && !s_audio_stream_open) {
        static uint32_t s_stale_drop_cnt = 0;
        if ((++s_stale_drop_cnt % 25) == 1) {
            ESP_LOGW(TAG, "stale audio delta dropped %u times (post-cancel)",
                     (unsigned)s_stale_drop_cnt);
        }
    } else if (type == DOUBAO_EVT_AUDIO_DELTA && data != NULL) {
        const doubao_audio_chunk_t *chunk = (const doubao_audio_chunk_t *)data;
        s_dispatch_audio_cnt++;
        s_dispatch_audio_bytes += chunk->samples * 2;
        if ((s_dispatch_audio_cnt % 100) == 1) {
            ESP_LOGI(TAG, "audio dispatch #%lu: %d samples, total %luKB",
                     (unsigned long)s_dispatch_audio_cnt,
                     (int)chunk->samples,
                     (unsigned long)(s_dispatch_audio_bytes / 1024));
        }
        if (chunk->pcm24 && chunk->samples > 0) {
            ensure_resampler();

            if (s_resample_buf) {
                size_t n_out = resampler_process(&s_resampler,
                                                  chunk->pcm24, chunk->samples,
                          s_resample_buf, RESAMPLE_BUF_SAMPLES);
                if (n_out > 0) {
                    dbaudio_out_push(s_resample_buf, n_out);
                    if ((s_dispatch_audio_cnt % 100) == 1) {
                        ESP_LOGI(TAG, "resampled %d→%d samples, ring=%d",
                                 (int)chunk->samples, (int)n_out,
                                 dbaudio_out_ring_depth());
                    }
                } else {
                    ESP_LOGW(TAG, "resampler returned 0 output samples");
                }
            } else {
                ESP_LOGW(TAG, "resample buffer not available");
            }
        } else {
            ESP_LOGW(TAG, "audio delta with null pcm24 or 0 samples");
        }
    }

    /* Sequential-phase model: capture MUST stop BEFORE playback starts.
     * Running both tasks simultaneously doubles internal DRAM usage
     * (two 16KB PSRAM-stack TCBs + two DMA buffers) which starves the
     * LCD SPI DMA allocator → "Failed to allocate priv TX buffer" →
     * display freezes.  Stop capture here first, then the callback
     * (doubao_chat) won't need to call dbaudio_in_stop() again. */
    if (type == DOUBAO_EVT_AUDIO_STARTED) {
        ESP_LOGI(TAG, "AUDIO_STARTED: stopping capture, starting playback");
        s_audio_stream_open = true;
        dbaudio_in_stop();
        dbaudio_out_start();
        /* Disable WiFi power save during audio playback: MIN_MODEM causes
         * ~100ms sleep/wake cycles that introduce latency spikes on the
         * WebSocket RX path, delaying audio delta delivery and causing
         * "ring empty" output gaps.  NONE keeps the radio always-on for
         * the 2-30s duration of a TTS response — negligible power impact
         * on a mains-powered desktop device. */
        esp_wifi_set_ps(WIFI_PS_NONE);
    }

    if (type == DOUBAO_EVT_AUDIO_DONE) {
        s_audio_stream_open = false;
        ESP_LOGI(TAG, "AUDIO_DONE: audio_cnt=%lu totalKB=%lu",
                 (unsigned long)s_dispatch_audio_cnt,
                 (unsigned long)(s_dispatch_audio_bytes / 1024));
        s_dispatch_audio_cnt = 0;
        s_dispatch_audio_bytes = 0;
        /* Restore WiFi power save after playback ends */
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    }

    /* Forward all events to the user callback */
    if (s_cb != NULL) {
        s_cb(type, data, len);
    }
}

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void cfg_view(doubao_cfg_t *out)
{
    out->api_key = s_api_key;
    out->voice = s_voice;
    out->instructions = s_instructions;
    out->speed = s_speed;
    out->loudness = s_loudness;
}

static bool ensure_send_task(void)
{
    if (s_send_running) return true;

    s_send_stop = false;
    BaseType_t ret = DB_TASK_CREATE_PSRAM(
        send_audio_task, "doubao_send", 16384, NULL, 8, &s_send_task, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create send task");
        return false;
    }
    s_send_running = true;
    return true;
}

/* ── Public API ──────────────────────────────────────────────────────── */

esp_err_t doubao_init(const doubao_cfg_t *cfg, doubao_event_cb_t cb)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(s_api_key, cfg->api_key ? cfg->api_key : "", sizeof(s_api_key) - 1);
    s_api_key[sizeof(s_api_key) - 1] = '\0';
    strncpy(s_voice, cfg->voice ? cfg->voice : "", sizeof(s_voice) - 1);
    s_voice[sizeof(s_voice) - 1] = '\0';
    strncpy(s_instructions, cfg->instructions ? cfg->instructions : "",
            sizeof(s_instructions) - 1);
    s_instructions[sizeof(s_instructions) - 1] = '\0';
    s_speed    = cfg->speed;
    s_loudness = cfg->loudness;
    s_cb       = cb;

    doubao_cfg_t view;
    cfg_view(&view);
    esp_err_t ret = dbws_start(&view, dispatch);
    if (ret != ESP_OK) {
        return ret;
    }

    /* The uplink consumer must exist wherever the WS is brought up, not
     * just in doubao_connect(): this function starts dbws itself, and it
     * is the only path the boot sequence takes. Without it the capture
     * task produces into a queue nobody drains — every frame is dropped
     * ("send queue overflow"), the server receives no audio at all, and
     * the session dies on the audio-idle timeout.
     * Idempotent, so the doubao_connect() call is harmless. */
    if (!ensure_send_task()) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t doubao_connect(void)
{
    if (s_cb == NULL && s_api_key[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    doubao_cfg_t view;
    cfg_view(&view);
    esp_err_t ret = dbws_start(&view, dispatch);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!dbws_is_connected()) {
        dbws_request_reconnect();
    }
    ensure_send_task();
    return ESP_OK;
}

esp_err_t doubao_disconnect(void)
{
    /* Stop send task */
    if (s_send_running) {
        s_send_stop = true;
        for (int i = 0; i < 20 && s_send_running; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    /* Stop audio out */
    dbaudio_out_stop();
    return dbws_stop();
}

bool doubao_is_connected(void)
{
    return dbws_is_connected();
}

/* Per-conversation session lifecycle (see doubao_ws_client.h): open on
 * wake, close when the conversation ends. An idle session fed silence
 * goes ASR-dead on the server after ~60s, so it must never idle open. */
esp_err_t doubao_ensure_session(void)
{
    return dbws_session_open();
}

esp_err_t doubao_close_session(void)
{
    return dbws_session_close();
}

bool doubao_session_active(void)
{
    return dbws_session_active();
}

esp_err_t doubao_send_audio(const int16_t *pcm16, size_t samples)
{
    /* Direct send bypass for special cases (e.g. loopback test).
     * Normal path: audio_in task captures → send_audio_task dequeues. */
    if (!dbws_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!pcm16 || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    static uint32_t direct_frame_id = 0;
    char frame[SEND_FRAME_BUF_CAP];
    esp_err_t ret = proto_build_audio_append(frame, sizeof(frame),
                                              pcm16, samples, direct_frame_id++);
    if (ret != ESP_OK) return ret;
    return dbws_send_frame(frame, strlen(frame));
}

esp_err_t doubao_commit_audio(void)
{
    if (!dbws_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    char frame[512];
    esp_err_t ret = proto_build_commit(frame, sizeof(frame));
    if (ret != ESP_OK) return ret;
    return dbws_send_frame(frame, strlen(frame));
}

esp_err_t doubao_interrupt(void)
{
    /* Stop local playback immediately. Also close the stale-audio gate:
     * the server may still have queued deltas of the canceled response —
     * they must be dropped, not played over the user's new turn. */
    s_audio_stream_open = false;
    dbaudio_out_interrupt();

    if (!dbws_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Send response.cancel to server */
    char frame[512];
    esp_err_t ret = proto_build_cancel(frame, sizeof(frame));
    if (ret != ESP_OK) return ret;
    return dbws_send_frame(frame, strlen(frame));
}

esp_err_t doubao_clear_session(void)
{
    /* 1. Interrupt: stop local audio + cancel server response */
    doubao_interrupt();

    /* 2. Stop capture and playback */
    dbaudio_in_stop();
    dbaudio_out_stop();
    dbaudio_in_reset_queue();

    /* 3. session.close (via the session API so the active/pending flags
     * stay consistent — the uplink gate must close with the session) */
    dbws_session_close();

    /* 4. Reset resampler state */
    if (s_resampler_inited) {
        memset(&s_resampler.history, 0, sizeof(s_resampler.history));
        s_resampler.wr_idx = 0;
        s_resampler.phase = 0;
    }

    /* 5. Reconnect (will session.create fresh without old session.id
     *    because proto_reset was called or server rejects old id) */
    return ESP_OK;
}

esp_err_t doubao_push_text(const char *text)
{
    if (text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!dbws_is_connected()) {
        ESP_LOGW(TAG, "push_text: not connected");
        return ESP_ERR_INVALID_STATE;
    }
    /* speech_text_buffer.* belongs to an open session (the demo sends its
     * greeting only after session.create). With sessions-per-conversation,
     * boot-time announcements have no session — reject early instead of
     * letting the server drop or error the orphaned frame. */
    if (!dbws_session_active()) {
        ESP_LOGW(TAG, "push_text: no active session");
        return ESP_ERR_INVALID_STATE;
    }
    /* Static PSRAM buffer: avoids 2KB stack frame which would overflow
     * in small-stack tasks (console=4KB, send_task=8KB). */
    static char *frame = NULL;
    if (!frame) {
        frame = heap_caps_calloc(1, 2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!frame) return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = proto_build_text_push(frame, 2048, text);
    if (ret != ESP_OK) return ret;
    ret = dbws_send_frame(frame, strlen(frame));
    if (ret != ESP_OK) return ret;
    ret = proto_build_text_commit(frame, 2048);
    if (ret != ESP_OK) return ret;
    return dbws_send_frame(frame, strlen(frame));
}

void doubao_set_uplink_muted(bool muted)
{
    if (s_uplink_muted != muted) {
        ESP_LOGI(TAG, "uplink %s", muted ? "muted (sending silence)" : "live");
    }
    s_uplink_muted = muted;
}

bool doubao_uplink_is_muted(void)
{
    return s_uplink_muted;
}

void doubao_set_frame_cb(doubao_frame_cb_t cb)
{
    s_frame_cb = cb;
}

const char *doubao_get_session_id(void)
{
    return dbws_get_session_id();
}
