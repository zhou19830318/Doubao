# Wake Word Detection (ESP-SR WakeNet)

## Overview
Wake word detection uses **WakeNet** (ESP-SR's purpose-built wake word engine) with pre-trained models.
Default: **"Hey Jarvis"** (`wn9_jarvis_tts`). Change model via `idf.py menuconfig` → ESP Speech Recognition → WakeNet.

**⚠️ MultiNet is NOT suitable for wake words** — it's designed for multi-word speech commands (3+ words) and fails on short phrases like "Hi Clawy" or "Hello".

## Available WakeNet Models (pre-trained, free)
| Kconfig Option | Wake Phrase | Model |
|---|---|---|
| `SR_WN_WN9_JARVIS_TTS` | **Hey Jarvis** (default) | wn9_jarvis_tts |
| `SR_WN_WN9_HIESP` | Hi ESP | wn9_hiesp |
| `SR_WN_WN9_ALEXA` | Alexa | wn9_alexa |
| `SR_WN_WN9_COMPUTER_TTS` | Hey Computer | wn9_computer_tts |
| `SR_WN_WN9_HEYWILLOW_TTS` | Hey Willow | wn9_heywillow_tts |
| `SR_WN_WN9_HIJASON_TTS2` | Hi Jason | wn9_hijason_tts2 |
| `SR_WN_WN9_HITELLY_TTS` | Hi Telly | wn9_hitelly_tts |
| `SR_WN_WN9_HEYWANDA_TTS` | Hey Wanda | wn9_heywanda_tts |
| `SR_WN_WN9_MYCROFT_TTS` | Mycroft | wn9_mycroft_tts |
| `SR_WN_WN9_SOPHIA_TTS` | Sophia | wn9_sophia_tts |
| `SR_WN_WN9_HIJOY_TTS` | Hi Joy | wn9_hijoy_tts |
| `SR_WN_WN9_HILILI_TTS` | Hi Lily | wn9_hilili_tts |
| `SR_WN_WN9_ASTROLABE_TTS` | Astrolabe | wn9_astrolabe_tts |
| `SR_WN_WN9_CUSTOMWORD` | (custom) | For Espressif-trained custom model |

## Custom Wake Word ("Hi Clawy")
To get a custom model, open a GitHub issue at https://github.com/espressif/esp-sr/issues:
1. Request TTS-based WakeNet training for your phrase (e.g., "Hi Clawy")
2. Espressif generates model from TTS samples (free, takes 2-3 weeks)
3. Set `CONFIG_SR_WN_WN9_CUSTOMWORD=y` and place model in partition

## Architecture
- **Component**: `components/wake_word/` with `esp-sr` managed component dependency
- **Model storage**: SPIFFS partition named `model` (960KB, WakeNet model ~290KB)
- **Detection task**: Runs on CPU1, priority 5, 4KB stack
- **Audio format**: 16kHz mono, chunk size determined by model (~512 samples)
- **Audio buffer**: Allocated in PSRAM

## Key APIs
```c
esp_err_t wake_word_init(void);           // Load model from SPIFFS
esp_err_t wake_word_start(EventGroupHandle_t eg, EventBits_t bit);
void wake_word_pause(void);               // Pause during recording/TTS
void wake_word_resume(void);              // Resume after recording/TTS
void wake_word_stop(void);                // Stop and cleanup
bool wake_word_is_running(void);          // Check if active
const char *wake_word_get_phrase(void);   // Get human-readable phrase
```

## I2S Bus Sharing
Wake word and voice recording share the same I2S mic. **MUST pause wake word before recording and resume after**:
```c
wake_word_pause();
voice_chat_start();  // Uses mic for STT
wake_word_resume();
```

## sdkconfig Settings
```
# WakeNet model — "Hey Jarvis" (change via menuconfig → ESP Speech Recognition)
CONFIG_SR_WN_WN9_JARVIS_TTS=y

# Disable MultiNet (saves ~3.8MB flash)
CONFIG_SR_MN_CN_NONE=y
CONFIG_SR_MN_EN_NONE=y
```

## Partition Table
```
model, data, spiffs, , 960K,   # 960KB for WakeNet model (~290KB)
```

## Memory Impact
- Flash: ~290KB WakeNet model + ~200KB esp-sr code
- RAM: ~50-100KB for inference (internal SRAM)
- PSRAM: audio buffer only
- Fits on both 8MB (SenseCAP) and 16MB (Waveshare) boards

## Why MultiNet Failed
MultiNet (`mn6_en`) is a **speech command recognition** engine:
- Trained on 3-6 word utterances ("Turn on the light", "Play my music")
- Has 6-second timeout window (speech start → match → reset)
- G2P phoneme conversion works, but detection model can't reliably match short phrases
- Even common single words like "Hello" fail to trigger
- The xiaozhi project uses it for Chinese commands where phonemic patterns differ

## Troubleshooting
- **Model load fails**: Check partition table has `model` partition (960KB+), verify `esp_srmodel_init("model")` succeeds
- **No detections**: Verify mic is working (`talk` command), check model is loaded in logs
- **Too many false detections**: Use `DET_MODE_95` (default, most restrictive)
- **I2S errors**: Ensure wake word is paused before any other I2S operation
