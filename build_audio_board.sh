#!/usr/bin/env bash
# Build AIWearable for Waveshare ESP32-S3-AUDIO-Board
# Target: ESP32-S3, 16MB flash, USB JTAG console

set -e
cd "$(dirname "$0")"

echo "============================================"
echo " AIWearable - Waveshare ESP32-S3-AUDIO-Board"
echo "============================================"

# Check if we need to switch to ESP32-S3 or switch board
NEED_SWITCH=0
if [ ! -f sdkconfig ]; then
    NEED_SWITCH=1
elif ! grep -q "CONFIG_HEYCLAWY_BOARD_WAVESHARE_AUDIO=y" sdkconfig 2>/dev/null; then
    NEED_SWITCH=1
fi

if [ "$NEED_SWITCH" -eq 1 ]; then
    echo "Switching to ESP32-S3 / Waveshare Audio target. Cleaning build..."
    rm -rf build sdkconfig
    mkdir -p build
    : > build/CMakeCache.txt
    idf.py set-target esp32s3
    if [ $? -ne 0 ]; then
        echo "ERROR: idf.py set-target esp32s3 failed"
        exit 1
    fi
fi

# Ensure board selection is Waveshare Audio
if ! grep -q "CONFIG_HEYCLAWY_BOARD_WAVESHARE_AUDIO=y" sdkconfig 2>/dev/null; then
    echo "Updating board selection to Waveshare Audio..."
    sed -i \
        -e 's/CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER=y/# CONFIG_HEYCLAWY_BOARD_SENSECAP_WATCHER is not set/' \
        -e 's/CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2=y/# CONFIG_HEYCLAWY_BOARD_M5STICKCPLUS2 is not set/' \
        -e 's/# CONFIG_HEYCLAWY_BOARD_WAVESHARE_AUDIO is not set/CONFIG_HEYCLAWY_BOARD_WAVESHARE_AUDIO=y/' \
        sdkconfig
fi

# Fix flash size for Waveshare Audio Board (16MB)
sed -i \
    -e 's/CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y/CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y/' \
    -e 's/CONFIG_ESPTOOLPY_FLASHSIZE="8MB"/CONFIG_ESPTOOLPY_FLASHSIZE="16MB"/' \
    sdkconfig

echo "Board: Waveshare ESP32-S3-AUDIO-Board (ESP32-S3, 16MB flash)"
echo ""

# Auto-detect serial port if not set
if [ -z "$ESPPORT" ]; then
    ESPPORT=$(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -1)
    if [ -n "$ESPPORT" ]; then
        echo "Auto-detected serial port: $ESPPORT"
        export ESPPORT
    fi
fi

idf.py build

if [ $? -eq 0 ]; then
    echo ""
    echo "============================================"
    echo " BUILD SUCCESSFUL"
    echo " Flash with: export ESPPORT=${ESPPORT:-/dev/ttyUSB0} && idf.py flash monitor"
    echo "============================================"
else
    echo ""
    echo "BUILD FAILED"
    exit 1
fi
