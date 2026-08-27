/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * Centralized state machine — validates transitions and arbitrates resources.
 *
 * Design:
 *   All state transitions go through app_state_request(), which:
 *    1. Validates the transition under the mutex (short critical section)
 *    2. Acquires required hardware resources
 *    3. Releases old state's resources
 *    4. Updates s_current_state and resource owners (under mutex)
 *    5. Releases the mutex
 *    6. Executes hooks (wake_word_pause/resume) OUTSIDE the mutex —
 *       these may block up to 500ms and must never be called while
 *       holding s_state_mutex to prevent recursive deadlock (C1) and
 *       long-lock-held blocking (C2).
 *    7. Applies the UI state via app_set_state()
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
 * All unlisted transitions are REJECTED (S2 fix: unknown = deny).
 * Special: ERROR is only allowed via explicit transition entries (S3 fix).
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
    [ST_LISTENING]  = STATE_MASK(ST_SENDING) | STATE_MASK(ST_THINKING) |
                      STATE_MASK(ST_IDLE) | STATE_MASK(ST_ERROR),
    [ST_SENDING]    = STATE_MASK(ST_THINKING) | STATE_MASK(ST_IDLE) |
                      STATE_MASK(ST_ERROR),
    [ST_THINKING]   = STATE_MASK(ST_STREAMING) | STATE_MASK(ST_IDLE) |
                      STATE_MASK(ST_LISTENING) | STATE_MASK(ST_ERROR),
    [ST_STREAMING]  = STATE_MASK(ST_RESPONSE) | STATE_MASK(ST_IDLE) |
                      STATE_MASK(ST_ERROR),
    [ST_RESPONSE]   = STATE_MASK(ST_TTS_LOAD) | STATE_MASK(ST_TTS_PLAY) |
                      STATE_MASK(ST_IDLE) | STATE_MASK(ST_ERROR),
    [ST_TTS_LOAD]   = STATE_MASK(ST_TTS_PLAY) | STATE_MASK(ST_IDLE) |
                      STATE_MASK(ST_ERROR),
    [ST_TTS_PLAY]   = STATE_MASK(ST_IDLE) | STATE_MASK(ST_LISTENING) |
                      STATE_MASK(ST_ERROR),
    [ST_MP3]        = STATE_MASK(ST_IDLE) | STATE_MASK(ST_ERROR),
    [ST_NOTIFYING]  = STATE_MASK(ST_IDLE) | STATE_MASK(ST_ERROR),
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

    /* S2 fix: reject out-of-range states — no silent allow */
    if (from >= sizeof(s_valid_transitions) / sizeof(s_valid_transitions[0])) {
        return false;
    }
    if (to >= sizeof(s_valid_transitions) / sizeof(s_valid_transitions[0])) {
        return false;
    }

    /* S3 fix: ERROR is only allowed via explicit entries in the table,
     * not as a global escape hatch. Callers must route through a proper
     * ERROR event if they want to enter ERROR state. */
    if (!s_valid_transitions[from]) return false; /* No entries = reject */
    return (s_valid_transitions[from] & STATE_MASK(to)) != 0;
}

/* ── Deferred hook tracking ──────────────────────────────────────
 * C1/C2 fix: hooks (wake_word_pause/resume) must execute OUTSIDE
 * the state machine mutex. These small structures record what hooks
 * need to run, populated during the locked critical section, then
 * executed after the mutex is released. */
typedef struct {
    ui_state_t leave_state;  /* State being left (for on_leave_state) */
    ui_state_t enter_state;  /* State being entered (for on_enter_state) */
    bool has_leave;
    bool has_enter;
} state_hooks_t;

/* Execute hooks outside the mutex — may block (wake_word_pause up to 500ms) */
static void execute_hooks(const state_hooks_t *hooks)
{
    if (hooks->has_leave) {
        switch (hooks->leave_state) {
        case UI_STATE_PLAYING_MP3:
            wake_word_resume();
            break;
        default:
            break;
        }
    }

    if (hooks->has_enter) {
        switch (hooks->enter_state) {
        case UI_STATE_LISTENING:
        case UI_STATE_PLAYING_MP3:
            wake_word_pause();
            break;
        case UI_STATE_IDLE:
            /* Wake word runs ONLY in IDLE. Resuming it on leave(LISTENING)
             * made it read the mic concurrently with the doubao capture
             * task during STREAMING/SPEAKING — three tasks then contended
             * for the codec mutex and the priority-9 capture task lost every
             * race ("mic read incomplete" floods). One reader at a time. */
            wake_word_resume();
            break;
        default:
            break;
        }
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

/* C1 fix: Internal variant that operates WITHOUT taking the mutex.
 * Called only from app_state_request() which already holds it.
 * Releases resource ownership for a given state. Does NOT call hooks
 * (on_leave_state) — those are deferred to execute_hooks(). */
static void state_release_resources_locked(ui_state_t state)
{
    for (int i = 0; i < RES_COUNT; i++) {
        if (s_resource_owner[i] == state) {
            s_resource_owner[i] = 0;
        }
    }
}

/* C1 fix: Internal variant for force-idle that operates WITHOUT
 * recursively taking the mutex. Only updates internal bookkeeping —
 * does NOT call app_set_state() (caller must do that after releasing mutex). */
static void state_force_idle_locked(ui_state_t state)
{
    state_release_resources_locked(state);
    if (s_current_state == state) {  /* S4 fix: only if still current */
        s_current_state = UI_STATE_IDLE;
    }
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

void app_state_machine_force_current(ui_state_t st)
{
    if (!s_state_mutex) return;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_current_state = st;
    xSemaphoreGive(s_state_mutex);
}

esp_err_t app_state_request(ui_state_t target)
{
    if (!s_state_mutex) {
        ESP_LOGE(TAG, "State machine not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    state_hooks_t hooks = {0};
    ui_state_t ui_target = target;  /* What to pass to app_set_state() */

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    ui_state_t old = s_current_state;

    /* 1. Validate transition (S2/S3: unknown/ERROR handled inside) */
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
                    /* C1 fix: use _locked variant instead of
                     * app_state_force_idle() which would recursively
                     * take the mutex → deadlock. */
                    state_force_idle_locked(owner);
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
        hooks.has_leave = true;
        hooks.leave_state = old;
        state_release_resources_locked(old);
    }

    /* 4. Acquire new state's resources (only if it's a resource owner) */
    if (state_is_resource_owner(target)) {
        for (int i = 0; i < 4; i++) {
            app_resource_t res = s_state_resources[target][i];
            if (res >= RES_COUNT) break;
            s_resource_owner[res] = target;
        }
        hooks.has_enter = true;
        hooks.enter_state = target;
    }

    s_current_state = target;
    xSemaphoreGive(s_state_mutex);

    /* 5. C2 fix: Execute hooks OUTSIDE the mutex.
     * wake_word_pause() can block up to 500ms waiting for the detection
     * task to acknowledge the pause. Holding the state machine mutex
     * during that wait causes:
     *   - Any other state transition request deadlocks (C1)
     *   - Audio tasks that need state info are blocked for 500ms+ (C2)
     * The two-phase approach (validate+update under lock, then side-effects
     * after release) keeps the critical section to a few μs. */
    execute_hooks(&hooks);

    /* 6. Apply the UI state */
    app_set_state(ui_target);
    return ESP_OK;
}

void app_state_release(ui_state_t state)
{
    if (!s_state_mutex) return;

    state_hooks_t hooks = {0};

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    hooks.has_leave = true;
    hooks.leave_state = state;
    state_release_resources_locked(state);
    xSemaphoreGive(s_state_mutex);

    /* C2 fix: hooks execute outside the mutex */
    execute_hooks(&hooks);
    ESP_LOGI(TAG, "Released resources for state %d", state);
}

void app_state_force_idle(ui_state_t state)
{
    if (!s_state_mutex) return;

    state_hooks_t hooks = {0};

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);

    /* C1 fix: use _locked variant instead of app_state_release()
     * which would recursively take the mutex → deadlock. */
    state_release_resources_locked(state);

    /* S4 fix: only update to IDLE if the caller's state is still current.
     * If the state has already changed (e.g., a higher-priority transition
     * happened), don't overwrite the new state. */
    if (s_current_state == state) {
        s_current_state = UI_STATE_IDLE;
        hooks.has_leave = true;
        hooks.leave_state = state;
        hooks.has_enter = true;
        hooks.enter_state = UI_STATE_IDLE;
    }

    xSemaphoreGive(s_state_mutex);

    /* Execute hooks and UI update outside the mutex */
    execute_hooks(&hooks);
    if (hooks.has_leave || hooks.has_enter) {
        app_set_state(UI_STATE_IDLE);
    }
    ESP_LOGI(TAG, "Forced state %d -> IDLE", state);
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
