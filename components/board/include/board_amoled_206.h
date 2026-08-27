/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * Waveshare ESP32-S3-Touch-AMOLED-2.06 pin definitions and hardware constants.
 *
 * Reference: https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06
 *            Schematic: ESP32-S3-Touch-AMOLED-2.06-Schematic-V1.0.pdf
 *            Examples: ESP32-S3-Touch-AMOLED-2.06/examples/ESP-IDF-v5.4.2/
 *
 * Hardware Overview:
 *   MCU:        ESP32-S3R8 (dual-core, 240MHz) with 32MB Flash + 8MB PSRAM
 *   Display:    CO5300 410x502 QSPI AMOLED (via BSP: waveshare/esp_lcd_sh8601)
 *   Touch:      FT3168 capacitive touch via I2C (compatible with FT5x06 driver)
 *   Speaker:    ES8311 DAC codec via I2S
 *   Microphone: ES7210 ADC codec (quad-channel) via I2S (shared bus)
 *   IMU:        QMI8658 6-axis (accel + gyro) via I2C
 *   PMU:        AXP2101 power management via I2C
 *   RTC:        PCF85063 via I2C
 *   SD Card:    MicroSD via SPI
 *   Buttons:    BOOT (GPIO0) + PWR (GPIO10, AXP2101-managed)
 *   No:         Camera, IO Expander, RGB LED ring
 */

#pragma once

#include "driver/gpio.h"

// ============================================================================
// Board identification
// ============================================================================
#define BOARD_NAME "Waveshare ESP32-S3-Touch-AMOLED-2.06"
#define BOARD_MCU  "ESP32-S3"

// ============================================================================
// Board capability flags
// ============================================================================
#define BOARD_HAS_DISPLAY       1
#define BOARD_HAS_TOUCH         1
#define BOARD_HAS_KNOB          0
#define BOARD_HAS_CAMERA        0   // No DVP camera interface
#define BOARD_HAS_IO_EXPANDER   0   // No TCA9555 IO expander
#define BOARD_HAS_RGB_RING      0   // No WS2812 LED ring
#define BOARD_HAS_USER_BUTTONS  0   // No IO expander buttons
#define BOARD_HAS_BOOT_BUTTON   1   // BOOT button on GPIO0 (always present on ESP32-S3)
#define BOARD_HAS_IMU           1   // QMI8658 6-axis IMU
#define BOARD_RGB_LED_COUNT     0

// ============================================================================
// General I2C Bus (I2C1 — shared by PMU, audio codecs, touch, IMU, RTC)
// ============================================================================
#define BOARD_I2C_PORT          1
#define BOARD_I2C_SDA           GPIO_NUM_15
#define BOARD_I2C_SCL           GPIO_NUM_14
#define BOARD_I2C_FREQ          400000

// ============================================================================
// Display (CO5300 QSPI AMOLED 410x502 — managed by BSP)
// ============================================================================
#define BOARD_LCD_H_RES         410
#define BOARD_LCD_V_RES         502
// QSPI pins (BSP-managed, defined here for reference):
//   SIO0=GPIO4, SIO1=GPIO5, SIO2=GPIO6, SIO3=GPIO7
//   SCLK=GPIO11, CS=GPIO12, RST=GPIO8, TE=GPIO13
#define BOARD_LCD_BPP           16   // RGB565

// ============================================================================
// Touch Panel (FT3168 via I2C at 0x38 — managed by BSP esp_lcd_touch_ft5x06)
// ============================================================================
#define BOARD_TOUCH_I2C_ADDR    0x38
// RST=GPIO9, INT=GPIO38 (BSP-managed)

// ============================================================================
// Audio I2S (shared bus: speaker ES8311 + mic ES7210)
// ============================================================================
#define BOARD_I2S_NUM           1
#define BOARD_I2S_MCLK          GPIO_NUM_16
#define BOARD_I2S_SCLK          GPIO_NUM_41      // BCLK
#define BOARD_I2S_LRCK          GPIO_NUM_45      // WS
#define BOARD_I2S_DSIN          GPIO_NUM_42      // Data IN (from mic ES7210)
#define BOARD_I2S_DOUT          GPIO_NUM_40      // Data OUT (to speaker ES8311)
#define BOARD_AUDIO_SAMPLE_RATE 16000
#define BOARD_AUDIO_SAMPLE_BITS 16
#define BOARD_AUDIO_PA_PIN      GPIO_NUM_46      // PA (speaker amplifier) enable

// Audio codec I2C addresses (on I2C1)
#define BOARD_ES8311_ADDR       0x18             // Speaker DAC (7-bit)
#define BOARD_ES7210_ADDR       0x40             // Microphone ADC (7-bit, ES7210 default)
#define BOARD_AUDIO_MIC_GAIN    36.0f

// ============================================================================
// Buttons
// ============================================================================
#define BOARD_BOOT_BUTTON       GPIO_NUM_0       // On-SoC BOOT button (active low)
#define BOARD_PWR_BUTTON        GPIO_NUM_10      // Power button (AXP2101-managed)

// No IO expander buttons on this board;
// board_user_button_pressed() always returns false.
#define BOARD_IOEXP_BTN1        0   // unused
#define BOARD_IOEXP_BTN2        0   // unused
#define BOARD_IOEXP_BTN3        0   // unused
#define BOARD_IOEXP_PA_EN       0   // unused (PA controlled directly via GPIO46)
#define BOARD_IOEXP_LCD_RST     0   // unused (LCD reset BSP-managed)
#define BOARD_IOEXP_TOUCH_RST   0   // unused (touch reset BSP-managed)
#define BOARD_IOEXP_TOUCH_INT   0   // unused
#define BOARD_IOEXP_CAM_EN      0   // unused
#define BOARD_IOEXP_CAM_MUX     0   // unused
#define BOARD_IOEXP_SD_CS       0   // unused (SD CS on GPIO17)

// ============================================================================
// SD Card (SPI mode)
// ============================================================================
#define BOARD_SD_MOSI           GPIO_NUM_1
#define BOARD_SD_SCLK           GPIO_NUM_2
#define BOARD_SD_MISO           GPIO_NUM_3
#define BOARD_SD_CS             GPIO_NUM_17
#define BOARD_SD_SPI_HOST       SPI3_HOST   /* Separate from display QSPI on SPI2 */

// ============================================================================
// PMU (AXP2101 on I2C1 at 0x34)
// ============================================================================
#define BOARD_PMU_I2C_ADDR      0x34

// ============================================================================
// IMU (QMI8658 on I2C1 at 0x6B)
// ============================================================================
#define BOARD_IMU_I2C_ADDR      0x6B

// ============================================================================
// RTC (PCF85063 on I2C1 at 0x51)
// ============================================================================
#define BOARD_RTC_I2C_ADDR      0x51

// ============================================================================
// Camera — not available on this board (stubbed)
// ============================================================================
#define BOARD_CAM_XCLK          0
#define BOARD_CAM_XCLK_FREQ_HZ  0
#define BOARD_CAM_PCLK          0
#define BOARD_CAM_VSYNC         0
#define BOARD_CAM_HREF          0
#define BOARD_CAM_PWDN          GPIO_NUM_NC
#define BOARD_CAM_RESET         GPIO_NUM_NC
#define BOARD_CAM_SIOD          0
#define BOARD_CAM_SIOC          0
#define BOARD_CAM_D0            0
#define BOARD_CAM_D1            0
#define BOARD_CAM_D2            0
#define BOARD_CAM_D3            0
#define BOARD_CAM_D4            0
#define BOARD_CAM_D5            0
#define BOARD_CAM_D6            0
#define BOARD_CAM_D7            0
