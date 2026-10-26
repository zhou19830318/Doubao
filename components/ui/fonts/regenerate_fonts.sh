#!/bin/bash
# Regenerate CJK fonts for all three sizes: 12px (status bar), 16px (bubbles), 24px (large text/clock)
# Usage: ./regenerate_fonts.sh
#
# Requires: lv_font_conv (npm install -g lv_font_conv)
# Font source: SourceHanSansCN-Medium.otf (must be in this directory)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FONT_OTF="$SCRIPT_DIR/SourceHanSansCN-Medium.otf"

if [ ! -f "$FONT_OTF" ]; then
    echo "❌ Font file not found: $FONT_OTF"
    echo "   Download from: https://github.com/adobe-fonts/source-han-sans/raw/release/SubsetOTF/CN/SourceHanSansCN-Medium.otf"
    exit 1
fi

if ! command -v lvgl_font_conv &>/dev/null; then
    echo "Installing lvgl_font_conv..."
    npm install -g lvgl_font_conv 2>/dev/null || {
        echo "❌ Failed to install lvgl_font_conv. Install manually: npm install -g lvgl_font_conv"
        exit 1
    }
fi

# Symbol set: GB2312 Level 1 (3755 common Chinese) + ASCII + punctuation
# Extracted from the existing 16px font file header
SYMBOLS=$(python3 -c "
import re
with open('$SCRIPT_DIR/SourceHanSansCN_Medium_16.c') as f:
    header = f.readline()
    while header:
        if '--symbols' in header:
            m = re.search(r'--symbols (.+) --format', header)
            if m:
                print(m.group(1))
                break
        header = f.readline()
")

if [ -z "$SYMBOLS" ]; then
    echo "❌ Could not extract symbol list from existing font file"
    exit 1
fi

echo "Font: $FONT_OTF"
echo "Symbols: $(echo "$SYMBOLS" | wc -c) chars"
echo ""

# Generate each size
for SIZE in 12 16 24; do
    OUTFILE="$SCRIPT_DIR/SourceHanSansCN_Medium_${SIZE}.c"
    echo "Generating ${SIZE}px font → $(basename $OUTFILE) ..."

    lvgl_font_conv --bpp 2 --size $SIZE --no-compress --stride 1 --align 1 \
        --font "$FONT_OTF" \
        --symbols "$SYMBOLS" \
        --format lvgl -o "$OUTFILE"

    if [ -f "$OUTFILE" ]; then
        LINES=$(wc -l < "$OUTFILE")
        echo "  ✅ Done ($LINES lines)"
    else
        echo "  ❌ Failed to generate ${SIZE}px font"
        exit 1
    fi
done

echo ""
echo "✅ All three fonts generated successfully!"
echo "   12px: SourceHanSansCN_Medium_12.c (status bar)"
echo "   16px: SourceHanSansCN_Medium_16.c (bubble text)"
echo "   24px: SourceHanSansCN_Medium_24.c (large text/clock)"
