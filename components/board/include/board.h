/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * Board abstraction layer - common interface for all hardware targets.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"
#if !defined(CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2)
#include "driver/i2s_std.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Board selection
// ============================================================================
#if defined(CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER)
#include "board_sensecap_watcher.h"
#elif defined(CONFIG_HEYCLAWY_BOARD_WAVESHARE_AUDIO)
#include "board_waveshare_audio.h"
#elif defined(CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2)
#include "board_m5stickcplus2.h"
#elif defined(CONFIG_HEYCLAWY_BOARD_GENERIC_ESP32S3)
#include "board_generic_esp32s3.h"
#else
#error "No board selected! Set HEYCLAWY_BOARD_SELECT in menuconfig."
#endif

/* Forward-declare LVGL types used in API */
typedef struct _lv_disp_t lv_disp_t;
typedef struct _lv_obj_t lv_obj_t;

// ============================================================================
// Board API
// ============================================================================

// --- System ---
esp_err_t board_init(void);
void      board_power_off(void);
void      board_reboot(void);
void      board_prepare_deep_sleep(void);  /* Power off peripherals + configure wake GPIO */
void *    board_get_i2c0_handle(void);    /* Returns i2c_master_bus_handle_t */

#if BOARD_HAS_DISPLAY
// --- Display ---
esp_err_t board_display_init(void);
esp_err_t board_display_set_brightness(int percent);

// --- Touch ---
esp_err_t board_touch_init(void);
void      board_tp_reset(bool active);

// --- LVGL ---
esp_err_t board_lvgl_init(void);
lv_disp_t *board_get_lvgl_disp(void);
#else
/* No-op stubs for screenless boards */
static inline esp_err_t board_display_init(void) { return ESP_OK; }
static inline esp_err_t board_display_set_brightness(int percent) { (void)percent; return ESP_OK; }
static inline esp_err_t board_touch_init(void) { return ESP_OK; }
static inline esp_err_t board_lvgl_init(void) { return ESP_OK; }
static inline lv_disp_t *board_get_lvgl_disp(void) { return NULL; }
#endif

// --- RGB LED ---
esp_err_t board_rgb_init(void);
esp_err_t board_rgb_set(uint8_t r, uint8_t g, uint8_t b);
esp_err_t board_rgb_set_single(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
esp_err_t board_rgb_refresh(void);

// RGB LED animation modes
typedef enum {
    RGB_MODE_SOLID = 0,     // Static color
    RGB_MODE_BREATHE,       // Smooth pulsing
    RGB_MODE_BLINK,         // On/off blinking
    RGB_MODE_OFF,           // LED off
    RGB_MODE_RAINBOW_SPIN,  // 1) Smooth rotating rainbow (startup default)
    RGB_MODE_CHASE,         // Lit pixel chases around ring
    RGB_MODE_PULSE_WAVE,    // Pulse wave travels around ring
    RGB_MODE_SPARKLE,       // Random sparkle flashes
    RGB_MODE_AURORA,        // 2) Northern lights shimmer (green/blue/purple)
    RGB_MODE_STARFIELD,     // 3) Twinkling multicolor stars
    RGB_MODE_FIRE,          // 4) Warm fire embers (red/orange/yellow)
    RGB_MODE_OCEAN,         // 5) Deep ocean waves (blue/teal/white crests)
} board_rgb_mode_t;

// Set LED animation (runs in background task)
esp_err_t board_rgb_animate(board_rgb_mode_t mode, uint8_t r, uint8_t g, uint8_t b);
// Start the RGB animation task (call once after board_rgb_init)
esp_err_t board_rgb_task_start(void);

// --- Audio ---
esp_err_t board_audio_init(void);
esp_err_t board_audio_record(int16_t *buffer, size_t samples, size_t *samples_read);
esp_err_t board_audio_play(const int16_t *buffer, size_t samples, size_t *samples_written);
esp_err_t board_audio_set_volume(int volume_percent);
esp_err_t board_audio_get_volume(int *volume_percent);
esp_err_t board_audio_mute(bool mute);
esp_err_t board_audio_reconfig(uint32_t sample_rate, uint8_t channels);
esp_err_t board_audio_reopen_adc(void);  /* Re-open ADC after MP3 playback */
void      board_play_tick(void);      /* Short tick sound for touch feedback */

#if BOARD_HAS_KNOB
// --- Knob / Wheel Button ---
esp_err_t board_knob_init(void);
bool      board_knob_button_pressed(void);
/** Get encoder delta (positive = clockwise, negative = CCW). Resets after read. */
int       board_knob_get_delta(void);
#else
static inline esp_err_t board_knob_init(void) { return ESP_OK; }
static inline bool board_knob_button_pressed(void) { return false; }
static inline int board_knob_get_delta(void) { return 0; }
#endif

#if BOARD_HAS_USER_BUTTONS
// --- User buttons (via IO expander or GPIO) ---
esp_err_t board_buttons_init(void);
bool      board_boot_button_pressed(void);
/** Read IO expander button (1-3). Returns true if pressed. */
bool      board_user_button_pressed(int btn_num);
#else
static inline esp_err_t board_buttons_init(void) { return ESP_OK; }
static inline bool board_boot_button_pressed(void) { return false; }
static inline bool board_user_button_pressed(int btn_num) { (void)btn_num; return false; }
#endif

// --- SD Card ---
esp_err_t board_sdcard_init(void);
esp_err_t board_sdcard_deinit(void);
bool      board_sdcard_is_inserted(void);

// --- Battery ---
uint16_t  board_battery_get_voltage_mv(void);
uint8_t   board_battery_get_percent(void);
bool      board_battery_is_charging(void);

// --- Board info ---
const char *board_get_name(void);
const char *board_get_mcu(void);

#if BOARD_HAS_IO_EXPANDER
// --- IO expander (handle returned as void* — cast to esp_io_expander_handle_t with the real header) ---
void *board_get_io_expander(void);
esp_err_t board_pa_enable(bool enable);
esp_err_t board_sd_cs(bool enable);
#else
static inline void *board_get_io_expander(void) { return NULL; }
static inline esp_err_t board_pa_enable(bool enable) { (void)enable; return ESP_OK; }
static inline esp_err_t board_sd_cs(bool enable) { (void)enable; return ESP_OK; }
#endif

#if BOARD_HAS_CAMERA
// --- SPI2 bus (shared: camera + SD card) ---
esp_err_t board_spi2_init(void);
#else
static inline esp_err_t board_spi2_init(void) { return ESP_OK; }
#endif

#ifdef __cplusplus
}
#endif
