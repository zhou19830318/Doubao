# Skill: ESP-IDF Build, Flash, and Configure

## Prerequisites
- ESP-IDF v5.5+ installed (currently at `C:\Espressif\frameworks\esp-idf-v5.5`)
- `idf.py` available in PATH (currently at `C:\bin\idf.py.cmd`)
- Device connected via USB (COM3 or COM4)

## Build Commands

### First-time setup
```bash
cd C:\Users\Omer\Dropbox\ESP2026\HeyClawy

# Set target chip
idf.py set-target esp32s3

# Configure project (opens menuconfig TUI)
idf.py menuconfig
```

### Build
```bash
idf.py build
```

### Flash
```bash
# Flash to device (auto-detect COM port)
idf.py -p COM4 flash

# Or specify baud rate for faster flashing
idf.py -p COM4 -b 921600 flash

# Flash only the app partition (faster for development)
idf.py -p COM4 app-flash
```

### Combined build + flash + monitor
```bash
idf.py -p COM4 flash monitor
```

## Configuration (menuconfig)

### Board Selection
Navigate to: `HeyClawy Application Configuration` → `Select target board`
- SenseCAP Watcher (default)
- Generic ESP32-S3 (future)

### OpenClaw Settings
Navigate to: `HeyClawy Application Configuration`
- OpenClaw Gateway host (configure in secrets.h)
- OpenClaw Gateway port (default: 18789)

### Key SDK Settings (sdkconfig.defaults)
- Flash: 8MB, QIO, 80MHz
- PSRAM: Octal, 80MHz
- Partition table: Custom (partitions.csv)
- FreeRTOS tick: 1000 Hz

## Verified Port Configuration (Feb 2026)
The SenseCAP Watcher connects via CH342 USB-UART:
- **COM3** = USB-Enhanced-SERIAL-B CH342 (flash port - CONFIRMED WORKING)
- **COM4** = USB-Enhanced-SERIAL-A CH342 (alternate serial)
- Set `ESPPORT=COM3` environment variable before flashing

**Note:** COM13/COM9 (USB JTAG/serial) are a different ESP32-C3 device, NOT the Watcher.

## Common Issues

### Flash fails with timeout
- Check COM port number (Device Manager → Ports)
- Use COM3 for SenseCAP Watcher (confirmed working)
- Try lower baud rate: `idf.py -p COM3 -b 460800 flash`
- Hold BOOT button on device while flashing (if available)

### Component not found
- Run `idf.py reconfigure` to re-resolve managed components
- Check `idf_component.yml` files for correct dependency versions

### Out of memory
- Increase partition size in `partitions.csv`
- Enable PSRAM usage: `CONFIG_SPIRAM=y`
- Check `CONFIG_SPIRAM_USE_MALLOC=y` for transparent PSRAM allocation

## Clean Build
```bash
idf.py fullclean
idf.py build
```
