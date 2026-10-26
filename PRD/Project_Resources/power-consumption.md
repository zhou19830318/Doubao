# Power Consumption Analysis & Optimization Guide

## Battery Targets
- **M5StickCPlus2**: 120mAh battery → target >1 hour runtime
- **SenseCAP Watcher**: 400mAh battery → target >3 hours runtime
- **Waveshare Audio Board**: No battery (USB powered only)

## Power Budget Overview (ESP32-S3 @ 160MHz)

| Component | Active (mA) | Idle/Sleep (mA) | Notes |
|-----------|-------------|-----------------|-------|
| CPU (dual-core) | 30-50 | 30-50 | No true CPU sleep implemented |
| WiFi radio (TX) | 180-240 | 0 (modem sleep) | DTIM beacon wake ~100ms |
| WiFi radio (RX) | 95-100 | 0 (modem sleep) | |
| WiFi modem sleep | - | 5-15 | Between beacons |
| Display backlight | 20-40 | 0 | Off during light sleep |
| LCD controller | 5-10 | 5-10 | Still powered during sleep |
| Microphone (PDM) | 1-3 | 1-3 | Always on for wake word |
| ESP-SR (wake word) | 15-30 | 0 (when paused) | Neural net inference |
| Speaker codec | 5-10 | 1-2 | Quiescent |
| PSRAM | 3-5 | 3-5 | Always active |
| IO expander | 1-2 | 1-2 | SenseCAP only |
| RGB LED | 0-15 | 0 | Off during sleep |
| **Total estimate** | **~150-300** | **~50-90** | |

## Current Consumers (Ranked by Impact)

### 1. WiFi Radio — THE Dominant Consumer (~100-200mA active)

**Current state**: WiFi is the single biggest power drain. Every WebSocket message,
HTTP request, and ping forces the WiFi radio fully awake for TX/RX.

**Traffic sources (measured from logs)**:
| What | Interval (idle) | Interval (active) | WS Messages |
|------|----------------|-------------------|-------------|
| ~~WS Ping/Pong~~ | ~~1 second~~ → **30s** | ~~1s~~ → **30s** | 2 (ping+pong) |
| Health request | **Removed when idle** | 5s (fast poll) | 2 (req+res) |
| Usage.status | **Removed when idle** | 5s (fast poll) | 2 (req+res) |
| Cron.list | **60s** (was 15s in sleep) | 5s (fast poll) | 2 (req+res) |
| Server health push | ~30s (server-side) | ~30s | 1 (event) |
| Server agent events | On demand | On demand | 1 per event |

**Already implemented (this session)**:
- ✅ WS ping interval: 1s → 30s after auth (saves ~29 WiFi wakes/30s)
- ✅ Idle polling: removed health+usage requests (server pushes health)
- ✅ Sleep polling: reduced to cron.list only, every 60s (was health+tasks every 15s)

**Estimated savings**: ~60-80mA average during idle (30× fewer WiFi wakes)

### 2. Wake Word Detection — Continuous Mic + Neural Net (~15-30mA)

**Current state**: `wake_word_task` runs on Core 1 at priority 5, continuously:
1. Reads 512-sample chunks from mic via `board_audio_record()` (blocking I2S read)
2. Feeds chunks to ESP-SR WakeNet for inference
3. Never stops unless explicitly paused (during recording/TTS)

**On M5StickCPlus2**: Wake word is **disabled** (ESP-SR requires ESP32-S3).
So this only applies to SenseCAP and Waveshare Audio Board.

**Impact**: Mic ADC runs at 16kHz continuously + WakeNet neural net inference on every chunk.

### 3. CPU Always Running (~30-50mA)

**Current state**: "Light sleep" only turns off display/LED. CPU remains at 160MHz.
`CONFIG_PM_ENABLE` is NOT set — no automatic frequency scaling or CPU light sleep.

**Tasks running during "light sleep"**:
| Task | Interval | CPU Load |
|------|----------|----------|
| status_update | 500ms | Low (polls counters) |
| sleep_task | 5s | Minimal |
| knob_timer | 5ms (200Hz!) | ISR-like, very frequent |
| LVGL task | 5ms timer | Still running even with display off |
| serial_cmd | Continuous | Blocking fgetc |
| wake_word | Continuous | High (neural net) |
| WS client task | Event-driven | Waiting on socket |
| SSCMA tasks | Continuous | Camera monitoring |

### 4. Display (~25-50mA when on)

**Current state**: Backlight turns off during sleep (good). LCD controller stays powered.

### 5. SSCMA Camera Tasks (~5-10mA)

**Current state**: Two tasks (`sscma_client_process`, `sscma_client_monitor`) run
continuously on SenseCAP for AI camera monitoring, even when not in use.

---

## Optimization Suggestions (Ranked: Most to Least Low-Hanging Fruit)

### ✅ DONE — WS Ping Interval Relaxation
**Impact**: HIGH | **Effort**: Trivial | **Trade-off**: None
- Changed from 1s to 30s after auth completes
- 1s was only needed for challenge data flush during handshake
- Saves ~29 unnecessary WiFi TX/RX cycles per 30 seconds

### ✅ DONE — Idle Polling Reduction
**Impact**: MEDIUM | **Effort**: Trivial | **Trade-off**: None
- Removed `usage.status` and `health` requests during idle (server pushes health)
- Reduced sleep cron polling from 15s to 60s
- Idle polling now: tasks only, every 60s (was health+usage+tasks every 30s)

### ✅ DONE — WiFi Power Save MAX_MODEM
**Impact**: HIGH (~20-40mA savings) | **Effort**: Low | **Trade-off**: Slight latency
```c
esp_wifi_set_ps(WIFI_PS_MAX_MODEM);  // M5Stick: always MAX; SenseCap: MAX during sleep, MIN when active
```
- **M5Stick**: Always `MAX_MODEM` after WiFi connect (`wifi_manager.c` IP_EVENT handler)
- **SenseCap/Audio**: `MIN_MODEM` when active → `MAX_MODEM` when entering light sleep (`sleep_task`) → back to `MIN_MODEM` on wake (`app_reset_activity_timer`)
- Increases WiFi latency by ~100-300ms for incoming messages during sleep
- In practice: no real-time events expected during display-off sleep anyway

### ✅ DONE — Pause Wake Word During Light Sleep
**Impact**: MEDIUM (~15-25mA) | **Effort**: Low | **Trade-off**: Must press button to interact
- Wake word detection paused when entering light sleep (mic + neural net stopped)
- Controlled by `wake_word_in_sleep` setting (default: false = save battery)
- Resumed automatically when device wakes (button press, external activity, serial command)
- Users who prefer voice wake can enable via WebUI settings

### ✅ DONE — Reduce Knob Timer to 20ms (50Hz)
**Impact**: LOW-MEDIUM (~5mA from reduced ISR overhead) | **Effort**: Trivial | **Trade-off**: None
- Currently 5ms (200Hz) — way too fast for human hand rotation
- 20ms (50Hz) is perfectly responsive for UI knobs
- Can also stop the timer entirely during light sleep (knob not needed)
- **M5Stick**: No knob, not applicable

### ✅ DONE — Enable CONFIG_PM_ENABLE (CPU Frequency Scaling)
**Impact**: MEDIUM (~10-20mA) | **Effort**: Medium | **Trade-off**: Requires testing
- Enables automatic CPU frequency scaling based on load
- CPU drops to 80MHz when idle tasks dominate, ramps to 160MHz on demand
- Configured via `esp_pm_configure()` after board init
- `CONFIG_FREERTOS_HZ=1000` already set (required)
- `light_sleep_enable=false` to avoid WiFi disconnection

### ✅ DONE — Stop LVGL Task During Sleep
**Impact**: LOW (~3-5mA) | **Effort**: Medium | **Trade-off**: Minor
- `lvgl_port_stop()` called when entering light sleep
- `lvgl_port_resume()` called on wake (button, activity, serial)
- No display updates needed during sleep — timer+task fully paused

### ✅ DONE — Stop SSCMA Tasks When Not In Use
**Impact**: LOW-MEDIUM (~5-10mA) | **Effort**: Medium | **Trade-off**: Camera startup delay
- Camera initialization deferred to first use (not at boot)
- SSCMA `process_task` + `monitor_task` don't run until camera button pressed
- ~500ms startup delay on first camera capture (acceptable)
- Saves power continuously for majority of device lifetime

### ✅ DONE — Status Task Slow Polling During Sleep
**Impact**: LOW (~2-3mA) | **Effort**: Trivial | **Trade-off**: None
- Status update task: 500ms loop → 2000ms during sleep
- Skips all UI updates, RSSI, thinking timer during sleep
- Only checks: external activity wake, reconnect watchdog, task polling (60s)

### 7. Duty-Cycle WiFi During Deep Idle
**Impact**: VERY HIGH (~80-100mA) | **Effort**: HIGH | **Trade-off**: Loss of real-time
- Periodically disconnect WiFi, sleep CPU, reconnect every N minutes
- During sleep: no wake word, no display, no real-time events
- On wake: reconnect WiFi (2-3s), SNTP resync, WebSocket reconnect (1-2s)
- **Trade-off**: 5-10 second gap where device is unreachable
- **Use case**: Battery saver mode for overnight/extended idle
- Could be triggered by very long idle (>30 minutes)

### 8. Reduce CPU Frequency to 80MHz
**Impact**: MEDIUM (~10-15mA) | **Effort**: Low | **Trade-off**: Slower processing
```
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=80
```
- Halves CPU power but also halves processing speed
- Wake word detection may be slower (but should still work)
- TTS decoding slower (minimp3)
- **Trade-off**: Longer response times, may affect real-time audio
- **Not recommended** as default, but could be used in battery saver mode

---

## M5StickCPlus2 Specific Notes
- **No wake word** (ESP32, not S3) — saves ~20mA inherently
- **No camera** — saves ~5-10mA
- **Smaller display** (135×240 vs 412×412) — less backlight power
- **No knob timer** — one less periodic consumer
- **Buzzer speaker** — very low power vs codec amplifier
- **120mAh battery** at estimated ~80-120mA idle = 1-1.5 hours
- **Key wins**: WiFi MAX_MODEM + reduced polling → maybe 60-80mA → 1.5-2 hours

## SenseCAP Watcher Specific Notes
- **400mAh battery** at estimated ~100-150mA idle = 2.5-4 hours
- **Largest display** — backlight is significant power draw
- **Wake word active** — mic + ESP-SR always running (now paused during sleep)
- **Camera tasks** — SSCMA deferred to first use (was always running)
- **Key wins**: WiFi optimization + pause wake word + SSCMA lazy + LVGL stop + DFS → ~60-80mA idle, ~40-60mA sleep
- **Estimated runtime after optimizations**: 5-7 hours (idle), 8-10 hours (mostly sleeping)

## Power Measurement
To accurately measure, use a USB power meter (e.g., Ruideng UM34C) between
USB cable and device. Monitor:
- Boot + WiFi connect: ~200-300mA (peak)
- Idle connected: ~100-150mA (current baseline)
- Idle connected after optimizations: target ~60-90mA
- Light sleep (display off): target ~40-70mA
- TTS playback: ~150-200mA (WiFi + speaker + CPU)
- Recording: ~120-160mA (mic + WiFi)

## Implementation Priority
1. ✅ WS ping relaxation (done)
2. ✅ Idle polling reduction (done)
3. ✅ WiFi MAX_MODEM (done — M5Stick always, SenseCap dynamic with sleep)
4. ✅ Knob timer 50Hz (done)
5. ✅ Pause wake word during sleep (configurable via `wake_word_in_sleep` setting)
6. ✅ CONFIG_PM_ENABLE (CPU 160→80MHz DFS)
7. ✅ LVGL pause during sleep
8. ✅ SSCMA lazy init (camera deferred to first use)
9. ✅ Status task slow polling (2s during sleep)
10. Battery saver deep idle mode (major feature — future)
