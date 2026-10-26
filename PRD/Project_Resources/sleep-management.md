# Skill: Sleep Management

## Architecture
`sleep_task()` in `main/app_tasks.c` runs at priority 1 on core 0, polling every 5 seconds.

## How It Works
1. **Activity tracking**: `s_last_activity_us` (volatile int64_t) set by `app_reset_activity_timer()`
2. **Idle check**: Compares `esp_timer_get_time()` vs `s_last_activity_us + sleep_timeout_ms * 1000`
3. **Skip conditions**: Won't sleep if UI state is active (LISTENING, SENDING, THINKING, STREAMING, TTS_*)
4. **Sleep action**: Turns off display brightness (0) and RGB LED (RGB_MODE_OFF)
5. **Wake action**: Polls `board_knob_button_pressed()` in tight loop (100ms), restores brightness + LED on press

## Settings
- NVS key: `sleep_ms` (uint32_t, default: 60000 = 1 minute)
- Accessed via `settings_get()->sleep_timeout_ms`
- Configurable via web UI

## Activity Reset Points
- KNOB_PRESSED_BIT event in main loop
- DETAILS_BIT event in main loop
- On any state transition to active state

## Important Notes
- This is NOT ESP32 deep/light sleep — just display/LED off for power saving
- The MCU remains fully running (WiFi, WebSocket, etc. all active)
- For true deep sleep, would need to disconnect WiFi and use RTC wake sources
- The wheel button is on the IO expander (PCA9535 P0.3), polled via I2C

## Files
- `main/app_tasks.c` — sleep_task() + app_reset_activity_timer()
- `main/include/app_tasks.h` — app_reset_activity_timer() declaration
- `components/settings/` — sleep_timeout_ms field
