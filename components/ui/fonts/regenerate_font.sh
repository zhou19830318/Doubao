#!/bin/bash
# Regenerate Chinese font with --bpp 2 for anti-aliased rendering
# Usage: ./regenerate_font.sh

set -e

FONT_URL="https://github.com/adobe-fonts/source-han-sans/raw/release/SubsetOTF/CN/SourceHanSansCN-Medium.otf"
FONT_TTF="SourceHanSansCN-Medium.ttf"
FONT_C="SourceHanSansCN_Medium_16.c"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Install lvgl font converter if not available
if ! command -v lvgl_font_conv &>/dev/null; then
    echo "Installing lvgl_font_conv..."
    npm install -g lvgl_font_conv 2>/dev/null || \
    pip install lvgl-font-converter 2>/dev/null || {
        echo "Downloading prebuilt binary..."
        wget -q "https://github.com/lvgl/lv_font_conv/releases/latest/download/lv_font_conv-linux-x86_64" -O /tmp/lv_font_conv
        chmod +x /tmp/lv_font_conv
        alias lvgl_font_conv=/tmp/lv_font_conv
    }
fi

# Download font if not present
if [ ! -f "$SCRIPT_DIR/$FONT_TTF" ]; then
    echo "Downloading SourceHanSansCN-Medium.otf..."
    wget -q "$FONT_URL" -O "$SCRIPT_DIR/$FONT_TTF"
fi

echo "Regenerating font with --bpp 2 (4-level anti-aliasing)..."
cd "$SCRIPT_DIR"

# Options explanation:
#   --bpp 2       = 2 bits per pixel (4 gray levels) for smooth Chinese chars
#   --size 16     = 16px (matches current usage)
#   --no-compress = keep bitmap_format=0 for LVGL 8.x compatibility
#   --stride 1    = byte-aligned stride
#   --align 1     = no alignment padding
#   --symbols     = include all the characters from the original font spec

lvgl_font_conv --bpp 2 --size 16 --no-compress --stride 1 --align 1 \
    --font "$FONT_TTF" \
    --symbols "$(python3 -c "
import re
# Extract the symbol list from the original .c file header
with open('SourceHanSansCN_Medium_16.c') as f:
    header = f.readline()
    while header:
        if '--symbols' in header:
            m = re.search(r'--symbols (.+) --format', header)
            if m:
                print(m.group(1))
                break
        header = f.readline()
")" \
    --format lvgl -o "SourceHanSansCN_Medium_16.new.c"

if [ -f "SourceHanSansCN_Medium_16.new.c" ]; then
    mv "SourceHanSansCN_Medium_16.c" "SourceHanSansCN_Medium_16.bak.c"
    mv "SourceHanSansCN_Medium_16.new.c" "SourceHanSansCN_Medium_16.c"
    echo "✅ Font regenerated successfully!"
    echo "   Backup saved as SourceHanSansCN_Medium_16.bak.c"
    echo "   New font uses --bpp 2 for anti-aliased Chinese text"
else
    echo "❌ Failed to regenerate font"
    exit 1
fi
