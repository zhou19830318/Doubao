/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * Voice chat — record → STT (DashScope streaming) → OpenClaw
 */

#include "voice_chat.h"
#include "app_state.h"
#include "recording_sounds.h"
#include "settings.h"

#include "board.h"
#include "openclaw_client.h"
#include "stt_client.h"
#include "ui.h"
#include "error_log.h"
#include "notes_manager.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "voice_chat";

/* Helper: play a short PCM sound through speaker */
static void play_feedback_sound(const int16_t *pcm, size_t len)
{
    size_t written = 0;
    size_t pos = 0;
    while (pos < len) {
        size_t chunk = len - pos;
        if (chunk > 1024) chunk = 1024;
        board_audio_play(pcm + pos, chunk, &written);
        pos += written;
    }
}

/* Helper: play a fading beep for countdown (5s) */
static void play_countdown_beep(int step)
{
    /* Use a simple square wave beep (150ms) */
    const int duration_ms = 150;
    const int samples = (BOARD_AUDIO_SAMPLE_RATE * duration_ms) / 1000;
    /* Allocate in SPIRAM for safety with large buffers, though this is small */
    int16_t *beep = heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!beep) return;

    /* Fading amplitude: 28000 down to 8000 over 5 steps (Max is 32767) */
    int16_t amp = 28000 - (step * 4000);
    for (int i = 0; i < samples; i++) {
        /* Square wave at 1000Hz: toggle every (16000/2000) = 8 samples */
        beep[i] = ((i / 8) % 2) ? amp : -amp;
    }

    size_t written;
    ESP_LOGI("voice_chat", "Playing countdown beep step %d (amp=%d, samples=%d)", step, (int)amp, samples);
    
    /* Before playing, ensure we are not holding any mutex that might block it, 
     * but board_audio_play handles its own mutex. */
    esp_err_t ret = board_audio_play(beep, samples, &written);
    if (ret != ESP_OK) {
        ESP_LOGE("voice_chat", "Beep play failed: %d", ret);
    }
    free(beep);
}

void voice_chat_start(void)
{
    if (openclaw_get_state() != OPENCLAW_STATE_CONNECTED) {
        ESP_LOGW(TAG, "OpenClaw not connected");
        app_set_state(UI_STATE_ERROR);
        ui_set_status_message("Not connected");
        vTaskDelay(pdMS_TO_TICKS(2000));
        app_set_state(UI_STATE_IDLE);
        return;
    }

    const settings_t *cfg = settings_get();

    /* Play start sound before recording */
    play_feedback_sound(snd_rec_start, snd_rec_start_len);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Start DashScope STT session (WebSocket connect) */
    stt_reset();
    esp_err_t stt_start_err = stt_start();
    if (stt_start_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start STT session");
        error_log_add(ERR_SRC_STT, ERR_SEV_ERROR, "STT session start failed");
        app_set_state(UI_STATE_IDLE);
        return;
    }

    g_recording = true;
    app_set_state(UI_STATE_LISTENING);

    /* Clear any existing widget when starting new conversation */

    /* Allocate recording buffer in PSRAM (used for local VAD only,
     * audio is also streamed to STT via stt_upload_chunk) */
    /* Max recording time: normally 15s, but in continuous mode we need 15s + 5s */
    int max_record_seconds = cfg->max_record_seconds;
    if (g_continue_listening) {
        max_record_seconds = 25; // Allow 15s silence + 5s countdown + some buffer
    }
    const int max_samples = BOARD_AUDIO_SAMPLE_RATE * max_record_seconds;
    int16_t *audio_buf = heap_caps_malloc(max_samples * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!audio_buf) {
        ESP_LOGE(TAG, "Audio buffer alloc failed");
        error_log_add(ERR_SRC_DEVICE, ERR_SEV_ERROR, "Audio buffer alloc failed");
        g_recording = false;
        stt_reset();
        app_set_state(UI_STATE_IDLE);
        return;
    }

    /* Record with silence detection + wheel-press-to-stop */
    const int chunk = 512;
    const int silence_chunks = (cfg->silence_timeout_ms * BOARD_AUDIO_SAMPLE_RATE) / (chunk * 1000);
    int silent_count = 0;
    int speech_chunks = 0;
    bool had_speech = false;
    size_t total_read = 0;

    /* Adaptive noise floor: calibrate from audio after I2S stabilizes */
    const int skip_chunks = 4;      /* Skip first 4 chunks (~128ms) — I2S startup noise */
    const int calibration_chunks = 16;
    int noise_floor = 0;
    int chunk_index = 0;
    int calibration_count = 0;
    int64_t calibration_sum = 0;
    int64_t calibration_dc_sum = 0;  /* Sum of raw samples for DC offset */
    int calibration_dc_count = 0;
    int32_t dc_offset = 0;           /* PDM mic DC bias (subtracted from all samples) */
    /* Effective threshold: max(configured, noise_floor * 2.5) */
    int effective_threshold = cfg->silence_threshold;
    /* Continuous conversation: 15s initial silence timeout */
    const int no_speech_timeout_ms = 15000;
    const int no_speech_max_chunks = (no_speech_timeout_ms * BOARD_AUDIO_SAMPLE_RATE) / (chunk * 1000);
    int countdown_step = 0;
    int64_t last_beep_ms = 0;

    ESP_LOGI(TAG, "Recording + streaming to DashScope (silence=%dms thresh=%d continuous=15s)...",
             cfg->silence_timeout_ms, cfg->silence_threshold);

    while (total_read < (size_t)max_samples) {
        EventBits_t ev = xEventGroupGetBits(g_app_events);
        if (ev & CANCEL_BIT) {
            xEventGroupClearBits(g_app_events, CANCEL_BIT);
            ESP_LOGI(TAG, "Recording cancelled by user (had_speech=%d)", had_speech);
            g_recording = false;
            g_continue_listening = false; // User cancelled - stop continuous mode
            play_feedback_sound(snd_rec_stop, snd_rec_stop_len);
            free(audio_buf);
            stt_reset();
            app_set_state(UI_STATE_IDLE);
            return;
        }

        size_t to_read = (size_t)chunk;
        if (total_read + to_read > (size_t)max_samples) to_read = max_samples - total_read;
        size_t read_now = 0;
        if (board_audio_record(audio_buf + total_read, to_read, &read_now) != ESP_OK) break;

        /* Stream chunk to STT immediately (even before VAD processing) */
        stt_upload_chunk(audio_buf + total_read, read_now);

        int64_t sum_sq = 0;
        for (size_t i = 0; i < read_now; i++) {
            int32_t s = audio_buf[total_read + i] - dc_offset;
            sum_sq += s * s;
        }
        int rms = read_now > 0 ? (int)sqrtf((float)(sum_sq / read_now)) : 0;

        total_read += read_now;
        chunk_index++;

        /* Skip first few chunks — I2S startup noise */
        if (chunk_index <= skip_chunks) {
            continue;
        }

        /* Noise floor calibration phase */
        if (calibration_count < calibration_chunks) {
            /* Accumulate DC offset from raw samples */
            for (size_t i = total_read - read_now; i < total_read; i++) {
                calibration_dc_sum += audio_buf[i];
                calibration_dc_count++;
            }
            calibration_sum += rms;
            calibration_count++;
            if (calibration_count == calibration_chunks) {
                /* Compute DC offset = mean of all calibration samples */
                dc_offset = (int32_t)(calibration_dc_sum / calibration_dc_count);
                /* Recompute noise floor RMS with DC offset removed */
                int64_t recalc_sum = 0;
                int recalc_n = 0;
                for (int c = 0; c < calibration_chunks; c++) {
                    size_t start = (skip_chunks + c) * chunk;
                    size_t end = start + chunk;
                    if (end > total_read) end = total_read;
                    int64_t sq = 0;
                    for (size_t i = start; i < end; i++) {
                        int32_t v = audio_buf[i] - dc_offset;
                        sq += v * v;
                    }
                    int n = (int)(end - start);
                    if (n > 0) {
                        recalc_sum += (int)sqrtf((float)(sq / n));
                        recalc_n++;
                    }
                }
                noise_floor = recalc_n > 0 ? (int)(recalc_sum / recalc_n) : 0;
                /* Effective threshold: noise_floor * 2.5, but at least 300 and at most 800 */
                int adaptive = (noise_floor * 5) / 2;
                if (adaptive < 300) adaptive = 300;
                if (adaptive > 800) adaptive = 800;
                effective_threshold = adaptive;
                ESP_LOGI(TAG, "DC offset: %d | Noise floor: %d → threshold: %d",
                         (int)dc_offset, noise_floor, effective_threshold);
            }
            continue;  /* Don't do speech detection during calibration */
        }

        if (rms > effective_threshold) {
            speech_chunks++;
            /* Require at least 3 consecutive speech chunks (~100ms) to trigger speech detection.
             * This helps filter out short noise spikes. */
            if (speech_chunks >= 3) {
                had_speech = true;
                countdown_step = 0; // Reset countdown if speech starts
            }
            silent_count = 0;
        } else {
            speech_chunks = 0;
            silent_count++;
            if (had_speech && silent_count >= silence_chunks) {
                ESP_LOGI(TAG, "Silence detected after speech (rms=%d < %d), stopping",
                         rms, effective_threshold);
                break;
            }
            /* No-speech timeout: handle 15s + 5s countdown */
            if (!had_speech && silent_count >= no_speech_max_chunks) {
                int64_t now_ms = esp_timer_get_time() / 1000;
                if (countdown_step == 0) {
                    ESP_LOGI(TAG, "No speech for 15s, starting 5s countdown...");
                    countdown_step = 1;
                    last_beep_ms = now_ms;
                    
                    /* CRITICAL: Pause audio recording before playing beep to avoid
                     * capturing the beep sound in STT audio. The beep would interfere
                     * with speech recognition and cause empty results. */
                    stt_pause_upload();
                    play_countdown_beep(countdown_step);
                    vTaskDelay(pdMS_TO_TICKS(200));  /* Wait for beep to finish (150ms + margin) */
                    stt_resume_upload();
                } else if (now_ms - last_beep_ms >= 1000) {
                    countdown_step++;
                    last_beep_ms = now_ms;
                    if (countdown_step > 5) {
                        ESP_LOGI(TAG, "Countdown finished, aborting recording");
                        g_continue_listening = false; // Exit continuous mode
                        break;
                    }
                    
                    /* Pause upload during beep playback */
                    stt_pause_upload();
                    play_countdown_beep(countdown_step);
                    vTaskDelay(pdMS_TO_TICKS(200));  /* Wait for beep to finish (150ms + margin) */
                    stt_resume_upload();
                }
            }
        }
    }
    g_recording = false;

    /* Play stop sound */
    play_feedback_sound(snd_rec_stop, snd_rec_stop_len);

    /* Remove DC offset from recorded audio for better STT accuracy */
    if (dc_offset != 0) {
        for (size_t i = 0; i < total_read; i++) {
            int32_t v = audio_buf[i] - dc_offset;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            audio_buf[i] = (int16_t)v;
        }
    }

    /* Stream recorded audio to DashScope STT (upload all chunks at once since
     * we recorded locally first for VAD, then upload the full buffer)
     * NOTE: We already stream during recording, but this ensures DC-corrected
     * tail (if any) is sent, though stt_finalize handles it.
     * stt_upload_chunk(audio_buf, total_read);
     */

    /* Log audio levels */
    int32_t min_val = 32767, max_val = -32768;
    int64_t sum_abs = 0;
    for (size_t i = 0; i < total_read; i++) {
        int16_t s = audio_buf[i];
        if (s < min_val) min_val = s;
        if (s > max_val) max_val = s;
        sum_abs += (s < 0) ? -s : s;
    }
    int avg_abs = total_read ? (int)(sum_abs / total_read) : 0;
    ESP_LOGI(TAG, "Recorded %d samples %.1fs (min=%d max=%d avg_abs=%d noise_floor=%d)",
             (int)total_read, (float)total_read / BOARD_AUDIO_SAMPLE_RATE,
             (int)min_val, (int)max_val, avg_abs, noise_floor);

    /* Don't send if too short (< 0.3s) */
    if (total_read < (size_t)(BOARD_AUDIO_SAMPLE_RATE * 0.3f)) {
        ESP_LOGW(TAG, "Recording too short, discarding");
        free(audio_buf);
        stt_reset();
        app_set_state(UI_STATE_IDLE);
        return;
    }

    free(audio_buf);

    app_set_state(UI_STATE_SENDING);
    ui_set_status_message("Transcribing...");

    /* Finalize STT — blocks until transcription result or timeout.
     * Even if local VAD didn't detect speech, the STT may have recognized it
     * (audio was streamed during recording). Trust STT result over local VAD. */
    char transcribed[STT_TEXT_MAX] = {0};
    esp_err_t stt_err = stt_finalize(transcribed, sizeof(transcribed));

    if (stt_err != ESP_OK || transcribed[0] == '\0') {
        /* STT returned nothing — only discard if local VAD also heard nothing */
        if (!had_speech) {
            ESP_LOGW(TAG, "No speech detected (avg_abs=%d, noise=%d), discarding", avg_abs, noise_floor);
            ui_set_status_message("No speech detected");
            vTaskDelay(pdMS_TO_TICKS(1500));
            app_set_state(UI_STATE_IDLE);
            return;
        }
        /* Local VAD heard speech but STT failed — still discard */
        ESP_LOGW(TAG, "STT failed despite detected speech, discarding");
        ui_set_status_message("No speech detected");
        vTaskDelay(pdMS_TO_TICKS(1000));
        app_set_state(UI_STATE_IDLE);
        return;
    }

    /* STT got a result — use it even if local VAD was unsure */
    if (!had_speech) {
        ESP_LOGI(TAG, "Local VAD missed speech but STT recognized: '%s'", transcribed);
    }

    /* Check for cancel during STT */
    EventBits_t cancel_ev = xEventGroupGetBits(g_app_events);
    if (cancel_ev & CANCEL_BIT) {
        xEventGroupClearBits(g_app_events, CANCEL_BIT);
        ESP_LOGI(TAG, "Cancelled during STT/send");
        app_set_state(UI_STATE_IDLE);
        return;
    }

    ESP_LOGI(TAG, "STT result: '%s'", transcribed);
    ui_set_status_message("Sending...");

    /* Save user message to notes */
    esp_err_t ret = notes_manager_save_message("user", transcribed, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save user message to notes: %s", esp_err_to_name(ret));
    }

    /* Check for pending camera image */
    uint8_t *jpeg = g_pending_jpeg;
    size_t jpeg_sz = g_pending_jpeg_size;
    g_pending_jpeg = NULL;
    g_pending_jpeg_size = 0;

    if (jpeg && jpeg_sz > 0) {
        ESP_LOGI(TAG, "Including camera image (%d bytes) with message", (int)jpeg_sz);
    }

    /* Send the transcribed text */
    const char *msg = transcribed;

    /* Send with or without image */
    if (jpeg && jpeg_sz > 0) {
        openclaw_chat_send_with_image(msg, jpeg, jpeg_sz, app_on_chat_response);
        free(jpeg);
    } else {
        openclaw_chat_send(msg, app_on_chat_response);
    }
}
