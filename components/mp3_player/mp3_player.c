/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * MP3 Player implementation — wraps esp_audio_simple_player for SD card MP3 playback
 */

#include "mp3_player.h"
#include "board.h"
#include "wake_word.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

static const char *TAG = "mp3_player";

static esp_asp_handle_t s_handle = NULL;
static bool s_playing = false;
static bool s_hw_configured = false;
static mp3_playback_state_t s_state = MP3_STATE_IDLE;
static char s_current_file[MP3_FILE_NAME_MAX] = {0};
static mp3_state_cb_t s_state_cb = NULL;
static mp3_completion_cb_t s_completion_cb = NULL;
static uint32_t s_duration_sec = 0;
static bool s_async_error = false;

/* Saved audio config for restoring after MP3 playback */
static int s_saved_volume = 60;
static uint32_t s_saved_sample_rate = 16000;

/* ── Audio output callback from esp_audio_simple_player ─── */
static int out_data_callback(uint8_t *data, int data_size, void *ctx)
{
    (void)ctx;
    size_t samples = data_size / sizeof(int16_t);
    size_t written = 0;
    esp_err_t ret = board_audio_play((const int16_t *)data, samples, &written);
    
    /* Log if write failed or incomplete (indicates buffer underrun or reconfig conflict) */
    if (ret != ESP_OK || written != samples) {
        static uint32_t underrun_count = 0;
        static uint32_t last_log_tick = 0;
        uint32_t current_tick = xTaskGetTickCount();
        
        underrun_count++;
        
        /* Log first occurrence and every 50th, but not more than once per second */
        if (underrun_count <= 3 || (underrun_count % 50 == 1 && (current_tick - last_log_tick) > 100)) {
            last_log_tick = current_tick;
            ESP_LOGW(TAG, "Audio write issue #%lu: ret=%d, requested=%d, written=%d (%.1f%% loss)", 
                     (unsigned long)underrun_count, ret, (int)samples, (int)written,
                     (samples > 0) ? (100.0f * (samples - written) / samples) : 0.0f);
        }
    }
    
    return (ret == ESP_OK) ? 0 : -1;
}

/* ── Player event callback ──────────────────────────────── */
static int event_callback(esp_asp_event_pkt_t *event, void *ctx)
{
    (void)ctx;

    if (event->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO) {
        esp_asp_music_info_t info = {0};
        memcpy(&info, event->payload, event->payload_size);
        ESP_LOGI(TAG, "Music info: rate=%d, ch=%d, bits=%d, bitrate=%d",
                 info.sample_rate, info.channels, info.bits, info.bitrate);

        /* Check if reconfiguration is needed */
        if (!s_hw_configured) {
            ESP_LOGW(TAG, "Hardware not configured yet, reconfiguring...");
            board_audio_reconfig(info.sample_rate, info.channels);
        } else if (info.sample_rate != 48000 || info.channels != 2) {
            ESP_LOGW(TAG, "Sample rate mismatch: expected 48kHz/2ch, got %dHz/%dch, reconfiguring...",
                     info.sample_rate, info.channels);
            board_audio_reconfig(info.sample_rate, info.channels);
        } else {
            ESP_LOGI(TAG, "I2S already configured for %d Hz, %d ch (no reconfig needed)",
                     info.sample_rate, info.channels);
        }
        s_hw_configured = true;

        /* Estimate duration from file size and bitrate (if available) */
        if (info.bitrate > 0 && strlen(s_current_file) > 0) {
            char full_path[300];
            struct stat st;
            snprintf(full_path, sizeof(full_path), "/sdcard/%s", s_current_file);
            if (stat(full_path, &st) == 0) {
                s_duration_sec = (uint32_t)((st.st_size * 8) / (info.bitrate * 1000));
            }
        }
    } else if (event->type == ESP_ASP_EVENT_TYPE_STATE) {
        esp_asp_state_t st = 0;
        memcpy(&st, event->payload, event->payload_size);
        ESP_LOGI(TAG, "Player state: %d (%s)", st, esp_audio_simple_player_state_to_str(st));

        switch (st) {
        case ESP_ASP_STATE_RUNNING:
            s_state = MP3_STATE_PLAYING;
            s_playing = true;
            break;
        case ESP_ASP_STATE_PAUSED:
            s_state = MP3_STATE_PAUSED;
            break;
        case ESP_ASP_STATE_STOPPED:
            s_state = MP3_STATE_STOPPED;
            s_playing = false;
            break;
        case ESP_ASP_STATE_FINISHED:
            s_state = MP3_STATE_IDLE;
            s_playing = false;
            /* Restore WiFi power saving mode */
            esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
            /* Restore audio for TTS/wake word */
            board_audio_reconfig(s_saved_sample_rate, 1);
            s_hw_configured = false;
            board_audio_set_volume(s_saved_volume);
            
            /* CRITICAL: Add delay to ensure I2S hardware is fully stable
             * before resuming wake word detection. Without this delay,
             * wake word task may fail to read audio properly. */
            vTaskDelay(pdMS_TO_TICKS(200));
            
            /* CRITICAL: Re-open ADC device to ensure proper audio capture.
             * During MP3 playback, ADC was left open but I2S config changed.
             * We need to close and re-open it with correct sample rate. */
            esp_err_t adc_ret = board_audio_reopen_adc();
            if (adc_ret != ESP_OK) {
                ESP_LOGW(TAG, "ADC reopen failed: %d (wake word may not work)", adc_ret);
            } else {
                ESP_LOGI(TAG, "ADC reopened successfully for wake word detection");
            }
            
            ESP_LOGI(TAG, "Resuming wake word detection after MP3 playback");
            wake_word_resume();
            if (s_completion_cb) s_completion_cb();
            break;
        case ESP_ASP_STATE_ERROR:
            ESP_LOGE(TAG, "Player error - stopping");
            s_state = MP3_STATE_IDLE;
            s_playing = false;
            /* Restore WiFi power saving mode */
            esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
            if (s_hw_configured) {
                board_audio_reconfig(s_saved_sample_rate, 1);
                s_hw_configured = false;
                board_audio_set_volume(s_saved_volume);
                vTaskDelay(pdMS_TO_TICKS(200));
                board_audio_reopen_adc();
            }
            wake_word_resume();
            /* Notify external callback so UI/app state can exit PLAYING_MP3 */
            s_async_error = true;
            if (s_completion_cb) s_completion_cb();
            break;
        default:
            break;
        }

        if (s_state_cb) s_state_cb(s_state, s_current_file);
    }
    return 0;
}

/* ── Public API ─────────────────────────────────────────── */

esp_err_t mp3_player_init(void)
{
    if (s_handle) return ESP_OK;

    esp_asp_cfg_t cfg = {
        .in.cb = NULL,
        .in.user_ctx = NULL,
        .out.cb = out_data_callback,
        .out.user_ctx = NULL,
        .task_prio = 10,  /* Balanced priority: high enough for smooth playback but not too aggressive */
        .task_stack = 8192,  /* Larger stack for MP3 decoding */
        .task_core = 0,  /* Run on core 0 */
        .task_stack_in_ext = true,  /* Use PSRAM for stack to save internal RAM */
    };

    ESP_RETURN_ON_ERROR(esp_audio_simple_player_new(&cfg, &s_handle), TAG, "Player create failed");
    ESP_RETURN_ON_ERROR(esp_audio_simple_player_set_event(s_handle, event_callback, NULL),
                        TAG, "Set event callback failed");

    /* Save current audio config */
    board_audio_get_volume(&s_saved_volume);
    s_saved_sample_rate = 16000;

    ESP_LOGI(TAG, "MP3 player initialized (prio=12, stack=8KB)");
    return ESP_OK;
}

esp_err_t mp3_player_play(const char *filename)
{
    if (!s_handle) return ESP_ERR_INVALID_STATE;
    if (!filename || !filename[0]) return ESP_ERR_INVALID_ARG;

    /* Stop any existing playback */
    if (s_playing) {
        esp_audio_simple_player_stop(s_handle);
        s_playing = false;
    }

    /* Save current state before switching to MP3 mode */
    board_audio_get_volume(&s_saved_volume);

    /* Pause wake word to free I2S for MP3 sample rate change */
    wake_word_pause();

    /* Immediately reconfigure I2S after pausing wake word.
     * This gives more time for hardware to stabilize before playback starts. */
    board_audio_reconfig(48000, 2);  /* Default to 48kHz stereo */
    s_hw_configured = true;
    ESP_LOGI(TAG, "I2S pre-configured to 48kHz stereo (waiting for stabilization)");

    /* Wait for I2S hardware to fully stabilize after reconfiguration */
    vTaskDelay(pdMS_TO_TICKS(20));  /* 20ms stabilization period */

    /* Build file URL */
    char url[256];
    snprintf(url, sizeof(url), "file://sdcard/mp3/%s", filename);

    /* Store current filename */
    strncpy(s_current_file, filename, sizeof(s_current_file) - 1);
    s_current_file[sizeof(s_current_file) - 1] = '\0';
    s_duration_sec = 0;
    /* NOTE: s_hw_configured stays true — it was already set above after the
     * initial board_audio_reconfig(48000, 2). Resetting it to false here would
     * cause the MUSIC_INFO event callback to attempt a second reconfig while
     * the GMF pipeline is already running, which fails with I2S sample rate
     * conflict and 100% audio loss. */

    ESP_LOGI(TAG, "Playing: %s", url);

    /* Log memory status before MP3 playback */
    uint32_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Memory before MP3: internal=%lu bytes, PSRAM=%lu bytes",
             (unsigned long)free_internal, (unsigned long)free_psram);

    /* Check for critically low memory */
    if (free_internal < 100000) {  /* Less than 100KB internal RAM */
        ESP_LOGW(TAG, "WARNING: Low internal RAM (%lu bytes) before MP3 playback",
                 (unsigned long)free_internal);
    }

    /* Disable WiFi power saving during MP3 playback to reduce interference */
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* I2S already configured above, no need to reconfigure here */

    /* Mute while starting to avoid pops */
    board_audio_mute(true);

    esp_err_t ret = esp_audio_simple_player_run(s_handle, url, NULL);
    if (ret != 0) {
        ESP_LOGE(TAG, "Player run failed: %d", ret);
        board_audio_mute(false);
        wake_word_resume();
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);  /* Restore WiFi PS on error */
        s_state = MP3_STATE_IDLE;
        s_current_file[0] = '\0';
        return ESP_FAIL;
    }

    /* Wait for I2S and codec to fully stabilize before unmuting.
     * This prevents initial glitch/noise from being heard. */
    vTaskDelay(pdMS_TO_TICKS(50));  /* 50ms stabilization period */

    /* Unmute — audio will start when pipeline delivers data */
    board_audio_mute(false);

    s_state = MP3_STATE_PLAYING;
    s_playing = true;
    if (s_state_cb) s_state_cb(s_state, s_current_file);

    if (s_async_error) {
        s_async_error = false;
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t mp3_player_stop(void)
{
    if (!s_handle) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Stopping playback");

    esp_audio_simple_player_stop(s_handle);
    s_playing = false;

    /* Restore WiFi power saving mode */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    /* Restore audio config for TTS/wake word */
    if (s_hw_configured) {
        board_audio_reconfig(s_saved_sample_rate, 1);
        s_hw_configured = false;
    }
    board_audio_set_volume(s_saved_volume);
    board_audio_mute(false);
    wake_word_resume();

    s_state = MP3_STATE_IDLE;
    s_current_file[0] = '\0';
    if (s_state_cb) s_state_cb(s_state, "");

    return ESP_OK;
}

esp_err_t mp3_player_pause(void)
{
    if (!s_handle) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Pausing playback");
    board_audio_mute(true);

    esp_err_t ret = esp_audio_simple_player_pause(s_handle);
    if (ret == 0) {
        s_state = MP3_STATE_PAUSED;
        if (s_state_cb) s_state_cb(s_state, s_current_file);
    }
    return (ret == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t mp3_player_resume(void)
{
    if (!s_handle) return ESP_ERR_INVALID_STATE;

    esp_asp_state_t cur = ESP_ASP_STATE_NONE;
    esp_audio_simple_player_get_state(s_handle, &cur);
    if (cur != ESP_ASP_STATE_PAUSED) {
        ESP_LOGW(TAG, "Cannot resume: state=%d (not PAUSED)", cur);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Resuming playback");
    board_audio_mute(false);

    esp_err_t ret = esp_audio_simple_player_resume(s_handle);
    if (ret == 0) {
        s_state = MP3_STATE_PLAYING;
        if (s_state_cb) s_state_cb(s_state, s_current_file);
    }
    return (ret == 0) ? ESP_OK : ESP_FAIL;
}

mp3_playback_state_t mp3_player_get_state(void)
{
    return s_state;
}

const char *mp3_player_get_current_file(void)
{
    return s_current_file;
}

uint32_t mp3_player_get_position_sec(void)
{
    if (!s_handle || !s_playing) return 0;

    esp_asp_state_t st = ESP_ASP_STATE_NONE;
    esp_audio_simple_player_get_state(s_handle, &st);
    if (st != ESP_ASP_STATE_RUNNING && st != ESP_ASP_STATE_PAUSED) return 0;

    /* Position tracking not available in this version */
    return 0;
}

uint32_t mp3_player_get_duration_sec(void)
{
    return s_duration_sec;
}

uint16_t mp3_player_scan_sd(const char *directory, char file_names[][MP3_FILE_NAME_MAX], uint16_t max_files)
{
    ESP_LOGI(TAG, "Scanning: %s", directory);
    DIR *dir = opendir(directory);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s (errno=%d)", directory, errno);
        return 0;
    }

    uint16_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_files) {
        if (entry->d_type == DT_REG) {
            char *ext = strrchr(entry->d_name, '.');
            if (ext && strcasecmp(ext, ".mp3") == 0) {
                strncpy(file_names[count], entry->d_name, MP3_FILE_NAME_MAX - 1);
                file_names[count][MP3_FILE_NAME_MAX - 1] = '\0';

                char full_path[300];
                struct stat st;
                snprintf(full_path, sizeof(full_path), "/sdcard/mp3/%s", entry->d_name);
                if (stat(full_path, &st) == 0) {
                    ESP_LOGI(TAG, "  [%d] %s (%ld bytes)", count, entry->d_name, (long)st.st_size);
                }
                count++;
            }
        }
    }
    closedir(dir);

    ESP_LOGI(TAG, "Found %d MP3 file(s)", count);
    return count;
}

esp_err_t mp3_player_scan_sd_dynamic(const char *directory, char ***out_files, uint16_t *out_count)
{
    if (!directory || !out_files || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Dynamic scanning: %s", directory);
    DIR *dir = opendir(directory);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s (errno=%d)", directory, errno);
        return ESP_ERR_NOT_FOUND;
    }

    /* First pass: count files */
    uint16_t total_count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            char *ext = strrchr(entry->d_name, '.');
            if (ext && strcasecmp(ext, ".mp3") == 0) {
                total_count++;
            }
        }
    }
    closedir(dir);

    if (total_count == 0) {
        ESP_LOGI(TAG, "No MP3 files found");
        *out_files = NULL;
        *out_count = 0;
        return ESP_OK;
    }

    /* Allocate array of pointers */
    char **files = calloc(total_count, sizeof(char *));
    if (!files) {
        ESP_LOGE(TAG, "Failed to allocate memory for %d files", total_count);
        return ESP_ERR_NO_MEM;
    }

    /* Second pass: collect filenames */
    dir = opendir(directory);
    if (!dir) {
        free(files);
        return ESP_ERR_NOT_FOUND;
    }

    uint16_t idx = 0;
    while ((entry = readdir(dir)) != NULL && idx < total_count) {
        if (entry->d_type == DT_REG) {
            char *ext = strrchr(entry->d_name, '.');
            if (ext && strcasecmp(ext, ".mp3") == 0) {
                files[idx] = strdup(entry->d_name);
                if (!files[idx]) {
                    ESP_LOGE(TAG, "Failed to duplicate filename");
                    /* Clean up already allocated strings */
                    for (int i = 0; i < idx; i++) {
                        free(files[i]);
                    }
                    free(files);
                    closedir(dir);
                    return ESP_ERR_NO_MEM;
                }

                char full_path[300];
                struct stat st;
                snprintf(full_path, sizeof(full_path), "/sdcard/mp3/%s", entry->d_name);
                if (stat(full_path, &st) == 0) {
                    ESP_LOGI(TAG, "  [%d] %s (%ld bytes)", idx, entry->d_name, (long)st.st_size);
                }
                idx++;
            }
        }
    }
    closedir(dir);

    *out_files = files;
    *out_count = idx;
    ESP_LOGI(TAG, "Dynamically found %d MP3 file(s)", idx);
    return ESP_OK;
}

void mp3_player_set_state_cb(mp3_state_cb_t cb)
{
    s_state_cb = cb;
}

void mp3_player_set_completion_cb(mp3_completion_cb_t cb)
{
    s_completion_cb = cb;
}
