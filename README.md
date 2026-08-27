# Doubao Voice Robot

> **Language**: English | [简体中文](README_CN.md)

An open-source AI voice companion robot based on ESP32-S3, featuring full-duplex real-time voice conversation with Doubao (ByteDance) AI via a single WebSocket connection.

[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5%2B-blue)](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/index.html)
[![Platform](https://img.shields.io/badge/Platform-ESP32--S3-red)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Stage](https://img.shields.io/badge/Stage-DVT-orange)]()

---

## Overview

Doubao Voice Robot transforms an ESP32-S3 board into a voice-first AI companion. It connects to [Volcengine Doubao](https://www.volcengine.com/product/doubao) real-time voice API via a single WebSocket connection for end-to-end voice conversations — ASR + LLM + TTS in one connection.

```
graph LR
+----------------------------------------------------------------------+
|                            Doubao Voice Robot                        |
+----------------------------------------------------------------------+
|                                                                      |
|   [Wake Word]  --->  [Doubao Chat]  <--->  [Doubao Voice API] ---> [Cloud]
|   (ESP-SR)           Engine                  (WSS Full-Duplex)       |
|       ^                  ^                          |                |
|       |                  |                          v                |
|   [I2S Mic]          [VAD]                    [I2S Speaker]          |
|   (ES7210)           (16kHz)                  (ES8311)               |
|                                                                      |
|   [LVGL UI]  [MP3 Player]  [Web Config]  [Wake Word ("你好小智")]     |
|   (AMOLED)                                                           |
+----------------------------------------------------------------------+
```

---

## Key Features

### Full-Duplex Voice Conversation
- **Single WSS Connection**: ASR + LLM + TTS all in one WebSocket connection to Doubao API
- **Real-time Interrupt**: Speak during AI response to interrupt — local energy detection with 6dB margin
- **Continuous Dialogue**: Auto-continue listening after response, configurable idle timeout (default 8s)
- **Multi-turn Context**: Server-side 20-turn context management

### Wake Word & VAD
- **Offline Wake Word**: ESP-SR WakeNet engine — "你好小智" (configurable)
- **Smart VAD**: Adaptive noise calibration, silence detection (1.5s default, 15s max)
- **Feedback Sounds**: "Ding-dong" on wake, "Dong-ding" on cancel

### Touch & Button Interaction
- **Single Tap**: Start dialogue / Interrupt playback
- **Double Tap**: Stop playback
- **Long Press 1s**: Open settings page
- **BOOT Button**: Talk / Cancel / Deep sleep

### Audio
- **Full-Duplex I2S**: Simultaneous capture (ES7210) and playback (ES8311) at 16kHz
- **MP3 Player**: SD card MP3 playback with playlist support
- **3:2 Resampler**: Server outputs 24kHz, hardware plays at 16kHz — polyphase resampling

### Device Control
- **AI-Controlled**: AI can adjust volume, brightness, play music, control RGB LEDs via `[DEVICE:]` commands
- **Voice Commands**: "Set volume to 50" → `[DEVICE:volume=50]`

### UI
- **AMOLED Display**: 410×502 AMOLED with LVGL 9.x dark theme
- **Chat Bubbles**: User (right) and AI (left) bubbles with streaming text
- **State Indicator**: Color-coded status (green=IDLE, red=LISTENING, orange=THINKING, etc.)

---

## Hardware

### Primary Target: Waveshare ESP32-S3-Touch-AMOLED-2.06

| Component | Specification |
|-----------|--------------|
| MCU | ESP32-S3R8 (dual-core Xtensa LX7 @ 240MHz) |
| Flash | 32MB (QSPI) |
| PSRAM | 8MB (Octal SPI) |
| Display | 410×502 QSPI AMOLED (CO5300/SH8601) |
| Touch | FT3168 capacitive (I2C) |
| Audio DAC | ES8311 (speaker) |
| Audio ADC | ES7210 (4-ch microphone) |
| IMU | QMI8658 6-axis |
| PMU | AXP2101 |
| SD Card | microSD (SPI) |
| Button | BOOT (GPIO0) |

### Also Supported
- Waveshare ESP32-S3-AUDIO-Board (1.47" IPS LCD, 7× WS2812 RGB ring)
- M5StickC Plus2 (legacy)
- SenseCAP Watcher (legacy)

---

## Quick Start

### Prerequisites

- **ESP-IDF v5.5+** with ESP32-S3 toolchain
- **Doubao API Key** from [Volcengine Console](https://console.volcengine.com/)
- Waveshare ESP32-S3-Touch-AMOLED-2.06 (or supported board)
- USB-C cable, microSD card (FAT32)

### Build & Flash

```bash
# Clone the repository
git clone <repo-url> doubao-voice-robot
cd doubao-voice-robot

# Set target
idf.py set-target esp32s3

# Configure (select board, wake word, etc.)
idf.py menuconfig

# Build
idf.py build

# Flash & monitor
idf.py -p /dev/ttyACM0 -b 921600 flash monitor
```

### First-Time Configuration

1. **WiFi**: Device starts in AP mode if no WiFi configured
   - Connect to `Doubao_Config` hotspot
   - Browser opens captive portal → enter WiFi credentials
   
2. **API Key**: Via web interface or serial CLI
   ```bash
   # Serial CLI
   Doubao> web    # Start web server
   # Then open http://<device-ip> in browser
   ```

3. **Wake Word**: Say "你好小智" to start a conversation

---

## State Machine

The application operates as a **15-state finite state machine** with two layers:

### UI States (Legacy State Machine)

| State | Color | Description |
|-------|-------|-------------|
| `SLEEP` | Dark gray | Deep sleep mode |
| `ARMED` | Indigo | Wake word armed |
| `BOOT` | Orange | System booting |
| `CONNECTING` | Blue | WiFi/WSS connecting |
| `IDLE` | Green | Ready, waiting for wake word |
| `LISTENING` | Red | Recording user speech |
| `SENDING` | Blue | Uploading audio |
| `THINKING` | Orange | Waiting for AI response |
| `STREAMING` | Purple | Receiving streaming text |
| `RESPONSE` | Green | Response displayed |
| `TTS_LOADING` | Cyan | Loading audio |
| `TTS_PLAYING` | Green | Playing AI response |
| `PLAYING_MP3` | Cyan | SD card MP3 playback |
| `NOTIFYING` | Orange | Notification |
| `ERROR` | Red | Error condition |

### Internal Chat States (Doubao Chat Engine)

| State | Description |
|-------|-------------|
| `CHAT_IDLE` | Waiting for start |
| `CHAT_LISTENING` | Mic capturing, VAD active |
| `CHAT_COMMITTING` | Commit sent, waiting for response |
| `CHAT_THINKING` | Server processing (transcript received) |
| `CHAT_SPEAKING` | Audio delta being played |
| `CHAT_AUTO_LISTEN` | Response done, auto-continue listening |

### Conversation Flow

```
IDLE ──(wake word/tap)──▶ LISTENING ──(VAD silence)──▶ COMMITTING
                                                         │
                          AUTO_LISTEN ◀──(audio done)◀── SPEAKING
                               │                           ▲
                               │ (user speaks)             │
                               └───────────────────────────┘
                               
IDLE ◀──(timeout 8s)── AUTO_LISTEN
IDLE ◀──(exit intent/20000002)── SPEAKING
IDLE ◀──(double tap/interrupt)── any state
```

### Resource Arbitration

| Resource | Owner States | Priority |
|----------|--------------|----------|
| `RES_AUDIO_IN` (I2S RX) | LISTENING | Highest |
| `RES_AUDIO_OUT` (I2S TX) | LISTENING, TTS_PLAYING, PLAYING_MP3, NOTIFYING | Preemptive |

Higher-priority states can preempt lower-priority states' resources.

---

## Configuration

### Via Web Interface (Recommended)

1. Start web server: `Doubao> web` or `[DEVICE:webserver=on]`
2. Open `http://<device-ip>` in browser
3. Configure: WiFi, API Key, Voice, Speed, Loudness, System Prompt

### Via Serial CLI

```bash
Doubao> help              # Show all commands
Doubao> status            # Device status
Doubao> talk              # Start voice chat
Doubao> say Hello AI      # Send text to AI
Doubao> doubao status     # Connection status
Doubao> reboot            # Restart device
```

### Via Voice Commands

| Command | Description |
|---------|-------------|
| "音量调到50" | Set volume to 50% |
| "调亮一点" | Increase brightness |
| "播放音乐" | Show playlist / play music |
| "停止音乐" | Stop MP3 playback |

---

## AI Device Commands

The AI can control the device via `[DEVICE:command]` tags:

| Command | Description |
|---------|-------------|
| `[DEVICE:volume=0-100]` | Set volume |
| `[DEVICE:brightness=0-100]` | Set brightness |
| `[DEVICE:mp3=play:<file>]` | Play MP3 file |
| `[DEVICE:mp3=stop]` | Stop playback |
| `[DEVICE:mp3=show]` | Show playlist |
| `[DEVICE:mp3=index:N]` | Play by index |
| `[DEVICE:rgb=rainbow/aurora/fire/ocean]` | RGB LED control |
| `[DEVICE:sleep=N]` | Set sleep timeout (minutes) |
| `[DEVICE:reboot]` | Reboot device |
| `[DEVICE:webserver=on/off]` | Toggle web server |
| `[DEVICE:auto_read=on/off]` | Toggle auto-read TTS |

---

## Project Structure

```
doubao-voice-robot/
├── main/                          # Application core
│   ├── app_main.c                 # System init & event loop
│   ├── app_state.c                # Global state, device commands
│   ├── app_state_machine.c        # FSM & resource arbitration
│   ├── app_tasks.c                # Background tasks
│   ├── doubao_chat.c              # Conversation engine (VAD, interrupt)
│   ├── serial_cmd.c               # Serial CLI
│   ├── mem_monitor.c              # Memory monitoring
│   └── include/                   # Headers
│
├── components/                    # Reusable components
│   ├── board/                     # Hardware abstraction (multi-board)
│   ├── doubao_voice/              # Doubao WSS full-duplex client
│   ├── ui/                        # LVGL user interface
│   ├── wifi_manager/              # WiFi + captive portal
│   ├── settings/                  # NVS persistent settings
│   ├── webserver/                 # HTTP config server
│   ├── wake_word/                 # ESP-SR wake word detection
│   ├── mp3_player/                # SD card MP3 player
│   ├── notes_manager/             # Chat history on SD card
│   ├── error_log/                 # Error logging
│   └── esp_websocket_client/      # Patched WebSocket client
│
├── docs/                          # Documentation
├── PRD/                           # Product requirements
├── partitions.csv                 # Flash partition table
├── sdkconfig.defaults             # SDK configuration
└── CMakeLists.txt                 # Build system
```

---

## Flash Partition Layout (32MB)

| Partition | Size | Purpose |
|-----------|------|---------|
| nvs | 24KB | Settings storage |
| phy_init | 4KB | PHY calibration |
| factory | 8MB | Application firmware |
| model | 960KB | Wake word model (WakeNet) |
| storage | 2MB | App data & logs |

---

## Dependencies

| Component | Version | Purpose |
|-----------|---------|---------|
| ESP-IDF | v5.5.5 | RTOS & peripherals |
| LVGL | v9.5.0 | GUI library |
| esp-sr | v1.9.5 | Wake word (WakeNet) |
| esp_lvgl_port | v2.6.0 | LVGL-ESP integration |
| esp_codec_dev | v1.5.0 | Audio codec drivers |
| doubao_voice | custom | Doubao WSS client |

---

## License

MIT License - see [LICENSE](LICENSE) for details.

```
SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
SPDX-License-Identifier: MIT
```

---

## Acknowledgments

- **[Volcengine Doubao](https://www.volcengine.com/product/doubao)** — Real-time voice dialogue API
- **[Espressif](https://www.espressif.com/)** — ESP-IDF framework and ESP-SR
- **[Waveshare](https://www.waveshare.com/)** — ESP32-S3 development boards
- **[LVGL](https://lvgl.io/)** — Embedded graphics library

---

*Built with ❤️ by Doubao Contributors*
