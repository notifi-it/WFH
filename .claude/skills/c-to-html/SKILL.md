---
name: c-to-html
description: Transcribe a firmware LVGL screen (ui.c) into an as-built HTML mock in design/, then prove the two match by rendering both and pixel-diffing them. Use when asked to mirror a board screen in HTML, create/update *-asbuilt.html, or check that a mock still matches the firmware.
---

# C → HTML: as-built screen mocks that provably match the firmware

The firmware (`firmware/main/ui.c`) is the source of truth. Each screen gets
a `design/<screen>-asbuilt.html` transcribed from the C — never from a
screenshot by eye — and the loop is closed with a pixel diff against a real
render. CLAUDE.md's rule applies: transcribe element by element; measured-off-
a-picture is how the countdown ended up on the wrong corner for days.

## The screens

Enumerate them from `ui.c`'s `build_*` functions and `sim/main.c`'s mode
switch. Currently: `grid` (build_grid), `popup` (build_popup), `card`
(build_card — built but never shown; skip unless asked).

## Step 1 — transcribe from C

Read the `build_*` function and the matching refresh function (layout often
lives in refresh, e.g. dot pitch in `dots_refresh`). Every number in the HTML
must trace to a line of C. Put the trace in an HTML comment at the top of the
file: which constants, from which functions.

LVGL → CSS translation table (the traps that already bit):

| LVGL | CSS / rule |
|---|---|
| `lv_obj_align(o, TOP_RIGHT, -10, 8)` | offsets are from the parent's **content area**, which excludes the border — a 2px-bordered tile's content starts at +2,+2 |
| `LV_OPA_40` etc. | percent of 255: OPA_40 = 40%, OPA_80 = 80%, OPA_20 = 20% |
| `bg_grad_dir VER`, `main_stop 255`, `grad_stop 0` | stops inverted ⇒ gradient runs grad-colour at top → main colour at bottom |
| no `clip_corner` | children are NOT clipped to the tile radius — don't add `overflow:hidden` |
| default theme buttons | carry a small grey drop shadow (`box-shadow: 0 3px 2px rgba(128,138,148,.5)`); `ui.c` strips it only where it calls `lv_obj_set_style_shadow_width(o, 0, 0)` |
| `montserrat_14/16/24/28` | `font-family: Montserrat, -apple-system, …`; browser Montserrat ≠ LVGL's bundled cut, so text will always diff a little |
| `LV_SYMBOL_*` | LVGL's own glyphs; a ✕ or an SVG approximation is fine but will show in the diff |
| icons | inline the SVGs from `design/icons.html` (they are the source of the firmware's A8 assets), `stroke: var(--tint)` |
| `lv_image_set_scale(o, 512)` | 2x, scaled about the widget's centre: a 34px widget at y=76 draws 68px spanning y=59..127 |
| positions | absolute-position everything; no CSS grid/flex for tile placement — the browser must not round differently than the C does |

State parity: the mock must hardcode the same state as whatever it will be
diffed against (same counts, same countdown text, same fills). For dot rows,
replicate `dots_refresh`: `pitch = min((TILE_W - 2*border - 2*inset)/n, 14)`,
`size = max(pitch-3, 6)`.

## Step 2 — render both sides

Ground truth, in order of preference:

1. The board itself: `tools/grab-screen.sh` (must print `clean`; a capture
   reporting dropped rows is not evidence — regrab). Note the grab resets the
   board, and only the active screen can be captured.
2. The simulator: `make -C sim grid` / `make -C sim popup` — real firmware
   UI compiled for the host, with `sim/main.c`'s demo state.

Mock: `cd design && node shot-one.mjs "$PWD/<screen>-asbuilt.html" shots/<screen>-asbuilt-1x.png 1`

## Step 3 — diff

    python3 .claude/skills/c-to-html/diff.py <truth.png> <mock.png> <prefix>

It prints two percentages and writes a heatmap + triptych. How to read it:

- **Glyph-shaped red** on text: expected (font cut differs). Ignore.
- **Faint uniform glow over saturated fills**: RGB565 quantization — the
  panel/sim render 16-bit colour, so e.g. #6ec3e0 comes back #6ac2e6. Max
  ~8/255 per channel, below the noticeable threshold; the heatmap's 3×
  amplification makes it visible. Keep the mock at the C's 24-bit values.
- **Solid red across a filled region**: a colour is wrong. Fix the mock —
  or notice the firmware inherited something unintended (that is how the
  theme's button shadow was found).
- **Red outline offset to one side**: geometry off by pixels. Recheck the
  content-area rule and the constant you transcribed.

Done means: strong-diff pixels are confined to text and symbol glyphs, and
you have looked at the heatmap and can name every red region. Send the
triptych with the numbers — the claim "they align" needs the picture.

## Step 4 — keep them aligned

A mock is wrong the moment `ui.c` changes. When touching a screen's C, rerun
this skill for that screen in the same change (CLAUDE.md: change design and
firmware together, then render both).
