/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * Board abstraction layer — Waveshare ESP32-S3 Audio Board
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Board pin & feature definitions — select by Kconfig */
#if defined(CONFIG_AIDB_BOARD_WAVESHARE_AUDIO)
#include "board_waveshare_audio.h"
#elif defined(CONFIG_AIDB_BOARD_SENSECAP_WATCHER)
/* SenseCAP Watcher — placeholder (not yet implemented as separate header) */
#include "board_waveshare_audio.h"  // fallback for now
#elif defined(CONFIG_AIDB_BOARD_AMOLED_206)
#include "board_amoled_206.h"
#else
#warning "No board selected via Kconfig — using Waveshare Audio defaults"
#include "board_waveshare_audio.h"
#endif

/* Forward-declare LVGL types used in API (LVGL 9.x) */
typedef struct _lv_display_t lv_display_t;
typedef struct _lv_obj_t lv_obj_t;

// ============================================================================
// Board API
// ============================================================================

// --- System ---
esp_err_t board_init(void);
void      board_power_off(void);
void      board_reboot(void);
void      board_prepare_deep_sleep(void);
void *    board_get_i2c0_handle(void);

// --- Display ---
esp_err_t board_display_init(void);
esp_err_t board_display_set_brightness(int percent);

// --- Touch ---
esp_err_t board_touch_init(void);
void      board_tp_reset(bool active);

// --- LVGL ---
esp_err_t board_lvgl_init(void);
lv_display_t *board_get_lvgl_disp(void);

// --- RGB LED ---
esp_err_t board_rgb_init(void);
esp_err_t board_rgb_set(uint8_t r, uint8_t g, uint8_t b);
esp_err_t board_rgb_set_single(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
esp_err_t board_rgb_refresh(void);

typedef enum {
    RGB_MODE_SOLID = 0,
    RGB_MODE_BREATHE,
    RGB_MODE_BLINK,
    RGB_MODE_OFF,
    RGB_MODE_RAINBOW_SPIN,
    RGB_MODE_CHASE,
    RGB_MODE_PULSE_WAVE,
    RGB_MODE_SPARKLE,
    RGB_MODE_AURORA,
    RGB_MODE_STARFIELD,
    RGB_MODE_FIRE,
    RGB_MODE_OCEAN,
} board_rgb_mode_t;

esp_err_t board_rgb_animate(board_rgb_mode_t mode, uint8_t r, uint8_t g, uint8_t b);
esp_err_t board_rgb_task_start(void);

// --- Audio ---
esp_err_t board_audio_init(void);
esp_err_t board_audio_record(int16_t *buffer, size_t samples, size_t *samples_read);
esp_err_t board_audio_play(const int16_t *buffer, size_t samples, size_t *samples_written);
esp_err_t board_audio_set_volume(int volume_percent);
esp_err_t board_audio_get_volume(int *volume_percent);
esp_err_t board_audio_mute(bool mute);
esp_err_t board_audio_reconfig(uint32_t sample_rate, uint8_t channels);
esp_err_t board_audio_reopen_adc(void);
esp_err_t board_audio_resume(void);
void      board_play_tick(void);

// --- User buttons ---
esp_err_t board_buttons_init(void);
bool      board_boot_button_pressed(void);
bool      board_user_button_pressed(int btn_num);

// --- SD Card ---
esp_err_t board_sdcard_init(void);
esp_err_t board_sdcard_deinit(void);
bool      board_sdcard_is_inserted(void);

// --- Battery ---
uint16_t  board_battery_get_voltage_mv(void);
uint8_t   board_battery_get_percent(void);
bool      board_battery_is_charging(void);

// --- IMU (6-axis accelerometer + gyroscope) ---
esp_err_t board_imu_init(void);
esp_err_t board_imu_read(float *accel_x, float *accel_y, float *accel_z,
                         float *gyro_x, float *gyro_y, float *gyro_z);

// --- Board info ---
const char *board_get_name(void);
const char *board_get_mcu(void);

// --- IO expander ---
void *board_get_io_expander(void);
esp_err_t board_pa_enable(bool enable);
esp_err_t board_sd_cs(bool enable);

// --- SPI2 (shared: camera + SD card) ---
esp_err_t board_spi2_init(void);

#ifdef __cplusplus
}
#endif