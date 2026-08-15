#!/usr/bin/env bash
# design/fonts/Geist-*.ttf -> firmware/main/fonts/*.c  (LVGL bitmap fonts)
#
# The design and the firmware share one typeface deliberately: a font that
# cannot be embedded is not a design candidate, however good it looks in a
# browser. ASCII only — the board renders names, numbers and two words.
set -euo pipefail
cd "$(dirname "$0")/.."
F=design/fonts
OUT=firmware/main/fonts
mkdir -p "$OUT"

for s in 14 18 22 28; do
  npx --yes lv_font_conv --font "$F/Geist-500.ttf" --size "$s" --bpp 4 --format lvgl \
    --lv-include lvgl.h -r 0x20-0x7F -o "$OUT/geist_$s.c" --force-fast-kern-format
done
npx --yes lv_font_conv --font "$F/Geist-700.ttf" --size 30 --bpp 4 --format lvgl \
  --lv-include lvgl.h -r 0x20-0x7F -o "$OUT/geist_bold_30.c" --force-fast-kern-format
echo "fonts regenerated"
