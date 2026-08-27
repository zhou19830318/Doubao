/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * doubao_chat — 对话编排（Task 7~12）。
 *
 * 完整对话数据流（设计文档 §6.1）：
 *   唤醒/单击 → wake_word_pause → ensure WSS connected → LISTENING
 *     → audio_in start → mic 40ms/帧 → VAD 判停 → commit_audio
 *     → THINKING → output_text.delta → 气泡流式
 *     → output_audio.delta → resample → 播放
 *     → 播报中用户开口 → 本地能量打断 → interrupt → 回 LISTENING
 *     → response.done → 自动续听(idle 8s) 或退出意图(20000002) → IDLE
 *
 * 线程：回调运行在 WSS 任务上下文；UI 调用一律 lvgl_port_lock/unlock；
 * SD/NVS 等慢操作不持锁。回调 data 生命周期：协议层内部缓冲，回调内即拷贝。
 *
 * CLAUDE.md rules applied:
 *   33 — 状态超时看门狗（commit 后 15s / 播报中 5s 无 audio delta）
 *   15 — 本地能量打断（vad_rms > play_rms + 6dB 且持续 80ms）
 *   14 — 播放感知门控
 */

#include "doubao_chat.h"

#include "app_state.h"
#include "app_state_machine.h"
#include "ui.h"
#include "error_log.h"
#include "notes_manager.h"
#include "vad.h"
#include "settings.h"
#include "wake_word.h"
#include "doubao_voice.h"
#include "doubao_audio_in.h"
#include "doubao_audio_out.h"
#include "board.h"
#include "recording_sounds.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

/* Strip [DEVICE:...] tags from a string in-place using memmove.
 * Returns the number of tags removed.  Used to clean streaming text
 * before it reaches the UI and the TTS audio pipeline. */
static int strip_device_tags(char *buf)
{
    int count = 0;
    char *p = buf;
    while ((p = strstr(p, "[DEVICE:")) != NULL) {
        char *start = p;
        char *end = strchr(p, ']');
        if (!end) break;
        size_t skip = (end + 1) - start;
        memmove(start, end + 1, strlen(end + 1) + 1);
        count++;
        p = start;  /* continue scanning from same position */
    }
    return count;
}
static const char *TAG = "doubao_chat";

/* Helper: play a short PCM sound through the speaker (Ver2.0 同款音效).
 * Routed through the audio_out play task (prio 10) — direct
 * board_audio_play() from this priority-1 caller let RX-processing
 * bursts starve the feeding loop and underrun the I2S TX DMA mid-tone
 * (audible stutter in 叮咚/咚叮). */
static void play_feedback_sound(const int16_t *pcm, size_t len)
{
    dbaudio_out_play_tone(pcm, len);
}

/* Wake-word / BOOT-button feedback flag: when set, start_listening()
 * skips its own 叮咚 (the caller already played it before entering
 * LISTENING state). Cleared at the start of each conversation turn. */
static bool s_wake_feedback_played = false;

void doubao_chat_play_wake_feedback(void)
{
    /* 叮咚：唤醒/按键确认音，在进入 LISTENING 之前播放。
     * 阻塞 ~200ms。start_listening() 会检查 s_wake_feedback_played
     * 跳过重复播放。 */
    play_feedback_sound(snd_rec_start, snd_rec_start_len);
    s_wake_feedback_played = true;
}

/* ── Dialogue state ──────────────────────────────────────────────────── */

typedef enum {
    CHAT_IDLE,          /* waiting for start */
    CHAT_LISTENING,     /* mic capturing, VAD active */
    CHAT_COMMITTING,    /* commit sent, waiting for server response */
    CHAT_THINKING,      /* server processing (transcript received) */
    CHAT_SPEAKING,      /* audio delta being played */
    CHAT_AUTO_LISTEN,   /* response done, auto-continue listening */
} chat_state_t;

static chat_state_t s_chat_state = CHAT_IDLE;

/* Auto-continue parameters (from settings) */
static int s_idle_timeout_s = 8;     /* silence before returning to IDLE */
static int s_idle_frame_cnt = 0;     /* frames of silence in AUTO_LISTEN */

/* Timeouts (spec §7 / 铁律 10) */
#define THINKING_TIMEOUT_S  30   /* 思考 30s 无 output → error */
#define COMMIT_TIMEOUT_S    15   /* commit后 15s 无 output → error */
#define SPEAK_TIMEOUT_S     60   /* 播报中 60s 无 audio delta → stop (song responses can be 30s+) */
static int s_timeout_cnt = 0;
static unsigned long s_audio_delta_cnt = 0;

/* Interrupt detection: play_rms reference for barge-in (spec §3.3) */
#define INTERRUPT_DB_MARGIN  6.0f  /* 6dB above play_rms */
#define INTERRUPT_HOLD_MS    80    /* sustained 80ms to trigger */
static int s_interrupt_hold_frames = 0;

/* Output text buffer for response.done handling */
static char s_final_text[NOTES_MAX_MESSAGE_LEN];
static bool s_bot_has_text = false;

/* ── VAD frame callback (runs in send task context) ──────────────────── */
/* One commit per turn. vad_process() reports a LEVEL, not an EDGE: once
 * silence-after-speech (or the max-record timeout) is reached it returns
 * that same verdict on every subsequent frame. That was harmless when the
 * mic stopped at commit time, but capture now runs continuously, so an
 * unlatched callback fires ~25 commits/second. The server dutifully ACKs
 * each one (input_audio_buffer.committed) while every commit seals a 40ms
 * fragment far too short to transcribe — which is exactly why it never
 * returned a single transcription event. */
static bool s_committed_this_turn = false;

/* Track whether any speech was detected in this turn. VAD fires commit
 * on SILENCE/MAX_TIMEOUT, but we must NOT commit silence that was never
 * preceded by speech — that just wastes bandwidth and confuses the server. */
static bool s_speech_heard_this_turn = false;

/* Set when a turn opened before its session existed: the session.create
 * was just sent and the capture is buffering; session.created releases
 * the uplink. */
static bool s_waiting_session = false;

/* Set by vad_frame_callback (send task context) the moment a commit is
 * actually transmitted; the tick promotes LISTENING→COMMITTING on the
 * next pass. Decouples the cross-task signal from the chat state so the
 * 15s commit watchdog starts from the real commit, not from the first
 * transcript delta. */
static volatile bool s_commit_pending = false;

/* True once this turn's response has produced audio — distinguishes
 * spoken responses (audio.done ends the turn) from text-only ones
 * (response.done must end it). */
static bool s_audio_active = false;

/* audio.done received but the ring still holds unplayed audio (drain
 * phase). The tick moves SPEAKING→AUTO_LISTEN only after the playback
 * task has actually run the ring dry — cutting playback at audio.done
 * used to truncate the tail of every response. */
static bool s_audio_done_pending = false;

static void go_idle(void);

static bool vad_frame_callback(const int16_t *pcm, size_t samples)
{
    vad_state_t st = vad_process(pcm, samples);
    if (st == VAD_SPEECH) {
        s_speech_heard_this_turn = true;
        return false;
    }
    if (st != VAD_SILENCE && st != VAD_MAX_TIMEOUT) {
        return false;
    }
    /* Only commit if we actually heard speech — never commit pure silence */
    if (!s_speech_heard_this_turn) {
        return false;
    }
    if (s_committed_this_turn) {
        return false;  /* level already consumed — wait for the next turn */
    }
    /* During AUTO_LISTEN the server owns the session; committing silence
     * here just wastes bandwidth and may confuse ASR.  Only commit in
     * the LISTENING state (user-initiated turn). */
    if (s_chat_state == CHAT_AUTO_LISTEN) {
        return false;
    }
    s_committed_this_turn = true;
    s_commit_pending = true;   /* tick promotes LISTENING→COMMITTING */
    return true;
}

/* ── Start dialogue (唤醒词 / 单击 / say 命令) ────────────────────────── */

void doubao_chat_start(void)
{
    if (g_app_events) {
        xEventGroupSetBits(g_app_events, DOUBAO_START_BIT);
        ESP_LOGD(TAG, "DOUBAO_START_BIT set");
    }
}

/* Internal start: called from DOUBAO_START_BIT handler and auto-listen */
void doubao_chat_start_listening(void)
{
    if (s_chat_state != CHAT_IDLE && s_chat_state != CHAT_AUTO_LISTEN) {
        ESP_LOGD(TAG, "start_listening: ignored (state=%d)", s_chat_state);
        return;
    }

    /* Ensure WSS is connected */
    if (!doubao_is_connected()) {
        ESP_LOGW(TAG, "WSS not connected — attempting reconnect");
        doubao_connect();
        /* The caller already moved the UI to LISTENING and paused the
         * wake word. Just returning here would leave the UI stuck in
         * LISTENING with no capture and a deaf wake word — unwind to
         * IDLE (reconnect proceeds in the background; next wake works). */
        go_idle();
        return;
    }

    /* Sessions live per conversation now (the server's ASR goes dead after
     * ~60s of pure silence, so an idle session must not exist). If none is
     * open, send session.create and start capture so the user's first
     * words buffer while the round-trip completes; session.created flips
     * s_waiting_session and the buffered audio drains onto the wire. */
    if (!doubao_session_active()) {
        ESP_LOGI(TAG, "opening session for turn");
        s_waiting_session = true;
        doubao_ensure_session();
    }

    /* Start (or keep) capture + the frame callback for this conversation.
     * Pause the wake word explicitly (idempotent): the doubao path owns
     * the mic for the whole conversation regardless of which trigger
     * (wake word / button / CLI) opened it. */
    wake_word_pause();
    /* 采集必须先于提示音启动：session.created 一到达，上行流必须立即
     * 开流——服务端会杀掉"会话已创建但流从未开始"的连接（实测
     * ~120ms 即断）。旧顺序（先叮咚再采集）把首帧拖到 session.created
     * 后 1.1s，导致服务端秒断。提示音在采集之后播放，麦克风会拾到
     * 少量叮咚回声——服务端 ASR 对此有容忍（demo 的 greeting 同理）。 */
    dbaudio_in_reset_queue();
    doubao_set_frame_cb(vad_frame_callback);
    if (dbaudio_in_start() != ESP_OK) {
        ESP_LOGE(TAG, "capture start failed");
    }
    /* 叮咚：进入聆听的反馈音（Ver2.0 同款），唤醒词/单击共用。
     * 如果调用方已在进入 LISTENING 前播放过（doubao_chat_play_wake_
     * feedback），跳过重复播放。 */
    if (!s_wake_feedback_played) {
        play_feedback_sound(snd_rec_start, snd_rec_start_len);
    }
    s_wake_feedback_played = false;  /* consumed */

    /* Arm the VAD BEFORE opening the wire. Unmuting first leaves a window
     * where frames are live but the VAD still holds the previous turn's
     * (or, at boot, a zeroed) state — with max_ms == 0 every frame reports
     * VAD_MAX_TIMEOUT and we spray input_audio_buffer.commit at the server
     * before the user has said a word. */
    const settings_t *cfg = settings_get();
    vad_init(cfg && cfg->silence_timeout_ms ? cfg->silence_timeout_ms : 1500,
             cfg && cfg->max_record_seconds ? (cfg->max_record_seconds * 1000) : 15000);    vad_reset();
    s_committed_this_turn = false;
    s_speech_heard_this_turn = false;
    s_commit_pending = false;
    s_audio_done_pending = false;
    doubao_set_uplink_muted(false);

    s_chat_state = CHAT_LISTENING;
    s_timeout_cnt = 0;
    s_interrupt_hold_frames = 0;
    s_audio_active = false;

    lvgl_port_lock(0);
    app_state_request(UI_STATE_LISTENING);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "listening started");
}

/* ── Auto-continue: response done, keep listening for follow-up ──────── */

static void enter_auto_listen(void)
{
    const settings_t *cfg = settings_get();
    if (!cfg || !cfg->auto_continue) {
        goto go_idle;
    }

    s_chat_state = CHAT_AUTO_LISTEN;
    s_idle_frame_cnt = 0;
    s_idle_timeout_s = cfg->idle_timeout_s ? cfg->idle_timeout_s : 8;

    /* Reset VAD flags BEFORE capture restarts — the send task dequeues
     * frames and calls vad_frame_callback as soon as capture starts;
     * if s_speech_heard_this_turn is still true from the previous turn,
     * the first silence frame triggers a spurious commit. */
    vad_reset();
    s_committed_this_turn = false;
    s_speech_heard_this_turn = false;
    s_commit_pending = false;
    s_audio_done_pending = false;

    /* Mic never stopped — just re-open the wire for the follow-up turn. */
    dbaudio_in_reset_queue();
    doubao_set_uplink_muted(false);

    /* Update UI: show LISTENING so the user knows the device is waiting
     * for a follow-up.  Use app_set_state() (not app_state_request)
     * because the audio handler uses app_set_state(TTS_PLAYING) which
     * bypasses the state machine — so the machine still thinks we're in
     * THINKING (7) and rejects THINKING→LISTENING. */
    lvgl_port_lock(0);
    app_set_state(UI_STATE_LISTENING);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "auto-listen: waiting for follow-up (timeout %ds)",
             s_idle_timeout_s);
    return;

go_idle:
    go_idle();
}

/* ── Go idle ─────────────────────────────────────────────────────────── */

static void go_idle(void)
{
    s_chat_state = CHAT_IDLE;
    s_waiting_session = false;
    doubao_set_uplink_muted(true);
    dbaudio_out_stop();
    /* Conversation over — tear the session down. An idle session fed
     * silence goes ASR-dead on the server after ~60s, and it costs
     * bandwidth/privacy meanwhile. The WSS connection itself stays up for
     * the next conversation. The mic returns to the wake-word task. */
    dbaudio_in_stop();
    doubao_close_session();
    vad_reset();
    s_committed_this_turn = false;
    s_speech_heard_this_turn = false;
    s_commit_pending = false;
    s_audio_done_pending = false;
    s_timeout_cnt = 0;
    s_interrupt_hold_frames = 0;

    /* The mic goes back to the wake-word task. This MUST happen here:
     * app_set_state() bypasses the legacy state machine whose
     * on_enter_state(IDLE) hook used to resume it — without this the
     * device stays deaf after the first conversation (field-confirmed). */
    wake_word_resume();

    lvgl_port_lock(0);
    app_set_state(UI_STATE_IDLE);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "returned to idle");
}

/* ── Event callback (runs in WSS task context) ───────────────────────── */

static const char *chat_state_name(chat_state_t s)
{
    switch (s) {
        case CHAT_IDLE:        return "IDLE";
        case CHAT_LISTENING:   return "LISTENING";
        case CHAT_COMMITTING:  return "COMMITTING";
        case CHAT_THINKING:    return "THINKING";
        case CHAT_SPEAKING:    return "SPEAKING";
        case CHAT_AUTO_LISTEN: return "AUTO_LISTEN";
        default:               return "???";
    }
}

void doubao_chat_on_event(doubao_event_type_t type, const void *data, size_t len)
{
    ESP_LOGD(TAG, "chat_event: type=%d state=%s data=%p len=%d",
             (int)type, chat_state_name(s_chat_state), data, (int)len);

    switch (type) {

    case DOUBAO_EVT_SESSION_CREATED:
        ESP_LOGI(TAG, "session created: id=%s",
                 doubao_get_session_id() ? doubao_get_session_id() : "(none)");
        if (s_waiting_session) {
            /* A turn was opened before the session existed — release the
             * buffered capture onto the wire now. */
            s_waiting_session = false;
            vad_reset();
            s_committed_this_turn = false;
            ESP_LOGI(TAG, "session ready — releasing buffered uplink");
        }
        break;

    case DOUBAO_EVT_CONNECTED:
        ESP_LOGI(TAG, "WSS connected");
        /* Transport-level readiness drives CONNECTING→IDLE now that
         * sessions are per-conversation: an idle device has no session at
         * all, so there is nothing else to wait for at boot. */
        {
            ui_state_t cur = ui_get_state();
            if (cur == UI_STATE_ERROR || cur == UI_STATE_CONNECTING) {
                lvgl_port_lock(0);
                app_state_request(UI_STATE_IDLE);
                lvgl_port_unlock();
            }
        }
        break;

    case DOUBAO_EVT_DISCONNECTED:
        ESP_LOGW(TAG, "WSS disconnected");
        s_waiting_session = false;
        if (s_chat_state != CHAT_IDLE) {
            /* Connection lost during dialogue → stop gracefully */
            go_idle();
        }
        doubao_set_uplink_muted(true);
        dbaudio_in_stop();
        break;

    /* ── Transcript: user speech recognition ──────────────────────────── */

    case DOUBAO_EVT_TRANSCRIPT_DELTA:
        /* Streaming transcript: update the bubble ONLY. The user is still
         * speaking — jumping to THINKING here (old behaviour) showed the
         * wrong UI state mid-sentence and started the commit watchdog on
         * the wrong edge. The demo keeps "user querying" through the
         * whole transcription. Reset the watchdog on every delta so a
         * long utterance can never trip the commit/thinking timeout. */
        if (data && s_chat_state >= CHAT_LISTENING) {
            lvgl_port_lock(0);
            ui_add_user_bubble((const char *)data);
            lvgl_port_unlock();
            s_timeout_cnt = 0;
        }
        break;

    case DOUBAO_EVT_TRANSCRIPT_DONE:
        if (data && s_chat_state >= CHAT_LISTENING) {
            lvgl_port_lock(0);
            ui_add_user_bubble((const char *)data);
            lvgl_port_unlock();
            /* Save user message to notes */
            notes_manager_save_message("user", (const char *)data, 0);
            /* End of speech = the real THINKING edge (demo semantics):
             * the server answers right after transcription.completed. */
            s_chat_state = CHAT_THINKING;
            s_timeout_cnt = 0;
            lvgl_port_lock(0);
            app_state_request(UI_STATE_THINKING);
            lvgl_port_unlock();
        }
        break;

    /* ── Output text: streaming bot reply ─────────────────────────────── */

    case DOUBAO_EVT_OUTPUT_TEXT_DELTA:
        if (data) {
            /* Strip [DEVICE:...] tags from streaming text before UI.
             * The server TTS reads the same text — tags on their own line
             * are less audible, but stripping from UI prevents visual
             * noise.  Tags are still present in the raw text passed to
             * the server for TTS; we cannot suppress server-side audio
             * for specific tokens. */
            static char s_delta_buf[512];
            size_t dlen = strlen((const char *)data);
            if (dlen >= sizeof(s_delta_buf)) dlen = sizeof(s_delta_buf) - 1;
            memcpy(s_delta_buf, data, dlen);
            s_delta_buf[dlen] = '\0';
            strip_device_tags(s_delta_buf);
            if (s_delta_buf[0] == '\0') break;  /* only tags, nothing to show */
            lvgl_port_lock(0);
            if (s_chat_state < CHAT_SPEAKING) {
                s_chat_state = CHAT_THINKING;
                app_set_state(UI_STATE_STREAMING);
            }
            ui_bot_bubble_append(s_delta_buf);
            lvgl_port_unlock();
            s_bot_has_text = true;
            s_timeout_cnt = 0;  /* reset commit timeout on any output */
        }
        break;

    case DOUBAO_EVT_OUTPUT_TEXT_DONE: {
        if (!data) break;
        strncpy(s_final_text, (const char *)data, sizeof(s_final_text) - 1);
        s_final_text[sizeof(s_final_text) - 1] = '\0';
        ESP_LOGI(TAG, "reply done: %.100s%s", s_final_text,
                 strlen(s_final_text) > 100 ? "..." : "");

        /* [DEVICE:] 语音控制解析 — 在 s_final_text 上原地执行，
         * parse_device_commands 会用 memmove 剥离 [DEVICE:...] 标签，
         * 这样后续保存到 notes 和显示到 UI 的文本都不含原始标签。 */
        int dev_cmds = parse_device_commands(s_final_text);
        if (dev_cmds > 0) {
            ESP_LOGI(TAG, "Executed %d device command(s)", dev_cmds);
        }

        /* Save assistant message (cleaned, without [DEVICE:] tags) */
        notes_manager_save_message("assistant", s_final_text, 0);

        lvgl_port_lock(0);
        if (!s_bot_has_text) {
            ui_bot_bubble_append(s_final_text);
        }
        lvgl_port_unlock();
        s_bot_has_text = false;
        break;
    }

    /* ── Audio pipeline ───────────────────────────────────────────────── */

    case DOUBAO_EVT_AUDIO_STARTED:
        /* Start playback task; mark speaking state */
        s_chat_state = CHAT_SPEAKING;
        s_audio_active = true;
        s_audio_delta_cnt = 0;
        /* Sequential-phase model (AIWatch_Ver2.0, proven on this board):
         * capture STOPS during playback. Concurrent mic read + speaker
         * write contended on the codec mutex until the capture task lost
         * every race ("mic read incomplete" floods), and the muted-silence
         * uplink collided with the response download on the ws client
         * lock ("Could not lock ws-client" → TX drops). The response runs
         * 2-20s, far under the server's ~60s audio-idle kill line, so
         * pausing the stream mid-response is safe. Interrupt = BOOT button
         * (local energy barge-in needs the mic; that's the AEC-stage
         * follow-up per spec §3.2). */
        doubao_set_uplink_muted(true);
        dbaudio_in_stop();
        s_timeout_cnt = 0;
        lvgl_port_lock(0);
        app_set_state(UI_STATE_TTS_PLAYING);
        lvgl_port_unlock();
        break;

    case DOUBAO_EVT_AUDIO_DELTA:
        /* Audio pushed to audio_out by doubao_voice.c dispatch();
         * just reset the speaking timeout */
        s_timeout_cnt = 0;
        s_audio_delta_cnt++;
        if ((s_audio_delta_cnt % 200) == 1) {
            ESP_LOGI(TAG, "audio delta #%lu",
                     (unsigned long)s_audio_delta_cnt);
        }
        break;

    case DOUBAO_EVT_AUDIO_DONE: {
        int status_code = data ? *(const int *)data : 0;
        ESP_LOGI(TAG, "audio done: status=%d", status_code);

        if (status_code == 20000002) {
            /* Exit intent → go idle immediately */
            ESP_LOGI(TAG, "exit intent received → idle");
            go_idle();
        } else {
            /* audio.done = the server finished SENDING — the ring may
             * still hold seconds of unplayed audio (burst delivery runs
             * ~2× real-time). Cutting playback here truncated the tail
             * of every response. Set the drain flag and stay in SPEAKING;
             * the tick moves to auto-listen once the ring runs dry. The
             * muted silence pump keeps the uplink alive meanwhile. */
            ESP_LOGI(TAG, "audio done — draining %d buffered samples",
                     dbaudio_out_ring_depth());
            s_audio_done_pending = true;
            dbaudio_out_drain_stop();
            s_timeout_cnt = 0;
        }
        break;
    }

    case DOUBAO_EVT_RESPONSE_DONE:
        /* Secondary terminator for TEXT-ONLY responses (no audio events at
         * all — in that case audio.done never fires). For spoken responses
         * this arrives after AUDIO_DONE already transitioned, so ignore. */
        ESP_LOGI(TAG, "response done (usage logged)");
        if (!s_audio_active) {
            enter_auto_listen();
        }
        break;

    case DOUBAO_EVT_SESSION_CLOSED:
        ESP_LOGI(TAG, "session closed (ack) — next wake opens a fresh one");
        /* The ack may arrive for OUR close (already IDLE → no-op) or for
         * a server-initiated close (error / TTS idle timeout mid-turn).
         * In the latter case the session is gone but the chat state and
         * the parked mic are still held — unwind now or the device stays
         * stuck and deaf until reboot. */
        if (s_chat_state != CHAT_IDLE) {
            go_idle();
        }
        break;

    case DOUBAO_EVT_INTERRUPTED:
        /* Arrives as transcription.started (user began speaking) or
         * response.canceled (our cancel acked). Only meaningful while the
         * server is talking — otherwise it is noise. */
        if (s_chat_state == CHAT_AUTO_LISTEN || s_chat_state == CHAT_SPEAKING) {
            ESP_LOGI(TAG, "server confirmed interrupt — back to listening");
            dbaudio_out_stop();
            s_chat_state = CHAT_LISTENING;
            /* Playback stopped → mic is safe to put back on the wire.
             * Server barge-in opens a new turn, so the commit latch
             * reopens — same as the local energy-based barge-in path.
             * Capture was parked during SPEAKING (sequential-phase model),
             * so restart it here. */
            dbaudio_in_reset_queue();
            vad_reset();
            s_committed_this_turn = false;
            s_speech_heard_this_turn = false;
            s_commit_pending = false;
            s_audio_done_pending = false;
            dbaudio_in_start();
            doubao_set_uplink_muted(false);
            lvgl_port_lock(0);
            /* app_set_state, NOT app_state_request: the legacy machine
             * state is stale (the doubao path bypasses it with direct
             * sets after LISTENING), so a request would be rejected. */
            app_set_state(UI_STATE_LISTENING);
            lvgl_port_unlock();
        }
        break;

    /* ── Error ────────────────────────────────────────────────────────── */

    case DOUBAO_EVT_ERROR: {
        const char *msg = data ? (const char *)data : "unknown error";
        ESP_LOGE(TAG, "Doubao error: %s", msg);
        error_log_add(ERR_SRC_DEVICE, ERR_SEV_ERROR, "Doubao: %.100s", msg);
        lvgl_port_lock(0);
        ui_set_status_message(msg);
        lvgl_port_unlock();
        /* The turn is dead — recover to IDLE the way the demo does after
         * an error (session teardown, fresh start). The wake word comes
         * back and the user retries by speaking again. Leaving the state
         * parked in ERROR only strands the mic and feeds the app's
         * consecutive-error deep-sleep counter. */
        go_idle();
        break;
    }

    default:
        ESP_LOGD(TAG, "unhandled event type %d", (int)type);
        break;
    }
}

/* ── VAD + interrupt check (called from app_tasks periodic tick) ──────── */

static uint32_t s_tick_log_cnt = 0;

void doubao_chat_tick(void)
{
    /* Periodic tick: ~100ms, called from main loop */
    s_tick_log_cnt++;
    if ((s_tick_log_cnt % 50) == 0) {  /* every ~5s */
        ESP_LOGD(TAG, "chat tick #%lu: state=%s timeout=%d idle=%d audio_delta=%lu",
                 (unsigned long)s_tick_log_cnt,
                 chat_state_name(s_chat_state),
                 s_timeout_cnt, s_idle_frame_cnt,
                 (unsigned long)s_audio_delta_cnt);
    }

    switch (s_chat_state) {

    case CHAT_LISTENING: {
        /* Commit actually transmitted (VAD end-of-speech; the send task's
         * frame callback set the flag) → COMMITTING. The 15s watchdog now
         * starts from the real commit edge, not the first transcript
         * delta — a long utterance can no longer trip it mid-speech. */
        if (s_commit_pending) {
            s_commit_pending = false;
            s_chat_state = CHAT_COMMITTING;
            s_timeout_cnt = 0;
            ESP_LOGI(TAG, "commit sent — waiting for response");
        }
        /* session.create must be answered. The turn buffers capture until
         * session.created arrives; if the server never answers (create
         * rejected or wedged), unwind instead of hanging in LISTENING
         * with a parked wake word. */
        if (s_waiting_session) {
            s_timeout_cnt++;
            if (s_timeout_cnt >= 50) {   /* 5s @ ~100ms/tick */
                ESP_LOGW(TAG, "session.created not received in 5s — aborting turn");
                error_log_add(ERR_SRC_DEVICE, ERR_SEV_WARNING,
                              "Doubao session.create timeout");
                go_idle();
            }
        }
        break;
    }

    case CHAT_THINKING: {
        /* 铁律10: 思考30s无output → error */
        s_timeout_cnt++;
        if (s_timeout_cnt >= THINKING_TIMEOUT_S * 10) {
            ESP_LOGW(TAG, "thinking timeout (%ds) — error", THINKING_TIMEOUT_S);
            error_log_add(ERR_SRC_DEVICE, ERR_SEV_WARNING, "Doubao thinking timeout");
            doubao_disconnect();
            doubao_connect();
            go_idle();
        }
        break;
    }

    case CHAT_AUTO_LISTEN: {
        /* Check for silence timeout → return to idle */
        s_idle_frame_cnt++;
        int idle_frames_needed = s_idle_timeout_s * 10;  /* 100ms per tick */
        if (s_idle_frame_cnt >= idle_frames_needed) {
            ESP_LOGI(TAG, "auto-listen idle timeout → idle");
            go_idle();
        }
        break;
    }

    case CHAT_COMMITTING: {
        /* Timeout: commit 后 15s 无 output → error */
        s_timeout_cnt++;
        if (s_timeout_cnt >= COMMIT_TIMEOUT_S * 10) {
            ESP_LOGW(TAG, "commit timeout (%ds) — reconnecting", COMMIT_TIMEOUT_S);
            error_log_add(ERR_SRC_DEVICE, ERR_SEV_WARNING, "Doubao commit timeout");
            doubao_disconnect();
            doubao_connect();
            go_idle();
        }
        break;
    }

    case CHAT_SPEAKING: {
        /* audio.done arrived and the drain has run the ring dry — the
         * spoken turn is truly over. Capture resumes so the follow-up
         * turn (auto-listen) can hear the user again. Reset VAD flags
         * BEFORE starting capture to prevent spurious commits from stale
         * s_speech_heard_this_turn left over from the previous turn. */
        if (s_audio_done_pending && !dbaudio_out_is_playing()) {
            s_audio_done_pending = false;
            vad_reset();
            s_committed_this_turn = false;
            s_speech_heard_this_turn = false;
            dbaudio_in_reset_queue();
            dbaudio_in_start();
            enter_auto_listen();
            break;
        }

        /* Timeout: 播报中 60s 无 audio delta → stop.
         * The demo Python client has NO speak timeout — it plays until
         * session.done.  Song responses can be 30s+, so 5s was killing
         * playback mid-phrase.  Use 60s as a safety net only. */
        s_timeout_cnt++;
        if (s_timeout_cnt >= SPEAK_TIMEOUT_S * 10) {
            ESP_LOGW(TAG, "speak timeout (%ds) — stopping", SPEAK_TIMEOUT_S);
            dbaudio_out_stop();
            enter_auto_listen();
        }

        /* Interrupt detection: mic RMS > play RMS + 6dB for 80ms.
         * Capture is parked during SPEAKING (sequential-phase model), so
         * vad_rms() holds a stale value — skip the check entirely until
         * the AEC stage brings the mic back during playback. */
        if (!dbaudio_in_is_running()) {
            s_interrupt_hold_frames = 0;
            break;
        }
        float mic_rms = vad_rms();
        float play_rms = dbaudio_out_current_rms();
        if (play_rms > 100.0f && mic_rms > play_rms * 2.0f) {
            /* 6dB ≈ ×2 in linear */
            s_interrupt_hold_frames++;
            if (s_interrupt_hold_frames >= 2) {  /* 200ms (2 ticks) > 80ms */
                ESP_LOGI(TAG, "local interrupt: mic_rms=%.0f > play_rms=%.0f",
                         mic_rms, play_rms);
                doubao_interrupt();
                dbaudio_out_stop();
                s_chat_state = CHAT_LISTENING;
                /* Playback stopped → mic is safe to put back on the wire.
                 * Barge-in opens a new turn, so the commit latch reopens. */
                dbaudio_in_reset_queue();
                vad_reset();
                s_committed_this_turn = false;
                doubao_set_uplink_muted(false);
                s_interrupt_hold_frames = 0;
                lvgl_port_lock(0);
                app_state_request(UI_STATE_LISTENING);
                lvgl_port_unlock();
            }
        } else {
            s_interrupt_hold_frames = 0;
        }
        break;
    }

    default:
        break;
    }
}

/* ── Cancel turn (BOOT single-press during a dialogue) ────────────────── */

void doubao_chat_cancel(void)
{
    if (s_chat_state == CHAT_IDLE) {
        return;
    }
    ESP_LOGI(TAG, "cancel: BOOT pressed in state=%s", chat_state_name(s_chat_state));
    go_idle();
    /* 咚叮：退出聆听的反馈音（Ver2.0 同款）。go_idle 已停采集/关会话/
     * 恢复唤醒词，此刻扬声器空闲。 */
    play_feedback_sound(snd_rec_stop, snd_rec_stop_len);
}
