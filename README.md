# AIWatch — ESP32-S3 AI Voice Wearable Terminal

> **🌐 Language**: English · [简体中文](README_zh-CN.md)

**AIWatch** is an open-source, AI-powered wearable device firmware for the ESP32-S3 microcontroller. It connects to an **OpenClaw AI Gateway** via WebSocket to deliver end-to-end voice conversations, MP3 music playback, and remote device control — all through natural voice interaction.

> 📌 **Current Stage**: DVT (Design Verification Testing) — v0.1.0-beta

[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5%2B-blue)](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/index.html)
[![Platform](https://img.shields.io/badge/Platform-ESP32--S3-red)](https://www.espressif.com/en/products/socs/esp32-s3)

---

## 📷 Overview

AIWatch transforms an ESP32-S3 board into a voice-first AI companion. Wake it with a wake word (e.g. "你好小智"), speak naturally, and receive AI-powered responses spoken back via TTS. It's an open alternative to closed platforms like Rabbit R1 and Humane AI Pin — low cost, privacy-controllable, and fully hackable.

```
┌──────────────────────────────────────────────────────┐
│                    AIWatch Device                     │
│  ┌─────────┐  ┌──────────┐  ┌────────────────────┐  │
│  │  Wake   │  │   STT    │  │   OpenClaw Client  │  │
│  │  Word   │─▶│ (MiMo)   │─▶│   (WebSocket JSON) │──┼──▶ OpenClaw
│  │ (ESP-SR)│  │          │  │                    │  │   Gateway
│  └─────────┘  └──────────┘  └────────────────────┘  │
│       ▲                            │                 │
│       │                            ▼                 │
│  ┌────┴──────┐              ┌──────────┐            │
│  │  I2S Mic  │              │   TTS    │            │
│  │ (ES7210)  │              │ (MiMo)   │            │
│  └───────────┘              └────┬─────┘            │
│                                  │                   │
│                                  ▼                   │
│                            ┌──────────┐              │
│                            │ I2S Spkr │              │
│                            │ (ES8311) │              │
│                            └──────────┘              │
│                                                      │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────┐  │
│  │ LVGL UI  │  │ MP3      │  │  Web Server      │  │
│  │ (AMOLED) │  │ Player   │  │  (Config & API)  │  │
│  └──────────┘  └──────────┘  └──────────────────┘  │
└──────────────────────────────────────────────────────┘
```

---

## 🎯 Features

### Voice AI Conversation
- **Offline Wake Word**: ESP-SR WakeNet engine (e.g. "你好小智") or MultiNet custom phrase — engine and phrase are selectable via menuconfig, no cloud dependency
- **Speech-to-Text**: Xiaomi MiMo-V2.5-ASR via HTTP REST API (OpenAI-compatible)
- **Text-to-Speech**: Xiaomi MiMo-V2.5-TTS via HTTP SSE streaming — multi-voice support
- **Smart VAD**: Adaptive noise calibration, silence detection (1.5s default timeout, extended to 15s in continuous conversation mode), max 25s recording buffer
- **Device Commands**: AI can control the device — `[DEVICE:volume=80]`, `[DEVICE:brightness=70]`, `[DEVICE:mp3=play:song.mp3]`, and more

### User Interface
- **AMOLED Display**: 410×502 AMOLED panel with LVGL 9.x dark-theme UI (iOS / HarmonyOS style, pure black background)
- **Dynamic Island**: Top pill (250×44, rounded capsule) showing the current state label with a state-color glow border; the **WiFi icon** and **OpenClaw connection dot** flank it on the same row (inset 50px from the edges, aligned to the pill's vertical center so they stay clear of the rounded-corner clipping)
- **Per-State Content**: each of the 15 states shows at most 3 elements — main title + Chinese subtitle + one scrolling text line (state-mutually-exclusive, no overlap)
- **IDLE Clock**: 120px extra-bold clock with the Chinese date below (e.g. `2025年8月13日 星期三`)
- **Waveform Animation**: LISTENING state renders a 16-bar animated audio waveform
- **Marquee Scrolling**: one unified left-scrolling text line (10 px/s) shared by LLM response (white), STT transcript (yellow `#FF9F0A`) and TTS caption (blue `#0A84FF`)
- **MP3 Player UI**: full-screen overlay with coral-red glow ring, gradient album art and a song-selection mode (swipe to browse · double-tap to confirm)
- **Touch Input**: tap the MP3 overlay to toggle play/pause; swipe + double-tap to pick a song

### Audio Playback
- **MP3 Player**: Play MP3 files from microSD card (`/sdcard/mp3/`) with playlist support
- **I2S Audio**: Shared I2S bus with ES8311 DAC (speaker) and ES7210 ADC (dual microphone)
- **Priority Arbitration**: Listening > MP3 > TTS > Notifications — ensures clean audio switching

### Networking
- **WiFi**: Station mode with auto-reconnect + AP mode with captive portal for provisioning
- **Web Server**: HTTP configuration UI and REST API on port 80
- **SNTP**: Automatic time synchronization

### System
- **Settings**: Persistent configuration via NVS (non-volatile storage)
- **Error Logging**: Structured error tracking with severity levels
- **Memory Monitoring**: Periodic DRAM/PSRAM health checks
- **Power Management**: CPU frequency scaling (80–240MHz), configurable sleep timeout (`[DEVICE:sleep=N]` minutes), deep sleep via CLI
- **Serial CLI**: Interactive command-line interface via USB-C

---

## 🛠️ Supported Hardware

### Primary Target: Waveshare ESP32-S3-Touch-AMOLED-2.06

| Component | Specification |
|-----------|--------------|
| **MCU** | ESP32-S3R8 (dual-core Xtensa LX7 @ 240MHz) |
| **Flash** | 32MB (QSPI) |
| **PSRAM** | 8MB (Octal SPI) |
| **Display** | 410×502 QSPI AMOLED (CO5300 / SH8601 driver) |
| **Touch** | FT3168 capacitive touch (I2C) |
| **Audio DAC** | ES8311 (speaker output via I2S) |
| **Audio ADC** | ES7210 (4-ch microphone input via I2S) |
| **IMU** | QMI8658 6-axis (accelerometer + gyroscope) |
| **PMU** | AXP2101 power management |
| **RTC** | PCF85063 |
| **SD Card** | microSD (SPI mode) |
| **Buttons** | BOOT (GPIO0) + PWR (AXP2101) |

### Also Supported
- **Waveshare ESP32-S3-AUDIO-Board**: 1.47" IPS LCD (172×320), 7× WS2812 RGB ring, OV2640 camera hardware (no camera driver in current firmware), TCA9555 IO expander
- **Seeed Studio SenseCAP Watcher**: ESP32-S3, SPD2010 display (legacy support)
- **M5StickC Plus2**: ESP32-PICO-V3-02 (legacy support)

---

## 📋 Prerequisites

### Hardware
- Waveshare ESP32-S3-Touch-AMOLED-2.06 (or other supported board)
- USB-C cable for flashing and serial console
- microSD card (for MP3 files and chat history)
- Optional: Speaker connected to audio output

### Software
- **ESP-IDF v5.5+** with ESP32-S3 toolchain
  ```bash
  git clone -b v5.5 --recursive https://github.com/espressif/esp-idf.git
  cd esp-idf && ./install.sh esp32s3 && . ./export.sh
  ```

### External Services (API Keys Required)
- **OpenClaw Gateway**: Self-hosted AI gateway (WebSocket server)
- **Xiaomi MiMo API**: For STT (ASR) and TTS services — obtain API key from [platform.xiaomimimo.com](https://platform.xiaomimimo.com)

---

## 🚀 Quick Start

### 1. Clone the Repository

```bash
git clone <repo-url> AIWatch
cd AIWatch
```

### 2. Configure Build Target

```bash
# Ensure ESP32-S3 target
idf.py set-target esp32s3
```

### 3. Configure via Menuconfig

```bash
idf.py menuconfig
```

Navigate to **AIWatch Application Configuration**:
- Select `Waveshare ESP32-S3-Touch-AMOLED-2.06` as the target board
- Choose wake word engine (WakeNet recommended)
- Set OpenClaw Gateway host and port

### 4. Configure API Secrets

Create `main/include/secrets.h` from the example:

```bash
cp main/include/secrets.h.example main/include/secrets.h
```

Edit `secrets.h` to set:
- WiFi SSID and password
- MiMo API key for STT and TTS
- OpenClaw device credentials (ED25519 keys)

### 5. Prepare SD Card

Format a microSD card with FAT32 and create the directory structure:

```
/sdcard/
├── mp3/         # MP3 music files
└── notes/       # Daily chat history (auto-created)
```

### 6. Build & Flash

```bash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Or use the convenience build script:

```bash
./build_audio_board.sh
```

---

## 🧭 State Machine

The application operates as a 15-state finite state machine:

| # | State | Description |
|---|-------|-------------|
| 0 | `SLEEP` | Screen off / deep sleep (low power) |
| 1 | `ARMED` | System initialized, waiting |
| 2 | `BOOT` | System boot / initialization |
| 3 | `CONNECTING` | Connecting to OpenClaw |
| 4 | `IDLE` | Ready, waiting for wake word |
| 5 | `LISTENING` | Recording user speech |
| 6 | `SENDING` | Uploading audio to STT |
| 7 | `THINKING` | Waiting for AI response |
| 8 | `STREAMING` | Receiving streaming response |
| 9 | `RESPONSE` | Full response received |
| 10 | `TTS_LOADING` | Preparing TTS audio |
| 11 | `TTS_PLAYING` | Playing TTS audio |
| 12 | `PLAYING_MP3` | MP3 music playback |
| 13 | `NOTIFYING` | Notification from gateway |
| 14 | `ERROR` | Error condition |

**Normal flow**: BOOT → CONNECTING → IDLE → (wake word) → LISTENING → SENDING → THINKING → STREAMING → RESPONSE → TTS_LOADING → TTS_PLAYING → IDLE

---

## ⌨️ Serial CLI Commands

Connect via USB-C serial (115200 baud) and type `help` (aliases shown in parentheses):

| Command | Description |
|---------|-------------|
| `help` (`h`, `?`) | Show the AIWatch command reference |
| `status` (`s`) | Show device status |
| `talk` (`t`) | Start voice chat |
| `say <text>` | Send text to the AI |
| `abort` | Abort the current chat |
| `details` (`d`) | Request full details from OpenClaw |
| `play` (`p`) | Read the last response aloud |
| `quiet` (`q`) | Toggle ESP_LOG* suppression |
| `wake` (`w`) | Wake up the display |
| `deepsleep` | Enter deep sleep |
| `reboot` (`restart`) | Restart the device |
| `wifi <ssid> <pass>` | Set WiFi credentials |
| `web` | Toggle the web management server |
| `tasks` | Open the tasks screen |
| `cron-add-test` | Add a test cron job |
| `cron-remove <id>` | Remove a cron job |
| `mp3` | Open the MP3 player UI |
| `mp3list` | List MP3 files on SD card |
| `mp3play <file>` | Play an MP3 file |
| `mp3stop` | Stop MP3 playback |
| `mp3pause` | Pause MP3 playback |
| `mp3resume` | Resume MP3 playback |

---

## 🎮 Device Commands (AI-Controlled)

The AI can send device commands in the format `[DEVICE:command]`:

| Command | Description |
|---------|-------------|
| `volume=N` | Set speaker volume (0-100) |
| `brightness=N` | Set display brightness (0-100) |
| `rgb=off/on/rainbow/aurora/starfield/fire/ocean/R,G,B` | Control RGB LED ring (Audio Board only) |
| `sleep=N` | Set sleep timeout to N **minutes** (0 = never) |
| `reboot` | Reboot the device |
| `webserver=on/off` | Toggle web management server |
| `auto_read=on/off` | Toggle auto-read of AI responses |
| `mp3=play:<file>` | Play an MP3 file by name |
| `mp3=stop` | Stop MP3 playback |
| `mp3=pause` / `mp3=resume` | Pause / resume playback |
| `mp3=show` | Show the current playlist |
| `mp3=scan` (or `mp3=list`) | Rescan the SD card for MP3s |
| `chatlog=query:date` | Load chat history from a date (YYYY-MM-DD) and ask AI to summarize |
| `widget=<json>` / `card=<json>` | Reserved — ignored by the current firmware |

---

## 🏗️ Project Structure

```
AIWatch/
├── main/                          # Application entry point & core logic
│   ├── app_main.c                 # System init & main event loop
│   ├── app_state.c                # Global state, device cmd parsing, MP3 mgmt
│   ├── app_state_machine.c        # FSM & resource arbitration
│   ├── app_tasks.c                # Background tasks (knob, TTS, status, sleep)
│   ├── voice_chat.c               # Recording → STT → VAD pipeline
│   ├── serial_cmd.c               # Serial CLI REPL
│   ├── mem_monitor.c              # Memory monitoring
│   └── include/                   # App config, secrets, globals
│
├── components/                    # Reusable ESP-IDF component library
│   ├── board/                     # Hardware abstraction (multi-board support)
│   ├── wifi_manager/              # WiFi + captive portal
│   ├── openclaw/                  # OpenClaw WebSocket client (ED25519 auth)
│   ├── stt/                       # Speech-to-Text (MiMo ASR)
│   ├── tts/                       # Text-to-Speech (MiMo TTS)
│   ├── wake_word/                 # ESP-SR wake word detection
│   ├── ui/                        # LVGL user interface
│   ├── mp3_player/                # SD card MP3 player
│   ├── notes_manager/             # Chat history on SD card
│   ├── settings/                  # NVS persistent settings
│   ├── error_log/                 # Structured error logging
│   ├── webserver/                 # HTTP configuration server
│   ├── ed25519_lib/               # ED25519 crypto library
│   ├── esp_audio_codec/           # Audio codecs (MP3, AAC, FLAC, etc.)
│   ├── esp_audio_simple_player/   # GMF-based audio player pipeline
│   ├── gmf_core/                  # Generic Media Framework core
│   ├── gmf_audio/                 # GMF audio processing elements
│   ├── gmf_io/                    # GMF I/O modules
│   └── sscma_client/              # SSCMA / TensorFlow Micro client
│
├── partitions.csv                 # Flash partition table (32MB layout)
├── sdkconfig.defaults             # ESP-IDF SDK configuration defaults
├── CMakeLists.txt                 # Root CMake (ESP-IDF v5.5+)
└── build_audio_board.sh           # Convenience build script
```

---

## 📄 Flash Partition Layout (32MB)

| Partition | Type | Size | Purpose |
|-----------|------|------|---------|
| nvs | data | 24KB | Non-volatile settings storage |
| phy_init | data | 4KB | PHY calibration data |
| factory | app | 8MB | Application firmware |
| model | spiffs | 960KB | Wake word model files |
| storage | spiffs | 2MB | Application data & logs |

---

## 🔧 Key Dependencies

| Component | Version | Purpose |
|-----------|---------|---------|
| ESP-IDF | v5.5.3 | RTOS & peripheral framework |
| LVGL | v9.5.0 | Embedded GUI library |
| esp-sr | v1.9.5 | Speech recognition (WakeNet) |
| esp_lvgl_port | v2.8.0 | LVGL-ESP integration |
| esp_codec_dev | v1.5.10 | Audio codec device drivers |
| esp_websocket_client | v1.1.0 | WebSocket client |
| led_strip | v2.5.5 | WS2812 LED control |
| esp32_s3_touch_amoled_2_06 | v1.0.7 | Waveshare AMOLED BSP |
| esp_lcd_sh8601 | v2.0.0 | SH8601 AMOLED display driver |

---

## 🤝 Contributing

Contributions are welcome! Please follow these steps:

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

---

## 📝 License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

```
SPDX-FileCopyrightText: 2024-2026 AIWatch Contributors
SPDX-License-Identifier: MIT
```

---

## 🙏 Acknowledgments

- **Waveshare** — Excellent ESP32-S3 development boards and BSP
- **Espressif** — ESP-IDF framework and ESP-SR speech recognition
- **LVGL** — Light and Versatile Embedded Graphics Library
- **OpenClaw** — AI gateway protocol enabling multi-modal LLM interaction
- **Xiaomi MiMo** — Speech-to-text and text-to-speech API services

---

*Built with ❤️ by [AIWatch Contributors](https://github.com/zhou19830318)*
