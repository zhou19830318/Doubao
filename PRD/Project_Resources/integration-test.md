# Integration Test Checklist

**Run after every major feature implementation** to verify no regressions.

## Devices
| Device | COM Port | Chip | Build File |
|--------|----------|------|------------|
| SenseCAP Watcher | COM3 | ESP32-S3 | `build_sensecap.bat` |
| M5StickCPlus2 | COM17 | ESP32 | `build_m5stick.bat` |
| Waveshare Audio | not connected | ESP32-S3 | `build_audio_board.bat` |

---

## Step 1: Build All 3 Boards

```powershell
# Build in correct order (bat files auto-switch targets)
cmd /c build_sensecap.bat
cmd /c build_m5stick.bat       # Will clean and switch to ESP32
cmd /c build_audio_board.bat   # Will clean and switch to ESP32-S3
```

**CRITICAL: bat file target-switching issue**

The bat files use `idf.py set-target` which requires a CMake build dir. If `build/` was just cleaned,
you'll get: `"Directory doesn't seem to be a CMake build directory"`.

**Fix**: The bat files create a fake `build/CMakeCache.txt` before calling `set-target`. If a bat file
fails with set-target error, manually:
```powershell
New-Item -ItemType Directory -Force build | Out-Null
"" | Out-File build\CMakeCache.txt
cmd /c "idf.py set-target esp32"   # or esp32s3
```

**CRITICAL: never flash the wrong binary to a device!**
- SenseCap: `--chip esp32s3`, bootloader at `0x0`, includes `srmodels.bin` at `0x310000`
- M5Stick: `--chip esp32`, bootloader at `0x1000`, no srmodels
- Check flash command from build output before flashing!

---

## Step 2: Flash Connected Devices

```powershell
# Flash SenseCap (build with build_sensecap.bat FIRST)
python -m esptool --chip esp32s3 -p COM3 -b 460800 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m \
  0x0 build\bootloader\bootloader.bin \
  0x8000 build\partition_table\partition-table.bin \
  0x10000 build\heyclawy.bin \
  0x310000 build\srmodels\srmodels.bin

# Flash M5Stick (build with build_m5stick.bat FIRST)
python -m esptool --chip esp32 -p COM17 -b 460800 --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 8MB --flash_freq 80m \
  0x1000 build\bootloader\bootloader.bin \
  0x8000 build\partition_table\partition-table.bin \
  0x10000 build\heyclawy.bin
```

---

## Step 3: Boot Verification (Serial Monitor)

Monitor each device for ~50 seconds after flash. Expected log sequence:

```
Board: <board name>
I2C / IO expander initialized
Audio codec initialized
WiFi STA started...
Connected, IP: 192.168.1.xxx
WiFi PS: [MIN_MODEM or MAX_MODEM]
SNTP time sync started
Time synced (attempt N)
OpenClaw client initialized
WebSocket connected
Got connect.challenge
Sent connect request
Snapshot: v20xx.x.x up=Nm ...
OpenClaw connected
Health: ok=1 agent=main ...
All tasks started. Ready.
```

**Red flags to check:**
- `E (xxx) ESP_TLS: ...` — TLS cert error (OpenClaw host/port wrong)
- `E (xxx) openclaw: WS disconnected...` repeatedly — connection failure
- `Guru Meditation Error` — crash/exception (check stack trace)
- No `Time synced` — SNTP failing (check WiFi)
- `CONFIG_IDF_TARGET_ESP32S3=y` in M5Stick build — wrong target (rebuild)

---

## Step 4: Voice Query Test (End-to-End)

### SenseCap
1. Wait for "READY" on display
2. Say "Hey Jarvis" near device (or long-press knob)
3. **Expected**: Display → RECORDING → LED pulse green
4. Ask "What time is it?" and stop talking
5. **Expected**: THINKING → concentric arc spinner visible → 
6. **Expected**: RESPONSE → text visible → TTS plays (hear response)
7. **Expected**: Back to READY

### M5Stick
1. Wait for display to show "READY"
2. Press button A (big front button)
3. **Expected**: Display → RECORDING state → sub_label shows "A=cancel"
4. Ask "What time is it?" 
5. Press A or release → sends to STT
6. **Expected**: THINKING → two-arc spinner visible
7. **Expected**: RESPONSE → text visible → TTS plays
8. **Expected**: Back to READY

### Webcam microphone test (for TTS verification)
```python
# Save as test_mic.py and run to capture 10s of audio from webcam mic
import sounddevice as sd, scipy.io.wavfile as wav
fs = 44100
duration = 10
print("Recording...")
audio = sd.rec(int(duration*fs), samplerate=fs, channels=1, device="Microphone (HD Webcam eMeet C960)")
sd.wait()
wav.write("test_capture.wav", fs, audio)
print("Saved to test_capture.wav")
```

---

## Step 5: Sleep/Wake Test

### SenseCap
1. Leave device idle for 60+ seconds
2. **Expected**: Display dims → turns off → LED off
3. **Expected**: Serial shows `Idle for Xs, entering light sleep` + `WiFi PS: MAX_MODEM`
4. Rotate knob — **Expected**: Display wakes, LED on, `WiFi PS: MIN_MODEM`
5. Long-press knob (4s) — **Expected**: `Deep sleep requested` → device unresponsive
6. Press knob button briefly — **Expected**: cold boot, normal startup

### M5Stick  
1. Leave idle for 60+ seconds → display off
2. Press button A or C to wake

---

## Step 6: Web Server Test

### SenseCap
1. Say `web` in serial terminal → web server starts
2. Open `http://192.168.1.125` in browser (IP from serial log)
3. Verify Dashboard, Device, OpenClaw, Tasks, Errors tabs load

### M5Stick
1. Double-press button B to toggle web server
2. Open device IP in browser

---

## Step 7: OpenClaw Reconnect Test

1. Restart OpenClaw gateway: `ssh user@192.168.1.155 "kill $(pgrep openclaw)"`
2. Within 30s, OpenClaw restarts
3. **Expected**: Device detects disconnect, shows "Connecting..." → reconnects
4. **Expected**: Within ~20s: `WebSocket connected` + `OpenClaw connected`

---

## Step 8: Activity Display Test (SenseCap Only)

1. Send a command that takes time (e.g., "search for something online")
2. While processing: **Expected** THINKING spinner with detail text updating
3. **Expected**: carousel rotates every 3s if multiple runs active
4. Response: **Expected** RESPONSE state with short text + full TTS plays

---

## Step 9: Notification Test

1. Ask OpenClaw to remind you in 1 minute
2. After ~1 minute:
3. **Expected**: Notification state → amber LED → response displayed/spoken

---

## Common Issues & Fixes

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| "Connecting..." forever | OpenClaw host/port wrong | Check settings, verify `ws://192.168.1.155:18789` reachable |
| "Processing" stuck >30s | Stale run detection | Health event with last_activity_sec≥30 clears it; check OpenClaw is responding |
| No TTS audio | `auto_read_response` off or codec issue | Check settings, verify ES8311 init in logs |
| Wrong chip error on flash | Built for wrong target | Rebuild with correct bat file first |
| High pitch noise (M5Stick) | I2S config mismatch | Verify sample rate and bit depth in board.c |
| Display white/black | LVGL buffer in PSRAM | Verify DMA buffers use internal RAM |
| WiFi PS: MIN_MODEM never switches | Sleep task not running | Check `sleep_task` in app_tasks.c logs |

---

## Build Artifacts Verification

After build, check binary sizes are reasonable:
- SenseCap: ~1.7MB binary (ESP32-S3 with LVGL + ESP-SR)
- M5Stick: ~1.5MB binary (ESP32 without ESP-SR)
- Audio Board: ~1.6MB binary (ESP32-S3 with ESP-SR, no LVGL)

Partition table must have space: `0x300000` (3MB) for app partition.
If binary > 3MB: check for bloat, may need to increase partition.
