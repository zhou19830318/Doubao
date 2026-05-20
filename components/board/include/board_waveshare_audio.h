/*
 * SPDX-FileCopyrightText: 2024-2026 AIClaw Contributors
 * SPDX-License-Identifier: MIT
 *
 * Waveshare ESP32-S3-AUDIO-Board pin definitions and hardware constants.
 *
 * Reference: https://www.waveshare.com/wiki/ESP32-S3-AUDIO-Board
 *            Schematic: ESP32-S3-AUDIO-Board_1.1.pdf
 *            xiaozhi-esp32: boards/waveshare-esp-box-2/config.h
 *
 * Hardware Overview:
 *   MCU:        ESP32-S3R8 (dual-core, 240MHz) with 16MB Flash + 8MB PSRAM
 *   Speaker:    ES8311 DAC codec via I2S
 *   Microphone: ES7210 ADC codec (4-channel, dual mic array) via I2S (shared bus)
 *   RGB LED:    7× WS2812 ring on GPIO38
 *   Buttons:    BOOT (GPIO0) + 3 user buttons via IO expander
 *   IO Expand:  TCA9555 16-bit I2C GPIO expander
 *   SD Card:    MicroSD via SPI (SDMMC 1-bit)
 *   RTC:        PCF85063 via I2C
 *   No display, no touch, no camera, no rotary encoder.
 */

#pragma once

#include "driver/gpio.h"
#include "hal/adc_types.h"

// ============================================================================
// Board identification
// ============================================================================
#define BOARD_NAME "Waveshare ESP32-S3 Audio"
#define BOARD_MCU  "ESP32-S3"

// ============================================================================
// Board capability flags
// ============================================================================
#define BOARD_HAS_DISPLAY       1
#define BOARD_HAS_TOUCH         1
#define BOARD_HAS_KNOB          0
#define BOARD_HAS_CAMERA        0
#define BOARD_HAS_IO_EXPANDER   1
#define BOARD_HAS_RGB_RING      1   // 7-LED WS2812 ring
#define BOARD_HAS_USER_BUTTONS  1   // BOOT + 3 IO expander buttons

// ============================================================================
// LCD Display (JD9853 via SPI)
// ============================================================================
#define BOARD_LCD_SPI_HOST      SPI2_HOST
#define BOARD_LCD_CS            GPIO_NUM_3
#define BOARD_LCD_SCLK          GPIO_NUM_4
#define BOARD_LCD_MOSI          GPIO_NUM_9
#define BOARD_LCD_MISO          GPIO_NUM_8
#define BOARD_LCD_DC            GPIO_NUM_7
#define BOARD_LCD_BL            GPIO_NUM_5
#define BOARD_LCD_RST           (-1)    // Reset via IO expander BOARD_IOEXP_LCD_RST

#define BOARD_LCD_H_RES         172
#define BOARD_LCD_V_RES         320
#define BOARD_LCD_PIXEL_CLK_HZ  (40 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS      8
#define BOARD_LCD_PARAM_BITS    8
#define BOARD_LCD_BPP           16

// ============================================================================
// Touch Panel (AXS5106L via I2C)
// ============================================================================
#define BOARD_TOUCH_I2C_ADDR    0x5C
#define BOARD_TOUCH_RST         (-1)    // Reset via IO expander BOARD_IOEXP_TOUCH_RST
#define BOARD_TOUCH_INT         (-1)    // Interrupt via IO expander BOARD_IOEXP_TOUCH_INT

// ============================================================================
// General I2C Bus (I2C0 - IO Expander, Codecs, RTC)
// ============================================================================
#define BOARD_I2C_PORT          0
#define BOARD_I2C_SDA           GPIO_NUM_11
#define BOARD_I2C_SCL           GPIO_NUM_10
#define BOARD_I2C_FREQ          400000

// ============================================================================
// IO Expander (TCA9555 on I2C0)
// ============================================================================
#define BOARD_IO_EXP_ADDR       0x20    // TCA9555 ADDRESS_000

// ============================================================================
// Audio I2S (shared bus: speaker ES8311 + mic ES7210)
// ============================================================================
#define BOARD_I2S_NUM           0
#define BOARD_I2S_MCLK          GPIO_NUM_12
#define BOARD_I2S_SCLK          GPIO_NUM_13
#define BOARD_I2S_LRCK          GPIO_NUM_14
#define BOARD_I2S_DSIN          GPIO_NUM_15     // Data IN  (from mic ES7210)
#define BOARD_I2S_DOUT          GPIO_NUM_16     // Data OUT (to speaker ES8311)
#define BOARD_AUDIO_SAMPLE_RATE 16000
#define BOARD_AUDIO_SAMPLE_BITS 16

// Audio codec I2C addresses (on I2C0)
#define BOARD_ES8311_ADDR       0x18    // Speaker DAC (7-bit)
#define BOARD_ES7210_ADDR       0x40    // Microphone ADC (7-bit, ES7210 default)
#define BOARD_AUDIO_MIC_GAIN    36.0f

// ============================================================================
// RGB LED Ring (7× WS2812)
// ============================================================================
#define BOARD_RGB_GPIO          GPIO_NUM_38
#define BOARD_RGB_LED_COUNT     7

// ============================================================================
// Buttons
// ============================================================================
#define BOARD_BOOT_BUTTON       GPIO_NUM_0      // On-SoC BOOT button

// IO expander button pins (active low via TCA9555)
#define BOARD_IOEXP_BTN1        9   // P1.1
#define BOARD_IOEXP_BTN2        10  // P1.2
#define BOARD_IOEXP_BTN3        11  // P1.3

// PA enable via IO expander
#define BOARD_IOEXP_PA_EN       8   // P1.0

// LCD/Touch reset (optional, if LCD attached)
#define BOARD_IOEXP_LCD_RST     0   // P0.0
#define BOARD_IOEXP_TOUCH_RST   1   // P0.1
#define BOARD_IOEXP_TOUCH_INT   2   // P0.2

// Camera enable/mux (optional, if camera attached)
#define BOARD_IOEXP_CAM_EN      5   // P0.5
#define BOARD_IOEXP_CAM_MUX     6   // P0.6

// SD card CS via IO expander
#define BOARD_IOEXP_SD_CS       3   // P0.3

// ============================================================================
// SD Card (SDMMC 1-bit mode)
// ============================================================================
#define BOARD_SD_CLK            GPIO_NUM_40
#define BOARD_SD_CMD            GPIO_NUM_42
#define BOARD_SD_D0             GPIO_NUM_41

// ============================================================================
// RTC (PCF85063 on I2C0)
// ============================================================================
#define BOARD_RTC_I2C_ADDR      0x51
