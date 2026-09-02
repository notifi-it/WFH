#!/usr/bin/env python3
"""Pixel-diff two same-size screen renders (368x448) and emit evidence.

    python3 diff.py <ground_truth.png> <mock.png> <out_prefix>

Writes <out_prefix>-heat.png (red-on-dimmed heatmap) and
<out_prefix>-triptych.png (truth | mock | heat), and prints two numbers:
noticeable = any channel off by >16/255, strong = off by >64/255.

Reading the numbers: text regions always differ (browser font vs LVGL's
bundled Montserrat) — glyph-shaped red is expected noise. Solid red over a
filled region means a colour is wrong; a red outline offset to one side
means geometry is off by pixels. Those two are the findings; chase them.
"""
import sys
from PIL import Image, ImageChops

truth_p, mock_p, prefix = sys.argv[1], sys.argv[2], sys.argv[3]
a = Image.open(truth_p).convert('RGB')
b = Image.open(mock_p).convert('RGB')
if a.size != b.size:
    sys.exit(f"size mismatch: {a.size} vs {b.size} — render both at 1x first")

g = ImageChops.difference(a, b).convert('L')
hist = g.histogram()
total = a.size[0] * a.size[1]
noticeable = sum(hist[16:])
strong = sum(hist[64:])

heat = Image.merge('RGB', (g.point(lambda v: min(255, v * 3)),
                           g.point(lambda v: 0), g.point(lambda v: 0)))
out = ImageChops.add(a.point(lambda v: v // 3), heat)
out.save(f'{prefix}-heat.png')

w, h = a.size
trip = Image.new('RGB', (w * 3 + 20, h), (20, 20, 20))
trip.paste(a, (0, 0)); trip.paste(b, (w + 10, 0)); trip.paste(out, (2 * w + 20, 0))
trip.save(f'{prefix}-triptych.png')

print(f"noticeable: {noticeable} px ({100 * noticeable / total:.1f}%)")
print(f"strong:     {strong} px ({100 * strong / total:.1f}%)")
print(f"wrote {prefix}-heat.png, {prefix}-triptych.png")
