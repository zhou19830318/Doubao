# Skill: Adding New Hardware Targets

## Overview
HeyClawy uses a board abstraction layer (HAL) with capability flags to support multiple ESP32-based hardware targets. Each board has its own pin definitions, capability flags, and conditional initialization code.

## Architecture
- Single `board.c` with `#if defined(CONFIG_HEYCLAWY_BOARD_*)` sections for board-specific code
- Capability flags: `BOARD_HAS_DISPLAY`, `BOARD_HAS_TOUCH`, `BOARD_HAS_KNOB`, `BOARD_HAS_CAMERA`, `BOARD_HAS_IO_EXPANDER`, `BOARD_HAS_RGB_RING`, `BOARD_HAS_USER_BUTTONS`
- Inline no-op stubs in `board.h` for unsupported features
- UI/camera/font code wrapped in `#if BOARD_HAS_*` preprocessor guards

## ⚠️ CRITICAL: CMake Kconfig Limitation
**Cannot use Kconfig variables in CMakeLists.txt** at `idf_component_register` time. ESP-IDF processes components in TWO cmake passes:
1. **Component enumeration** (early) — sdkconfig vars are **empty**
2. **Component build** (late) — sdkconfig vars are available ("y")

**Consequence**: All components must register with ALL dependencies. Use `#if` guards in C code for conditional compilation, NOT CMake `if(CONFIG_*)` around `idf_component_register`.

## Steps to Add a New Board

### 1. Create Board Header
Create `components/board/include/board_<name>.h` with:
- Pin definitions (GPIO numbers)
- Peripheral bus assignments (SPI, I2C, UART, I2S)
- Capability flags (BOARD_HAS_*)
- Board identification constants (BOARD_NAME, BOARD_MCU)

### 2. Add Kconfig Option
Edit `main/Kconfig.projbuild`:
```
config HEYCLAWY_BOARD_<NAME>
    bool "Board Display Name"
    help
        Description of the board
```

### 3. Add Board Include Guard
Edit `components/board/include/board.h`:
```c
#elif defined(CONFIG_HEYCLAWY_BOARD_<NAME>)
#include "board_<name>.h"
```

### 4. Add Board-Specific Code in board.c
Use `#if defined(CONFIG_HEYCLAWY_BOARD_<NAME>)` for:
- I2C bus addresses/pins
- IO expander configuration
- Power-on sequence
- Codec initialization (mic ADC type)
- PA enable logic

### 5. Handle Missing Features
For features the board doesn't have, set capability flag to 0. The inline stubs in `board.h` handle the rest:
```c
#define BOARD_HAS_DISPLAY 0  // → board_display_init() returns ESP_OK
#define BOARD_HAS_CAMERA  0  // → camera stubs (no SSCMA code compiled)
```

## Required Board API Functions
Core (all boards): `board_init()`, `board_power_off()`, `board_reboot()`, `board_rgb_init()`, `board_audio_init()`

Optional (via capability flags): display, touch, LVGL, knob, camera, user buttons

## Example: Screenless Board
See `board_waveshare_audio.h` — sets DISPLAY=0, TOUCH=0, KNOB=0, CAMERA=0. All UI calls become no-ops. BOOT button mapped to KNOB_PRESSED_BIT via `board_boot_button_pressed()`.
