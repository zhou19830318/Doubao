# Hebrew/RTL Font Rendering on LVGL

## Problem
LVGL's built-in Montserrat fonts only cover Latin + FontAwesome symbols. Hebrew (U+0590-U+05FF), Arabic, CJK, and other non-Latin scripts are NOT supported. Attempting to display Hebrew text with Montserrat results in invisible/empty output.

## Solution: Custom Font Generation

### Prerequisites
```bash
npm install -g lv_font_conv
```

### Generate Hebrew Font
```bash
# Download NotoSansHebrew-Regular.ttf from Google Fonts
# Place in tools/fonts/

# Generate LVGL C font file (28pt example)
lv_font_conv --no-compress --no-prefilter --bpp 4 \
  --size 28 --font tools/fonts/NotoSansHebrew-Regular.ttf \
  --range 0x0590-0x05FF,0x20-0x7E \
  --format lvgl -o components/ui/lv_font_hebrew_28.c \
  --lv-include lvgl.h

# 36pt version
lv_font_conv --no-compress --no-prefilter --bpp 4 \
  --size 36 --font tools/fonts/NotoSansHebrew-Regular.ttf \
  --range 0x0590-0x05FF,0x20-0x7E \
  --format lvgl -o components/ui/lv_font_hebrew_36.c \
  --lv-include lvgl.h
```

### Integration Steps

1. **Add font files to CMakeLists.txt:**
   ```cmake
   # components/ui/CMakeLists.txt
   set(SRCS ... "lv_font_hebrew_28.c" "lv_font_hebrew_36.c")
   ```

2. **Enable BiDi in sdkconfig.defaults:**
   ```
   CONFIG_LV_USE_BIDI=y
   CONFIG_LV_BIDI_BASE_DIR_DEF=0
   ```

3. **Declare fonts in ui.c:**
   ```c
   LV_FONT_DECLARE(lv_font_hebrew_28);
   LV_FONT_DECLARE(lv_font_hebrew_36);
   ```

4. **Detect Hebrew text:**
   ```c
   static bool is_hebrew_2byte(uint8_t b0, uint8_t b1) {
       uint16_t cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
       return (cp >= 0x0590 && cp <= 0x05FF);
   }
   ```

5. **Apply Hebrew font + RTL direction per-object:**
   ```c
   if (has_hebrew(text)) {
       lv_obj_set_style_base_dir(label, LV_BASE_DIR_RTL, 0);
       lv_obj_set_style_text_font(label, &lv_font_hebrew_28, 0);
   }
   ```

## Critical Bug: Font Reset Order

**WRONG** (Hebrew rendered with Latin font = invisible):
```c
ui_set_response(NULL, text);         // Sets Hebrew font
app_set_state(UI_STATE_RESPONSE);    // ui_set_state() RESETS font to Montserrat!
```

**CORRECT** (Hebrew font survives):
```c
app_set_state(UI_STATE_RESPONSE);    // Resets font first
ui_set_response(NULL, text);         // Sets Hebrew font LAST — stays correct
```

Also: `ui_set_state()` must skip font reset for states that preserve the response text (RESPONSE, TTS_LOADING, TTS_PLAYING):
```c
if (state != UI_STATE_RESPONSE && state != UI_STATE_TTS_LOADING &&
    state != UI_STATE_TTS_PLAYING) {
    lv_obj_set_style_text_font(s_big_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_base_dir(s_big_label, LV_BASE_DIR_LTR, 0);
}
```

## Text Sanitization

Two sanitizers exist:
- `ui_sanitize_text()` — strips all non-Latin characters (Hebrew → gone)
- `sanitize_text_hebrew()` — keeps Hebrew + Latin characters

Use `has_hebrew()` to choose which sanitizer to apply before display.

## Font Sizes
- 36pt: short Hebrew text (≤12 bytes) — fills circle display nicely
- 28pt: longer Hebrew text (>12 bytes) — prevents overflow
- Generated files: ~42KB (28pt) and ~58KB (36pt)

## Fallback Font
Hebrew fonts have Montserrat set as `.fallback` in the generated C files. This ensures digits (0-9), punctuation (!?.,), and Latin characters render correctly when mixed with Hebrew text. Without fallback, these characters appear as rectangles because NotoSansHebrew may not include all Latin glyphs.

## Nikud / Vowel Marks
LVGL does NOT properly render Hebrew combining characters (nikud/cantillation marks U+0591-U+05CF). These appear as separate rectangles between letters. `sanitize_text_hebrew()` strips them, keeping only:
- Hebrew letters: U+05D0-U+05EA
- Hebrew ligatures: U+05F0-U+05F4
- ASCII printable: U+0020-U+007E
- Latin extended: via `is_renderable_2byte()`

## TTS Hebrew Voice
EdgeTTS's "alloy" voice returns HTTP 500 for Hebrew text. `tts_speak()` auto-detects Hebrew (UTF-8 lead bytes 0xD6-0xD7) and switches to `he-IL-HilaNeural`. This is hardcoded in `components/tts/tts_client.c`.

## File Locations
- Source font: `tools/fonts/NotoSansHebrew-Regular.ttf`
- Generated: `components/ui/lv_font_hebrew_28.c`, `lv_font_hebrew_36.c`
