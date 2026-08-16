/*
 * SPDX-FileCopyrightText: 2024-2026 AIWatch Contributors
 * SPDX-License-Identifier: MIT
 *
 * Centralized state machine — validates transitions and arbitrates resources.
 */

#include "app_state_machine.h"
#include "app_state.h"
#include "wake_word.h"
#include "board.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "state_machine";

static SemaphoreHandle_t s_state_mutex = NULL;
static ui_state_t s_current_state = UI_STATE_BOOT;
static ui_state_t s_resource_owner[RES_COUNT] = {0};

/* ── Transition table ────────────────────────────────────────────
 * For each state, bitmask of states it can transition TO.
 * BIT(n) = allowed target for state n.
 * All unlisted transitions are rejected.
 * Special: UI_STATE_BOOT (value 2) can go to CONNECTING or directly to IDLE.
 *           UI_STATE_CONNECTING (value 3) can go to IDLE or ERROR.
 *           Any state can transition to ERROR (unspecified = allowed).
 */
#define STATE_MASK(s) (1ULL << (s))

enum {
    ST_BOOT       = UI_STATE_BOOT,       /* 2 */
    ST_CONNECTING = UI_STATE_CONNECTING,  /* 3 */
    ST_IDLE       = UI_STATE_IDLE,        /* 4 */
    ST_LISTENING  = UI_STATE_LISTENING,   /* 5 */
    ST_SENDING    = UI_STATE_SENDING,     /* 6 */
    ST_THINKING   = UI_STATE_THINKING,    /* 7 */
    ST_STREAMING  = UI_STATE_STREAMING,   /* 8 */
    ST_RESPONSE   = UI_STATE_RESPONSE,    /* 9 */
    ST_TTS_LOAD   = UI_STATE_TTS_LOADING, /* 10 */
    ST_TTS_PLAY   = UI_STATE_TTS_PLAYING, /* 11 */
    ST_MP3        = UI_STATE_PLAYING_MP3, /* 12 */
    ST_NOTIFYING  = UI_STATE_NOTIFYING,   /* 13 */
    ST_ERROR      = UI_STATE_ERROR,       /* 14 */
};

static const uint64_t s_valid_transitions[] = {
    [ST_BOOT]       = STATE_MASK(ST_CONNECTING) | STATE_MASK(ST_IDLE),
    [ST_CONNECTING] = STATE_MASK(ST_IDLE) | STATE_MASK(ST_ERROR),
    [ST_IDLE]       = STATE_MASK(ST_LISTENING) | STATE_MASK(ST_MP3) |
                      STATE_MASK(ST_NOTIFYING) | STATE_MASK(ST_CONNECTING) |
                      STATE_MASK(ST_ERROR),
    [ST_LISTENING]  = STATE_MASK(ST_SENDING) | STATE_MASK(ST_IDLE),
    [ST_SENDING]    = STATE_MASK(ST_THINKING) | STATE_MASK(ST_IDLE),
    [ST_THINKING]   = STATE_MASK(ST_STREAMING) | STATE_MASK(ST_IDLE) |
                      STATE_MASK(ST_ERROR),
    [ST_STREAMING]  = STATE_MASK(ST_RESPONSE) | STATE_MASK(ST_IDLE),
    [ST_RESPONSE]   = STATE_MASK(ST_TTS_LOAD) | STATE_MASK(ST_TTS_PLAY) | STATE_MASK(ST_IDLE),
    [ST_TTS_LOAD]   = STATE_MASK(ST_TTS_PLAY) | STATE_MASK(ST_IDLE),
    [ST_TTS_PLAY]   = STATE_MASK(ST_IDLE) | STATE_MASK(ST_LISTENING),
    [ST_MP3]        = STATE_MASK(ST_IDLE),
    [ST_NOTIFYING]  = STATE_MASK(ST_IDLE),
    [ST_ERROR]      = STATE_MASK(ST_CONNECTING) | STATE_MASK(ST_IDLE),
};

/* ── State → required resources ──────────────────────────────────
 * Each state lists the resources it needs, terminated by RES_COUNT.
 */
static const app_resource_t s_state_resources[][4] = {
    [ST_LISTENING]  = {RES_AUDIO_IN, RES_AUDIO_OUT, RES_COUNT},
    [ST_TTS_LOAD]   = {RES_COUNT},  /* Transient — no hardware resources needed */
    [ST_TTS_PLAY]   = {RES_AUDIO_OUT, RES_COUNT},
    [ST_MP3]        = {RES_AUDIO_OUT, RES_COUNT},
    [ST_NOTIFYING]  = {RES_AUDIO_OUT, RES_COUNT},
    /* All other states need no exclusive resources */
};

/* ── Priority (lower = higher priority) ───────────────────────── */
static int state_priority(ui_state_t st)
{
    switch (st) {
    case UI_STATE_LISTENING:   return 1;  /* Highest */
    case UI_STATE_PLAYING_MP3: return 2;
    case UI_STATE_TTS_PLAYING: return 3;
    case UI_STATE_NOTIFYING:   return 4;
    default:                   return 9;  /* Lowest — never preempted */
    }
}

static bool is_valid_transition(ui_state_t from, ui_state_t to)
{
    if (from == to) return true;
    if (from >= sizeof(s_valid_transitions) / sizeof(s_valid_transitions[0]))
        return true; /* Unknown state — allow */
    if (to == UI_STATE_ERROR) return true; /* ERROR always allowed */
    if (!s_valid_transitions[from]) return true; /* Unspecified — allow */
    return (s_valid_transitions[from] & STATE_MASK(to)) != 0;
}

/* ── Pre-transition hooks ─────────────────────────────────────── */
static void on_enter_state(ui_state_t target)
{
    switch (target) {
    case UI_STATE_LISTENING:
        /* Pause wake word (I2S RX) */
        wake_word_pause();
        break;
    case UI_STATE_PLAYING_MP3:
        wake_word_pause();
        break;
    default:
        break;
    }
}

static void on_leave_state(ui_state_t from)
{
    switch (from) {
    case UI_STATE_LISTENING:
        wake_word_resume();
        break;
    case UI_STATE_PLAYING_MP3:
        wake_word_resume();
        break;
    default:
        break;
    }
}

/* ── Helpers ───────────────────────────────────────────────────── */

/* True if this state is a "resource owner" (not a transient sub-state).
 * Transient states (SENDING, THINKING, STREAMING, RESPONSE) inherit
 * resources from their parent resource-owning state. */
static bool state_is_resource_owner(ui_state_t st)
{
    return (st == UI_STATE_LISTENING ||
            st == UI_STATE_PLAYING_MP3 ||
            st == UI_STATE_TTS_PLAYING ||
            st == UI_STATE_NOTIFYING);
}

/* ── Public API ────────────────────────────────────────────────── */

void app_state_machine_init(void)
{
    if (s_state_mutex) return;
    s_state_mutex = xSemaphoreCreateMutex();
    s_current_state = UI_STATE_BOOT;
    memset(s_resource_owner, 0, sizeof(s_resource_owner));
    ESP_LOGI(TAG, "State machine initialized");
}

ui_state_t app_state_current(void)
{
    ui_state_t st;
    if (s_state_mutex) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        st = s_current_state;
        xSemaphoreGive(s_state_mutex);
    } else {
        st = UI_STATE_BOOT;
    }
    return st;
}

esp_err_t app_state_request(ui_state_t target)
{
    if (!s_state_mutex) {
        ESP_LOGE(TAG, "State machine not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    ui_state_t old = s_current_state;

    /* 1. Validate transition */
    if (!is_valid_transition(old, target)) {
        xSemaphoreGive(s_state_mutex);
        ESP_LOGW(TAG, "Rejected transition: %d -> %d (not allowed)", old, target);
        return ESP_ERR_INVALID_STATE;
    }

    /* 2. Check/acquire resources for target state (only if target owns resources) */
    if (state_is_resource_owner(target)) {
        for (int i = 0; i < 4; i++) {
            app_resource_t res = s_state_resources[target][i];
            if (res >= RES_COUNT) break;

            ui_state_t owner = s_resource_owner[res];
            if (owner != 0 && owner != old && owner != target) {
                int owner_prio = state_priority(owner);
                int target_prio = state_priority(target);

                if (target_prio < owner_prio) {
                    ESP_LOGW(TAG, "Preempting state %d (prio %d) for state %d (prio %d)",
                             owner, owner_prio, target, target_prio);
                    app_state_force_idle(owner);
                    s_resource_owner[res] = 0;
                } else {
                    xSemaphoreGive(s_state_mutex);
                    ESP_LOGW(TAG, "Resource %d owned by state %d (prio %d), requested by %d (prio %d)",
                             res, owner, owner_prio, target, target_prio);
                    return ESP_ERR_INVALID_STATE;
                }
            }
        }
    }

    /* 3. Release old state's resources when:
     *    - target is IDLE (terminal — release everything), OR
     *    - old is a resource owner AND target doesn't inherit its resources */
    bool release_old = (target == UI_STATE_IDLE) ||
                       (state_is_resource_owner(old) && !state_is_resource_owner(target));

    if (old != target && release_old) {
        on_leave_state(old);
        for (int i = 0; i < RES_COUNT; i++) {
            if (s_resource_owner[i] == old) {
                s_resource_owner[i] = 0;
            }
        }
    }

    /* 4. Acquire new state's resources (only if it's a resource owner) */
    if (state_is_resource_owner(target)) {
        for (int i = 0; i < 4; i++) {
            app_resource_t res = s_state_resources[target][i];
            if (res >= RES_COUNT) break;
            s_resource_owner[res] = target;
        }
        on_enter_state(target);
    }

    s_current_state = target;
    xSemaphoreGive(s_state_mutex);

    /* 5. Apply the UI state */
    app_set_state(target);
    return ESP_OK;
}

void app_state_release(ui_state_t state)
{
    if (!s_state_mutex) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    on_leave_state(state);
    for (int i = 0; i < RES_COUNT; i++) {
        if (s_resource_owner[i] == state) {
            s_resource_owner[i] = 0;
        }
    }
    xSemaphoreGive(s_state_mutex);
    ESP_LOGI(TAG, "Released resources for state %d", state);
}

void app_state_force_idle(ui_state_t state)
{
    /* Release all resources owned by this state */
    app_state_release(state);

    /* If this was the current state, reset to IDLE */
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (s_current_state == state) {
        s_current_state = UI_STATE_IDLE;
    }
    xSemaphoreGive(s_state_mutex);

    /* Apply IDLE state (outside mutex to avoid deadlock with app_set_state) */
    app_set_state(UI_STATE_IDLE);
    ESP_LOGI(TAG, "Forced state %d → IDLE", state);
}

bool app_resource_is_owned(app_resource_t res)
{
    if (res >= RES_COUNT) return false;
    if (!s_state_mutex) return false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool owned = (s_resource_owner[res] != 0);
    xSemaphoreGive(s_state_mutex);
    return owned;
}

ui_state_t app_resource_owner(app_resource_t res)
{
    if (res >= RES_COUNT) return 0;
    if (!s_state_mutex) return 0;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    ui_state_t owner = s_resource_owner[res];
    xSemaphoreGive(s_state_mutex);
    return owner;
}
