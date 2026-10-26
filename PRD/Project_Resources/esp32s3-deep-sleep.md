# Skill: ESP32-S3 Deep Sleep on SenseCAP Watcher

## Wake Sources
- **Light sleep**: GPIO41/42 (encoder A/B) via `gpio_wakeup_enable()` — fast ~200μs wakeup
- **Deep sleep**: Only GPIO0-15 are RTC-capable. GPIO41/42 CANNOT wake from deep sleep
- **GPIO2** (BOARD_IO_EXP_INT = PCA9535 interrupt pin) IS RTC-capable → knob button press wakes

## Implementation

### Deep Sleep — Proper Sequence (board_prepare_deep_sleep)
```c
// 1. Wait for knob button release (user is holding long-press)
// 2. Power off peripherals (LCD, AI, SD, PA, Grove, BAT_ADC)
// 3. Reconfigure non-wake input pins as OUTPUTS:
//    P0.0 (CHRG_DET), P0.1 (STDBY_DET), P0.2 (VBUS_DET),
//    P0.4 (SD_DET), P0.5 (TOUCH_INT), P0.6 (SSCMA_SYNC)
//    Only P0.3 (KNOB_BTN) remains as input for wake
// 4. Read all inputs to clear PCA9535 INT latch
// 5. Verify GPIO2 is HIGH
// 6. Enable RTC pull-up, configure ext0 wakeup on GPIO2
// 7. esp_deep_sleep_start()
```

### ⚠️ CRITICAL: Why Pin Direction Change is Required
- PCA9535 INT fires on ANY **input** pin change (not output pins)
- After powering off peripherals, floating pins cause spurious INT
- **The key fix**: reconfigure all input pins EXCEPT the wake button as outputs
- This prevents floating pins from triggering GPIO2 LOW → immediate wake
- Pin directions are restored automatically on reboot via `board_io_expander_init()`
- Additionally: must wait for button RELEASE before starting — releasing the long-press button would trigger INT

### Previous Failed Approaches
1. Just powering off peripherals (factory firmware approach) — NOT enough, floating pins still trigger INT
2. Only reading input registers to clear INT — clears momentarily but new changes happen during sleep entry
3. IO expander read-clearing loop — doesn't prevent new interrupts from floating pins

### Sleep Flow
1. Idle timeout → light sleep (display off, LED off, CPU running)
2. Long wheel press (4s) → deep sleep (CPU off, GPIO2 wake only)
3. Knob button press from deep sleep → full reboot → BOOT → IDLE

## Testing Notes
- **Serial/JTAG always wakes ESP32-S3 from deep sleep** (known HW limitation)
- Test deep sleep by closing serial port after sending `deepsleep` command
- Wake cause logged at boot: `esp_sleep_get_wakeup_cause()` → EXT0/TIMER/etc.
- `deepsleep` serial command available for testing without physical button
- GPIO2 level is logged before `esp_deep_sleep_start()` — must be HIGH (1)

## Important Notes
- WiFi stays connected during light sleep (CPU running)
- Deep sleep loses all state — full reboot on wake
- `app_is_sleeping()` function lets main loop know to ignore button actions (wake only)
- Touch and knob events reset `s_last_activity_us` via `app_reset_activity_timer()`
