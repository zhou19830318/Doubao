/**
 * @file touch_axs5106l.h
 * @brief AXS5106L I2C capacitive touch driver + LVGL input device
 *
 * Touch panel on Waveshare 1.47" screen, connected via I2C.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Touch configuration
 */
typedef struct {
    uint16_t x_max;        /**< Touch X coordinate max (display width after rotation) */
    uint16_t y_max;        /**< Touch Y coordinate max (display height after rotation) */
    uint8_t  i2c_addr;     /**< AXS5106L I2C address (7-bit) */
    bool     swap_xy;      /**< Swap X and Y axes */
    bool     mirror_x;     /**< Mirror X axis */
    bool     mirror_y;     /**< Mirror Y axis */
} touch_axs5106l_config_t;

/** Default touch config (172x320 portrait, 0deg rotation) */
#define TOUCH_AXS5106L_CONFIG_DEFAULT() { \
    .x_max    = 171, \
    .y_max    = 319, \
    .i2c_addr = 0x5C, \
    .swap_xy  = false, \
    .mirror_x = true, \
    .mirror_y = false, \
}

/**
 * @brief Initialize AXS5106L touch hardware (I2C probe + reset)
 *
 * @param cfg  Touch configuration (NULL for defaults)
 * @return ESP_OK on success
 */
esp_err_t touch_axs5106l_init(const touch_axs5106l_config_t *cfg);

/**
 * @brief Register the initialized touch driver as an LVGL input device
 *
 * MUST be called after lvgl_port_init().
 * @return ESP_OK on success
 */
esp_err_t touch_axs5106l_register_lvgl(void);

#ifdef __cplusplus
}
#endif
