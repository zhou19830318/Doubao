/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/**
 * @file esp_lcd_panel_jd9853.h
 * @brief JD9853 LCD panel driver for esp_lcd interface
 *
 * Ported from Waveshare ESP32-S3-AUDIO-Board-Demo.
 */

#pragma once

#include "esp_lcd_panel_vendor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LCD panel initialization commands.
 */
typedef struct {
    int cmd;                /**< The specific LCD command */
    const void *data;       /**< Buffer that holds the command specific data */
    size_t data_bytes;      /**< Size of `data` in memory, in bytes */
    unsigned int delay_ms;  /**< Delay in milliseconds after this command */
} jd9853_lcd_init_cmd_t;

/**
 * @brief LCD panel vendor configuration.
 *
 * @note Pass to `vendor_config` field in `esp_lcd_panel_dev_config_t`.
 */
typedef struct {
    const jd9853_lcd_init_cmd_t *init_cmds;     /**< Init commands array. NULL = use defaults. */
    uint16_t init_cmds_size;                    /**< Number of commands in above array */
} jd9853_vendor_config_t;

/**
 * @brief Create LCD panel for JD9853
 *
 * @param[in] io LCD panel IO handle
 * @param[in] panel_dev_config general panel device configuration
 * @param[out] ret_panel Returned LCD panel handle
 * @return ESP_OK on success
 */
esp_err_t esp_lcd_new_panel_jd9853(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif
