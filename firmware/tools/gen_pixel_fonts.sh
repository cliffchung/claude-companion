#!/bin/sh
# MaruMinya's 12px grid: body/small 24px (2x), titles 36px (3x).
# Retain existing C symbol names to avoid changing every UI caller.
# Usage: gen_pixel_fonts.sh [path/to/x12y12pxMaruMinya.ttf]
# Requires lv_font_conv and Python fontTools; LV_FONT_CONV may name a local CLI.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="$HERE/../main/fonts"
FONT="${1:-$HERE/../../x12y12pxMaruMinya.ttf}"
LVFC="${LV_FONT_CONV:-npx lv_font_conv}"
FULL=$(python3 - "$FONT" <<'PY'
import sys
from fontTools.ttLib import TTFont
print(','.join('0x%X'%cp for cp in sorted(TTFont(sys.argv[1]).getBestCmap()) if cp>=32))
PY
)
SYMS=$(python3 "$HERE/i18n_chars.py" "$HERE/../main" --font "$FONT")
# One 24 px face: FONT_SMALL is a RAM copy of font_px24 (fonts.c), so no font_px20.c (Plan 6).
for NAME in font_px24; do
    $LVFC --font "$FONT" -r "$FULL" --size 24 --bpp 1 --format lvgl \
      --lv-font-name "$NAME" --lv-include lvgl.h --no-kerning --no-prefilter -o "$OUT/$NAME.c"
done
$LVFC --font "$FONT" -r '0x20-0x7E' --symbols "$SYMS" --size 36 --bpp 1 --format lvgl \
  --lv-font-name font_px36b --lv-include lvgl.h --no-kerning --no-prefilter -o "$OUT/font_px36b.c"
