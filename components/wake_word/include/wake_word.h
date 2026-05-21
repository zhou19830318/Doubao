/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * Wake word detection using ESP-SR.
 * Supports WakeNet ("Hi ESP") or MultiNet (custom phrase, default "Hi Clawy").
 */

#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize wake word detection.
 * Loads model from SPIFFS "model" partition.
 * Engine selected via menuconfig (WakeNet or MultiNet).
 * @return ESP_OK on success
 */
esp_err_t wake_word_init(void);

/**
 * Start wake word detection task.
 * Continuously reads mic audio and feeds to detection engine.
 * Sets event_bit in event_group when wake word detected.
 */
esp_err_t wake_word_start(EventGroupHandle_t event_group, EventBits_t event_bit);

/** Pause detection (e.g., during recording or TTS playback) */
void wake_word_pause(void);

/** Resume detection */
void wake_word_resume(void);

/** Stop and free resources */
void wake_word_stop(void);

/** Check if wake word detection is initialized and active */
bool wake_word_is_running(void);

/** Get the configured wake word phrase (e.g., "Hi ESP" or "Hi Clawy") */
const char *wake_word_get_phrase(void);

#ifdef __cplusplus
}
#endif
