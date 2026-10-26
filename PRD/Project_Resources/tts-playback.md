# TTS Playback via EdgeTTS

## Overview
The TTS component (`components/tts/`) provides text-to-speech via an EdgeTTS-compatible HTTP API. It sends text and receives MP3 audio that is decoded on-device using minimp3 and played through the speaker.

## Configuration
Settings in `main/include/secrets.h`:
- `SECRETS_TTS_HOST` — EdgeTTS server IP (configure in secrets.h)
- `SECRETS_TTS_PORT` — HTTP port (default: 5050)
- `SECRETS_TTS_API_KEY` — API key (can be placeholder)
- `SECRETS_TTS_VOICE` — Voice name (default: "alloy")

## API
```c
#include "tts_client.h"

// Initialize once at startup
tts_config_t cfg = {
    .host = "<TTS_HOST>",
    .port = 5050,
    .api_key = "your_api_key_here",
    .voice = "alloy",
    .model = "tts-1",
};
tts_init(&cfg);

// Blocking call — run in a separate task
esp_err_t err = tts_speak("Hello world");

// Check/stop playback
bool playing = tts_is_playing();
tts_stop();
```

## HTTP Request
```
POST /v1/audio/speech
Content-Type: application/json
Authorization: Bearer <api_key>

{"model":"tts-1","input":"<text>","voice":"alloy"}
```
Note: Default response format is MP3 (not WAV). Do NOT specify `response_format`.

## Playback Flow
1. HTTP POST to EdgeTTS server → receive full MP3 into PSRAM buffer
2. minimp3 decodes MP3 frames (24kHz mono 48kbps typical)
3. PCM resampled from 24kHz → 16kHz (board I2S sample rate)
4. Audio streamed to `board_audio_play()` in chunks
5. Stop flag checked between frames for interruption

## ⚠️ CRITICAL: Memory Architecture (DMA Conflict Fix)
The TTS task MUST use a PSRAM stack to avoid stealing internal DMA memory from the LCD SPI driver.

**Problem**: The ESP32-S3 has limited internal DMA-capable RAM (~264KB). The LCD SPI driver needs 32KB DMA buffers for `panel_io_spi_tx_color`. If the TTS task stack is in internal RAM (default), it consumes 16KB of DMA-capable memory, causing `Mem alloc fail` when LVGL flushes the display.

**Solution**: Create TTS task as a persistent static task with PSRAM-allocated stack:
```c
// TCB MUST be in internal RAM (FreeRTOS assertion: xPortCheckValidTCBMem)
StaticTask_t *tts_tcb = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
// Stack CAN be in PSRAM
StackType_t *tts_stack = heap_caps_calloc(1, 16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
xTaskCreateStaticPinnedToCore(tts_play_task, "tts_play", 16384, NULL, 4, tts_stack, tts_tcb, 1);
```

**Key rules**:
- TCB (`StaticTask_t`) → MUST be `MALLOC_CAP_INTERNAL` (FreeRTOS asserts valid TCB memory)
- Stack (`StackType_t[]`) → Use `MALLOC_CAP_SPIRAM` (frees internal DMA RAM)
- Stack size: 16KB minimum (minimp3 `mp3dec_decode_frame` uses ~8-10KB stack for IMDCT/synth tables)
- 8KB stack causes `BREAK instr` / DoubleException (stack overflow in minimp3)
- `mp3dec_t` struct (~12KB) → already on PSRAM heap via `heap_caps_calloc`
- Task pinned to core 1 to avoid competing with LVGL/WiFi on core 0

**The TTS task is persistent** — created once at startup, waits on `TTS_PLAY_BIT` event group forever. No dynamic create/delete.

## Integration with App State Machine
- `UI_STATE_RESPONSE` → user presses wheel → `TTS_PLAY_BIT` event
- Persistent `tts_play_task` wakes on `TTS_PLAY_BIT`, does playback, returns to wait
- State: RESPONSE → TTS_LOADING → (plays) → IDLE
- LED: Teal during TTS

## Tested Results
- "Hey!" → 8,352 bytes MP3 → 22,272 PCM samples, ~2s total (download + decode + play)
- "00:56" → 14,112 bytes MP3 → 37,632 PCM samples, ~3s total
- Response time from EdgeTTS: ~700ms-1s
- No DMA crashes with PSRAM stack solution

## Serial Command
Send `play` over serial to trigger TTS of the last response.

## Notes
- MP3 format is used (not WAV) — minimp3 decodes on-device
- EdgeTTS doesn't validate the API key in some configurations
- Short text (<5 words) works best; long text should be chunked (not yet implemented)
- PCM buffers, resample buffers, MP3 download buffer all allocated in PSRAM
- Resampling: linear interpolation from 24kHz → 16kHz
