# Skill: Serial Monitoring

## ⚠️ CRITICAL: RTS Pin Control
The CH342 USB-UART chip on the SenseCAP Watcher uses RTS for flow control. **You MUST set `rts=False`** when opening the serial port, otherwise the chip blocks serial output after the bootloader phase. This is the #1 cause of "serial stops after boot" issues.

## Basic Monitor
```bash
# Start serial monitor (default 115200 baud)
idf.py -p COM3 monitor

# Specify baud rate
idf.py -p COM3 -B 115200 monitor
```

## Monitor Key Shortcuts
- `Ctrl+]` — Exit monitor
- `Ctrl+T` → `Ctrl+H` — Show help
- `Ctrl+T` → `Ctrl+R` — Reset device
- `Ctrl+T` → `Ctrl+F` — Enter bootloader (for flashing)

## Combined Commands
```bash
# Build, flash, and monitor in one command
idf.py -p COM3 flash monitor

# Just flash app and monitor (faster iteration)
idf.py -p COM3 app-flash monitor
```

## Python Serial Access (Recommended)
Python serial is more reliable than `idf.py monitor` for automated use:
```python
import serial, time
port = serial.Serial('COM3', 115200, timeout=1)
port.dtr = False   # Prevent device reset
port.rts = False   # ⚠️ CRITICAL: CH342 blocks output if RTS is high!
port.reset_input_buffer()

# Read output
while True:
    data = port.read(port.in_waiting or 1)
    if data:
        print(data.decode('utf-8', errors='replace'), end='')
```

### Resetting device via serial:
```python
port.dtr = False
time.sleep(0.1)
port.dtr = True
time.sleep(0.1)
port.dtr = False
# ⚠️ Must also set rts=False after reset!
port.rts = False
```

## Sending Commands Over Serial
The device accepts commands via stdin (fgetc-based):
```python
# Send a command
port.write(b'say Hello world!\r\n')

# Available commands:
# talk / t      - Simulate knob press (voice chat)
# say <msg>     - Send text message to OpenClaw
# play / p      - Play TTS of last response
# details / d   - Request detailed response
# status / s    - Show WiFi/OpenClaw/heap/task status
# web / w       - Toggle web server on/off
# tasks         - Toggle tasks screen
# cron-add-test - Create test cron job
# cron-remove <id> - Remove cron job by UUID
# abort         - Abort current chat
# wake          - Wake from sleep
# reboot        - Restart device
```

## Interactive Serial Session (for Copilot CLI / write_powershell)

When using `write_powershell` to interact with the device, use this pattern:

### Python Script for Bidirectional Serial
```python
import serial, threading, sys, time
port = serial.Serial('COM3', 115200, timeout=0.1)
port.dtr = False
port.rts = False
time.sleep(0.5)

def reader():
    while True:
        data = port.read(4096)
        if data:
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()

t = threading.Thread(target=reader, daemon=True)
t.start()

while True:
    line = sys.stdin.readline()
    if line:
        port.write((line.strip() + '\r\n').encode())
```

### Launching and Sending Commands
1. Start with `powershell` `mode="async"` `shellId="ser"`
2. Wait 15s for boot output with `read_powershell`
3. Send commands with `write_powershell` input: `talk\n` (or `talk{enter}`)
4. **CRITICAL**: The `read_powershell` tool returns the FULL buffer since session start, not just new output. Scroll to the end for latest output.

### ⚠️ Command Duplication Bug
- `write_powershell` sends text to Python's stdin buffer
- If you send `talk` without a newline, it stays buffered
- Then sending `talk{enter}` on the next call results in `talktalk` being read by `readline()`
- **Fix**: Always include a newline in the same `write_powershell` call: use `talk\n` or `talk{enter}`
- Never send text without a newline terminator in one call and then send more text in the next call

### ⚠️ Device Reboot on Port Open
- Opening the serial port with DTR=True (default) reboots the device
- The Python script sets `dtr=False` after opening, but the brief DTR pulse still triggers a reboot
- This is normal — the device boots cleanly and reconnects to WiFi/OpenClaw within ~15s
- **Close the serial port before flashing** (stop_powershell) or idf.py will fail with permission denied

## Log Levels
The ESP-IDF log system uses these levels (set via menuconfig):
- `E` (Error) — Always shown
- `W` (Warning)
- `I` (Info) — Default level
- `D` (Debug)
- `V` (Verbose)

Set per-component log level at runtime:
```c
esp_log_level_set("board", ESP_LOG_DEBUG);
esp_log_level_set("heyclawy", ESP_LOG_VERBOSE);
```

## Identifying COM Port

**SenseCAP Watcher confirmed port: COM3** (USB-Enhanced-SERIAL-B CH342)

```powershell
# List serial ports on Windows
[System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
# Or with device names:
Get-WMIObject Win32_PnPEntity | Where-Object { $_.Name -match 'COM\d' } | Select-Object Name
```

### Known Port Map (Feb 2026)
- COM3: SenseCAP Watcher ESP32 console (CH342 SERIAL-B, Interface MI_02) ← USE THIS
- COM4: SenseCAP Watcher Himax AI camera (CH342 SERIAL-A, Interface MI_00, 921600 baud)
- COM13/COM9: Different ESP32-C3 device (USB JTAG, NOT the Watcher)

## Troubleshooting
- **Serial stops after bootloader**: Set `rts=False` — CH342 blocks output otherwise
- **Device enters download mode**: DTR toggle timing wrong — use the sequence: False→True→False with 100ms delays
- **Garbled output**: Wrong baud rate (115200 for ESP32, 921600 for Himax camera on COM4)
- **No response to commands**: Device might be in sleep mode — send `\r\n` to wake serial task
