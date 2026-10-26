# Waveshare ESP32-S3-AUDIO-Board Support

## Overview
Second supported hardware target for HeyClawy. Screenless board with 7-LED RGB ring, dual microphone array, and ES8311 speaker codec. Uses BOOT button for primary interaction.

## Hardware
- **MCU**: ESP32-S3-WROOM-1 (16MB Flash, 8MB PSRAM Octal)
- **Speaker**: ES8311 DAC (I2C addr 0x18 7-bit)
- **Mic**: ES7210 4-channel ADC (I2C addr 0x40 7-bit, uses MIC1+MIC2)
- **RGB**: 7× WS2812 ring on GPIO38
- **IO Expander**: TCA9555 at 0x20 (compatible with PCA9535 driver)
- **I2C**: SDA=GPIO11, SCL=GPIO10
- **I2S**: MCLK=12, SCLK=13, LRCK=14, DIN=15, DOUT=16
- **Buttons**: BOOT (GPIO0) + 3 IO expander buttons (P1.1-P1.3)
- **PA Enable**: IO expander P1.0 (pin 8) — must enable for speaker output
- **USB**: Native USB JTAG/serial (not CH342)

## Serial Port
- **COM16**: Native USB JTAG/serial (VID 303A:1001)
- **COM7**: Debug interface (Interface 2)
- **Console**: Needs `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` for serial input via monitor (currently uses UART default which doesn't work for serial commands over USB JTAG)

## Build Configuration
```bash
# Switch to Waveshare board in sdkconfig:
# Change CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER to not set
# Set CONFIG_HEYCLAWY_BOARD_WAVESHARE_AUDIO=y
# Change flash size to 16MB

# Or use menuconfig:
idf.py menuconfig
# → HeyClawy Application Configuration → Select target board → Waveshare

# Flash:
ESPPORT=COM16 idf.py flash monitor
```

## ES7210 I2C Address ⚠️ CRITICAL
The `audio_codec_i2c_cfg_t.addr` field expects an **8-bit I2C address** (shifted left by 1).
- Define: `BOARD_ES7210_ADDR 0x40` (7-bit)
- Usage: `.addr = BOARD_ES7210_ADDR << 1` → sends 0x80
- Default constant: `ES7210_CODEC_DEFAULT_ADDR = 0x80` (already 8-bit)
- Mic selection: `ES7210_SEL_MIC1 | ES7210_SEL_MIC2` (NOT `ES7210_INPUT_MIC1`)

## Capability Flags
```c
#define BOARD_HAS_DISPLAY       0   // No LCD
#define BOARD_HAS_TOUCH         0
#define BOARD_HAS_KNOB          0   // No rotary encoder
#define BOARD_HAS_CAMERA        0   // No SSCMA camera
#define BOARD_HAS_IO_EXPANDER   1   // TCA9555
#define BOARD_HAS_RGB_RING      1   // 7-LED ring
#define BOARD_HAS_USER_BUTTONS  1   // BOOT + 3 IO expander
```

## Conditional Compilation Approach
- **Cannot use Kconfig variables in CMakeLists.txt** at `idf_component_register` time — ESP-IDF processes components in TWO passes; sdkconfig vars are empty during the first (enumeration) pass
- **Solution**: All components register with full dependency lists; conditional code handled via `#if BOARD_HAS_*` preprocessor guards in C source files
- **Font files**: Hebrew LVGL fonts wrapped in `#if BOARD_HAS_DISPLAY` to avoid compiling LVGL glyph data for screenless boards
- **Camera**: Inline stubs via `#if !BOARD_HAS_CAMERA` at top of camera.c

## IO Expander Pin Map (TCA9555)
| Pin | Function | Direction |
|-----|----------|-----------|
| P0.0 | LCD RST | Output |
| P0.1 | Touch RST | Output |
| P0.2 | Touch INT | Input |
| P0.3 | SD CS | Output |
| P0.5 | Camera EN | Output |
| P0.6 | Camera MUX | Output |
| P1.0 (8) | PA Enable | Output |
| P1.1 (9) | Button 1 | Input |
| P1.2 (10) | Button 2 | Input |
| P1.3 (11) | Button 3 | Input |

## Known Issues
- First WiFi connection attempt sometimes fails with WS timeout; auto-reconnects
- Serial console input doesn't work via `idf.py monitor` because console is configured for UART but device uses USB JTAG (need sdkconfig change for USB JTAG console)
- I2C pull-up warning at boot is cosmetic (still works)

## Reference Projects
- **xiaozhi-esp32**: `C:\Users\Omer\Dropbox\ESP\xiaozhi\xiaozhi-esp32\main\boards\waveshare-esp-box-2` — pin definitions, codec config
- **Waveshare wiki**: https://www.waveshare.com/wiki/ESP32-S3-AUDIO-Board
- ⚠️ The AMOLED demo projects are NOT for this board (different hardware)
