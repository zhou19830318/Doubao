# IO Expander Power Control (PCA9535)

## Critical API Note

The `esp_io_expander_set_level()` function signature:
```c
esp_err_t esp_io_expander_set_level(esp_io_expander_handle_t handle, uint32_t pin_num_mask, uint8_t level);
```

**The `level` parameter is `uint8_t` (0 or 1), NOT a bitmask!**

- `level = 1` → ALL pins in `pin_num_mask` go HIGH
- `level = 0` → ALL pins in `pin_num_mask` go LOW

### Common Bug (CRITICAL)

```c
// ❌ WRONG — power_mask is a large uint32 value (e.g., 0xDB00)
// When cast to uint8_t, the lower 8 bits are 0x00 = OFF!
esp_io_expander_set_level(handle, power_mask, power_mask);

// ✅ CORRECT — pass 1 to set all masked pins HIGH
esp_io_expander_set_level(handle, power_mask, 1);
```

This bug caused the display to be completely black because `BOARD_IOEXP_PWR_LCD` (pin 9 = bit 9 = 0x200) was never powered on. The entire power_mask value 0xDB00 truncated to `uint8_t` = 0x00, turning all power rails OFF.

## SenseCAP Watcher Power Rails (IO Expander Port 1)

| Pin | Name | Description |
|-----|------|-------------|
| P1.0 (8) | PWR_SDCARD | SD card power |
| P1.1 (9) | PWR_LCD | LCD display power |
| P1.2 (10) | PWR_SYSTEM | System power |
| P1.3 (11) | PWR_AI | AI chip (Himax) power |
| P1.4 (12) | PWR_PA | Audio codec PA power |
| P1.5 (13) | PWR_BAT_DET | Battery detect (INPUT on original firmware) |
| P1.6 (14) | PWR_GROVE | Grove connector power |
| P1.7 (15) | PWR_BAT_ADC | Battery ADC power |

## Power-On Sequence

1. Set PWR_SYSTEM HIGH first
2. Wait 100ms
3. Set all other power rails HIGH
4. Wait 100ms for rails to stabilize

## Debugging Display Issues

If the display is completely black (no backlight):
1. Check IO expander power rails are properly enabled (this was the root cause)
2. Verify LEDC backlight on GPIO8 is outputting (duty > 0)
3. Check `esp_lcd_panel_disp_on_off(panel, true)` was called
4. Ensure `esp_lcd_panel_init()` completed successfully
