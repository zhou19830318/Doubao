# STT Language Detection

## Overview
The STT (Speech-to-Text) service runs on the OpenClaw server as a custom Python script using `faster-whisper`. It transcribes audio sent from the ESP32 device and returns the detected language.

## Architecture
- **ESP32 side**: `components/stt/stt_client.c` — records audio, builds WAV, sends HTTP POST to STT server
- **Server side**: `~/.openclaw/tools/stt_server.py` on the OpenClaw machine (`<OC_HOST>:5051`)
- **Model**: faster-whisper with CTranslate2 backend

## Critical Bug History
The STT server originally had `language="en"` hardcoded in the `model.transcribe()` call, which forced English-only transcription regardless of input language. This was fixed by:
1. Removing the `language=` parameter entirely (lets faster-whisper auto-detect)
2. Upgrading model from `"tiny"` to `"small"` for better multilingual accuracy
3. Increasing `beam_size` from 1 to 5
4. Adding `ensure_ascii=False` to JSON response for proper UTF-8 output
5. Adding `language_probability` field to response

## STT Server Configuration
```python
# Key parameters in stt_server.py
model = WhisperModel("small", device="cpu", compute_type="int8")
# In transcribe call — do NOT set language= parameter for auto-detection
segments, info = model.transcribe(tmp_path, beam_size=5)
```

## STT Server Management
```bash
# SSH to OpenClaw server
ssh <SSH_USER>@<OC_HOST>  # see secrets.txt for credentials

# Check if running
curl http://<OC_HOST>:5051/health

# Find PID
ps aux | grep stt_server

# Restart
kill <old_pid>
nohup python3 ~/.openclaw/tools/stt_server.py > /tmp/stt_server.log 2>&1 &

# Check logs
tail -f /tmp/stt_server.log
```

## Response Format
```json
{
  "text": "שלום, מה שלומך היום?",
  "language": "he",
  "language_probability": 0.704
}
```

## Non-Latin Display Handling
LVGL's Montserrat fonts only cover Latin characters (U+0000-U+024F). For non-Latin responses:
- `ui_sanitize_text()` strips characters the font can't render
- `extract_short_response()` shows `LV_SYMBOL_OK` ("✓") as fallback when text is mostly non-Latin
- Full response text is preserved in `s_full_response` for TTS playback
- Sub-label shows "Tap ▶ to hear" to guide user to TTS

### `has_non_latin()` function
Checks if a string contains characters outside the Latin Unicode range. Used to detect when the response needs the TTS fallback UI.

### `is_renderable_2byte()` function
Checks if a 2-byte UTF-8 codepoint falls within the Montserrat font's renderable range (Latin Extended-A/B). Characters outside this range are stripped.

## Testing Language Detection
```bash
# Convert test audio to WAV
ffmpeg -y -i tools/HebrewQuestion.m4a -ar 16000 -ac 1 -sample_fmt s16 tools/HebrewQuestion.wav

# Play audio while device records (Python script)
# 1. Send 'talk' serial command
# 2. After 0.5s delay, play audio via ffplay with volume boost
# 3. Device's silence detection captures the audio
# 4. Check serial logs for "lang=he" in STT response
```

## Future Enhancement
Add a custom LVGL font with Hebrew/Arabic glyphs using `lv_font_conv` tool:
```bash
npm install lv_font_conv
lv_font_conv --font NotoSansHebrew-Regular.ttf -r 0x0590-0x05FF --size 28,36,48 --format lvgl --bpp 4 -o hebrew_font.c
```
This would allow direct display of Hebrew text on the device.
