/*
 * SPDX-FileCopyrightText: 2024-2026 AIWatch Contributors
 * SPDX-License-Identifier: MIT
 *
 * Centralized state machine with resource arbitration.
 *
 * All state transitions must go through app_state_request(), which:
 *  1. Validates the transition (rejects impossible state changes)
 *  2. Acquires required hardware resources (I2S TX, I2S RX)
 *  3. Preempts lower-priority state holders when necessary
 *  4. Runs pre/post-transition hooks (wake word pause)
 *  5. Applies the UI state via app_set_state()
 *
 * Resources:
 *   RES_AUDIO_OUT (I2S TX) — needed by TTS, MP3, beeps
 *   RES_AUDIO_IN  (I2S RX) — needed by wake word, voice recording
 *
 * Priority (for preemption, highest first):
 *   1. LISTENING  (voice recording)
 *   2. PLAYING_MP3
 *   3. TTS_PLAYING
 */

#pragma once

#include "esp_err.h"
#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Resources that the state machine arbitrates */
typedef enum {
    RES_AUDIO_OUT,   /* I2S TX (speaker) */
    RES_AUDIO_IN,    /* I2S RX (microphone) */
    RES_COUNT
} app_resource_t;

/**
 * @brief Request a state transition with resource arbitration.
 *
 * Validates the transition, acquires required resources (possibly preempting
 * lower-priority holders), runs pre-transition hooks, and applies the state.
 *
 * @param target  Desired UI state
 * @return ESP_OK on success
 *         ESP_ERR_INVALID_STATE if transition is not allowed
 *         ESP_ERR_BUSY if resources held by a higher-priority owner
 */
esp_err_t app_state_request(ui_state_t target);

/**
 * @brief Initialize the state machine.
 * Must be called once at boot, before any state transitions.
 */
void app_state_machine_init(void);

/**
 * @brief Get the current state (thread-safe).
 */
ui_state_t app_state_current(void);

/**
 * @brief Release all resources held by a state.
 * Called automatically on transition; also useful for forced cleanup.
 */
void app_state_release(ui_state_t from_state);

/**
 * @brief Force a state back to IDLE, releasing all its resources.
 * Used for preemption and error recovery.
 */
void app_state_force_idle(ui_state_t state);

/**
 * @brief Check if a resource is currently owned.
 */
bool app_resource_is_owned(app_resource_t res);

/**
 * @brief Get owner state of a resource (0 = UI_STATE_BOOT = unowned).
 */
ui_state_t app_resource_owner(app_resource_t res);

#ifdef __cplusplus
}
#endif
