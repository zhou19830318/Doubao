/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * Background tasks — knob monitor, minimal sleep
 */

#include "app_tasks.h"
#include "app_state.h"
#include "app_state_machine.h"
#include "settings.h"

#include "board.h"
#include "ui.h"
#include "wake_word.h"
#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#if BOARD_HAS_DISPLAY
#include "esp_lvgl_port.h"
#endif
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "app_tasks";

/* Forward declaration */
static void enter_deep_sleep(void);

/* ── Sleep state ─────────────────────────────────────────────────── */
static volatile int64_t s_last_activity_us = 0;
static volatile int64_t s_last_wake_us = 0;
static volatile bool s_sleeping = false;

/* ── Knob monitoring — long press detection + cancel ──────────────── */
#define LONG_PRESS_MS 4000
#define DEBOUNCE_MS   50

static void knob_task(void *arg)
{
    bool was_pressed = false;
    int64_t press_start = 0;
    bool long_press_fired = false;

    extern EventGroupHandle_t g_app_events;
    while (!g_app_events) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    while (1) {
#if BOARD_HAS_KNOB
        bool pressed = board_knob_button_pressed();
#elif (BOARD_HAS_USER_BUTTONS || BOARD_HAS_BOOT_BUTTON) && !defined(CONFIG_AIDB_BOARD_M5STICKCPLUS2)
        bool pressed = board_boot_button_pressed();
#else
        bool pressed = false;
#endif

        if (pressed && !was_pressed) {
            ESP_LOGI(TAG, "BOOT button pressed (state=%d)", ui_get_state());
            press_start = esp_timer_get_time() / 1000;
            long_press_fired = false;
        } else if (pressed && was_pressed && !long_press_fired) {
            int64_t held_ms = (esp_timer_get_time() / 1000) - press_start;
            if (held_ms >= LONG_PRESS_MS) {
                long_press_fired = true;
#if BOARD_HAS_USER_BUTTONS && !BOARD_HAS_KNOB
                ESP_LOGW(TAG, "Button A long press — cancel/abort");
                ui_state_t st = ui_get_state();
                if (st == UI_STATE_THINKING || st == UI_STATE_STREAMING ||
                    st == UI_STATE_TTS_LOADING || st == UI_STATE_TTS_PLAYING) {
                    lvgl_port_lock(0);
                    app_set_state(UI_STATE_IDLE);
                    lvgl_port_unlock();
                }
#else
                ESP_LOGW(TAG, "Long press detected (%lld ms) — entering deep sleep", held_ms);
#if BOARD_HAS_RGB_RING
                board_rgb_animate(RGB_MODE_BLINK, 32, 0, 0);
#endif
                vTaskDelay(pdMS_TO_TICKS(500));
                enter_deep_sleep();
#endif
            }
        } else if (!pressed && was_pressed && !long_press_fired) {
            if (g_recording) {
                xEventGroupSetBits(g_app_events, CANCEL_BIT);
            } else {
                xEventGroupSetBits(g_app_events, KNOB_PRESSED_BIT);
            }
        }

        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

#if BOARD_HAS_USER_BUTTONS && !BOARD_HAS_KNOB
#ifdef CONFIG_AIDB_BOARD_M5STICKCPLUS2
        {
            static bool a_was = false;
            static int64_t a_press_ms = 0;
            static bool a_long_fired = false;
            static int64_t a_release_ms = 0;
            static bool a_pending_single = false;
            int64_t now_ms = esp_timer_get_time() / 1000;
            bool a_now = board_boot_button_pressed();
            #define DBLCLK_WINDOW_MS 350

            if (a_now && !a_was) {
                if (a_pending_single && (now_ms - a_release_ms) < DBLCLK_WINDOW_MS) {
                    a_pending_single = false;
                    app_reset_activity_timer();
                    bool ar = settings_get()->auto_read_response;
                    settings_get_mutable()->auto_read_response = !ar;
                    settings_save();
                    ESP_LOGI(TAG, "Btn A DblClk: auto-read TTS %s", ar ? "OFF" : "ON");
                    lvgl_port_lock(0);
                    ui_set_status_message(ar ? "Auto-TTS: OFF" : "Auto-TTS: ON");
                    lvgl_port_unlock();
                }
                a_press_ms = now_ms;
                a_long_fired = false;
            } else if (a_now && a_was && !a_long_fired) {
                if ((now_ms - a_press_ms) >= LONG_PRESS_MS) {
                    a_long_fired = true;
                    ESP_LOGW(TAG, "Btn A long press — deep sleep");
                    board_rgb_animate(RGB_MODE_BLINK, 32, 0, 0);
                    vTaskDelay(pdMS_TO_TICKS(500));
                    enter_deep_sleep();
                }
            } else if (!a_now && a_was && !a_long_fired) {
                a_release_ms = now_ms;
                a_pending_single = true;
            }

            if (a_pending_single && !a_now && (now_ms - a_release_ms) >= DBLCLK_WINDOW_MS) {
                a_pending_single = false;
                app_reset_activity_timer();
                if (g_recording) {
                    ESP_LOGI(TAG, "Btn A: cancel recording");
                    xEventGroupSetBits(g_app_events, CANCEL_BIT);
                } else {
                    ESP_LOGI(TAG, "Btn A: talk");
                    xEventGroupSetBits(g_app_events, KNOB_PRESSED_BIT);
                }
            }
            a_was = a_now;
        }
#endif
#endif
    }
}

/* ── State timeout tracking ──────────────────────────────────────── */
#define TIMEOUT_THINKING_MS   30000
#define TIMEOUT_STREAMING_MS  30000
#define TIMEOUT_TTS_PLAY_MS   60000

static int64_t s_state_enter_us = 0;
static ui_state_t s_tracked_state = UI_STATE_BOOT;

/* ── Error recovery tracking ─────────────────────────────────────── */
#define MAX_CONSECUTIVE_ERRORS  10
#define ERROR_WINDOW_MS      300000

static int  s_error_count = 0;
static int64_t s_first_error_us = 0;

/* ── Periodic status ─────────────────────────────────────────────── */
static void status_update_task(void *arg)
{
    while (1) {
        if (s_sleeping) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        lvgl_port_lock(0);

        /* WiFi RSSI */
        if (wifi_manager_get_state() == WIFI_STATE_CONNECTED) {
            ui_set_wifi_status(true, wifi_manager_get_rssi());
        }

        /* Response timeout (60s) */
        if (ui_get_state() == UI_STATE_RESPONSE && g_response_shown_at > 0) {
            int64_t elapsed = (esp_timer_get_time() - g_response_shown_at) / 1000;
            if (elapsed > 60000) {
                app_state_request(UI_STATE_IDLE);
                g_response_shown_at = 0;
            }
        }

        /* State timeout protection */
        {
            ui_state_t cur = ui_get_state();
            if (cur != s_tracked_state) {
                s_tracked_state = cur;
                s_state_enter_us = esp_timer_get_time();
            }
            int64_t dwell_ms = (esp_timer_get_time() - s_state_enter_us) / 1000;

            if (cur == UI_STATE_THINKING && dwell_ms > TIMEOUT_THINKING_MS) {
                ESP_LOGW(TAG, "THINKING timeout (%lld ms), forcing IDLE", dwell_ms);
                app_state_request(UI_STATE_IDLE);
            }
            if (cur == UI_STATE_STREAMING && dwell_ms > TIMEOUT_STREAMING_MS) {
                ESP_LOGW(TAG, "STREAMING timeout (%lld ms), forcing IDLE", dwell_ms);
                app_state_request(UI_STATE_IDLE);
            }
            if (cur == UI_STATE_TTS_PLAYING && dwell_ms > TIMEOUT_TTS_PLAY_MS) {
                ESP_LOGW(TAG, "TTS_PLAYING timeout (%lld ms), stopping TTS", dwell_ms);
            }
        }

        /* Error recovery */
        if (ui_get_state() == UI_STATE_ERROR) {
            int64_t now_us = esp_timer_get_time();
            if (s_error_count == 0) {
                s_first_error_us = now_us;
                s_error_count = 1;
            } else if ((now_us - s_first_error_us) < ERROR_WINDOW_MS * 1000LL) {
                s_error_count++;
            } else {
                s_first_error_us = now_us;
                s_error_count = 1;
            }

            if (s_error_count >= MAX_CONSECUTIVE_ERRORS) {
                ESP_LOGW(TAG, "%d consecutive errors in window — entering deep sleep",
                         s_error_count);
                s_error_count = 0;
                lvgl_port_unlock();
                app_enter_deep_sleep();
            }
        } else if (ui_get_state() == UI_STATE_IDLE || ui_get_state() == UI_STATE_LISTENING) {
            s_error_count = 0;
            s_first_error_us = 0;
        }

        lvgl_port_unlock();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ── Sleep management ────────────────────────────────────────────── */

void app_reset_activity_timer(void)
{
    s_last_activity_us = esp_timer_get_time();
    if (s_sleeping) {
        s_sleeping = false;
        s_last_wake_us = esp_timer_get_time();
        board_audio_resume();
        const settings_t *cfg = settings_get();
        board_display_set_brightness(cfg->brightness);
        if (cfg->rgb_enabled) app_led_for_state(ui_get_state());
#if BOARD_HAS_DISPLAY
        lvgl_port_resume();
#endif
#if !defined(CONFIG_AIDB_BOARD_M5STICKCPLUS2)
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
#endif
        if (!cfg->wake_word_in_sleep) {
            wake_word_resume();
        }
        ESP_LOGI(TAG, "Wake from light sleep (activity)");
    }
}

bool app_just_woke(void)
{
    if (s_last_wake_us == 0) return false;
    return (esp_timer_get_time() - s_last_wake_us) < 800000;
}

bool app_is_sleeping(void)
{
    return s_sleeping;
}

static void enter_deep_sleep(void)
{
    ESP_LOGW(TAG, "Entering deep sleep — press button to wake");
    esp_wifi_disconnect();
    esp_wifi_stop();

    board_display_set_brightness(0);
#if BOARD_HAS_RGB_RING
    board_rgb_animate(RGB_MODE_OFF, 0, 0, 0);
#endif
    vTaskDelay(pdMS_TO_TICKS(100));
    board_prepare_deep_sleep();

    ESP_LOGW(TAG, "Calling esp_deep_sleep_start()");
    esp_deep_sleep_start();
}

void app_enter_deep_sleep(void)
{
    enter_deep_sleep();
}

static void sleep_task(void *arg)
{
    s_last_activity_us = esp_timer_get_time();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        const settings_t *cfg = settings_get();
        if (cfg->sleep_timeout_ms == 0) continue;

        ui_state_t state = ui_get_state();
        if (state == UI_STATE_LISTENING || state == UI_STATE_SENDING ||
            state == UI_STATE_THINKING || state == UI_STATE_STREAMING ||
            state == UI_STATE_TTS_LOADING || state == UI_STATE_TTS_PLAYING) {
            s_last_activity_us = esp_timer_get_time();
            continue;
        }

        int64_t idle_ms = (esp_timer_get_time() - s_last_activity_us) / 1000;

        if (s_sleeping) {
#if BOARD_HAS_KNOB
            if (board_knob_button_pressed()) {
                while (board_knob_button_pressed()) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
#elif BOARD_HAS_USER_BUTTONS || BOARD_HAS_BOOT_BUTTON
            if (board_boot_button_pressed()) {
                while (board_boot_button_pressed()) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
#else
            if (false) {
#endif
                ESP_LOGI(TAG, "Wake from light sleep (button press)");
                s_sleeping = false;
                s_last_wake_us = esp_timer_get_time();
                board_display_set_brightness(cfg->brightness);
                if (cfg->rgb_enabled) app_led_for_state(ui_get_state());
#if BOARD_HAS_DISPLAY
                lvgl_port_resume();
#endif
#if !defined(CONFIG_AIDB_BOARD_M5STICKCPLUS2)
                esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
#endif
                if (!cfg->wake_word_in_sleep) {
                    wake_word_resume();
                }
                s_last_activity_us = esp_timer_get_time();
            }
            continue;
        }

        if (idle_ms > (int64_t)cfg->sleep_timeout_ms) {
            ESP_LOGI(TAG, "Idle for %lld ms, entering light sleep", idle_ms);
            s_sleeping = true;

            board_display_set_brightness(0);
#if BOARD_HAS_RGB_RING
            board_rgb_animate(RGB_MODE_OFF, 0, 0, 0);
#endif
#if BOARD_HAS_DISPLAY
            lvgl_port_stop();
#endif
            if (!cfg->wake_word_in_sleep) {
                wake_word_pause();
                ESP_LOGI(TAG, "Wake word paused for sleep (use button to wake)");
            }
#if !defined(CONFIG_AIDB_BOARD_M5STICKCPLUS2)
            esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
#endif
        }
    }
}

/* ── Start all tasks ─────────────────────────────────────────────── */
void app_tasks_start(void)
{
    xTaskCreatePinnedToCore(knob_task, "knob", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(status_update_task, "status", 6144, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(sleep_task, "sleep", 4096, NULL, 1, NULL, 0);

    ESP_LOGI(TAG, "Background tasks started");
}
