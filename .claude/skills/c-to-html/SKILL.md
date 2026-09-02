---
name: c-to-html
description: Transcribe a firmware LVGL screen (ui.c) into an as-built HTML mock in design/, then prove the two match by rendering both and pixel-diffing them against the simulator. Use when asked to mirror a board screen in HTML, create/update *-asbuilt.html, or check that a mock still matches the firmware.
---

# C → HTML: as-built screen mocks that provably match the firmware

The firmware (`firmware/main/ui.c`) is the source of truth. Each screen gets
a `design/<screen>-asbuilt.html` transcribed from the C — never from a
screenshot by eye — and the loop is closed with a pixel diff against a real
render. CLAUDE.md's rule applies: transcribe element by element; measured-off-
a-picture is how the countdown ended up on the wrong corner for days.

No as-built mocks are checked in. The ones that existed were transcribed
from the pre-v4 firmware and were deleted when it changed; this skill
produces a fresh `design/<screen>-asbuilt.html` each time it is invoked.
Treat an as-built as a diff artifact, not a design document.

## The screens

`ui.c` has five builders; `sim/main.c` and `sim/Makefile` expose six render
targets (the flow builder serves two actions):

| builder | screen | sim target |
|---|---|---|
| `build_grid` | the six-tile grid | `make -C sim grid` |
| `build_popup` | one action's Done / Skip / +15m page | `make -C sim popup` (water) |
| `build_card` | the due-now collision card, several actions at once | `make -C sim card` (stand + water + roll) |
| `build_flow` | the guided countdown-ring step, shown for stretch and roll | `make -C sim stretch`, `make -C sim rollflow` |
| `build_history` | one action's slots for the day, with Undo | `make -C sim history` (water) |

Each `make -C sim <target>` writes `sim/<target>.png` at 368×448.

## As-built ≠ design mock

The v4 design mocks (`design/*-v4*.html`) are set in Geist 500/700. The
firmware draws text with LVGL's bundled Montserrat at the sizes `ui.c`
requests: 14, 16, 24, 28, 36 and 48. An as-built mock must use those
Montserrat sizes (`font-family: Montserrat, -apple-system, …`), so it never
matches a v4 design mock and must not be diffed against one. The diff is
always as-built vs the simulator render (or the board capture).

## Step 1 — transcribe from C

Read the `build_*` function and the matching refresh/show function (layout
often lives there, e.g. dot pitch in `dots_refresh`, per-action
tint in `ui_show_*`). Every number in the HTML must trace to a line of C.
Put the trace in an HTML comment at the top of the file: which constants,
from which functions.

LVGL → CSS translation table (the traps that already bit):

| LVGL | CSS / rule |
|---|---|
| `lv_obj_align(o, TOP_RIGHT, -10, 8)` | offsets are from the parent's **content area**, which excludes the border — a 2px-bordered tile's content starts at +2,+2 |
| `LV_OPA_40` etc. | percent of 255: OPA_40 = 40%, OPA_80 = 80%, OPA_20 = 20% |
| `bg_grad_dir VER`, `main_stop 255`, `grad_stop 0` | stops inverted ⇒ gradient runs grad-colour at top → main colour at bottom |
| no `clip_corner` | children are NOT clipped to the tile radius — don't add `overflow:hidden` |
| default theme buttons | carry a small grey drop shadow (`box-shadow: 0 3px 2px rgba(128,138,148,.5)`); `ui.c` strips it only where it calls `lv_obj_set_style_shadow_width(o, 0, 0)` |
| `lv_font_montserrat_N` | `font: <weight> Npx Montserrat, …`; browser Montserrat ≠ LVGL's bundled cut, so text will always diff a little |
| `LV_SYMBOL_*` | LVGL's own glyphs; a ✕ or an SVG approximation is fine but will show in the diff |
| icons | inline the SVGs from `design/icons.html` (they are the source of the firmware's A8 assets), `stroke: var(--tint)` |
| `lv_image_set_scale(o, 512)` | 2x, scaled about the widget's centre: a 34px widget at y=76 draws 68px spanning y=59..127 |
| `lv_arc` (flow ring) | an SVG circle with `stroke-dasharray`; arc angles are clockwise from 3 o'clock unless `lv_arc_set_rotation` says otherwise |
| positions | absolute-position everything; no CSS grid/flex for tile placement — the browser must not round differently than the C does |

State parity: the mock must hardcode the same state as the sim target it will
be diffed against — `sim/main.c` sets the demo state for each target (which
action, counts, countdown text, fills). For dot rows, replicate `dots_refresh`:
`pitch = min((TILE_W - 2*border - 2*inset)/n, 14)`,
`size = max(pitch-3, 6)`; check the current constants in `ui.c` before
copying these.

## Step 2 — render both sides

Ground truth, in order of preference:

1. The board itself: `tools/grab-screen.sh` (must print `clean`; a capture
   reporting dropped rows is not evidence — regrab). The grab resets the
   board, and only the active screen can be captured.
2. The simulator: `make -C sim <target>` from the table above — real
   firmware UI compiled for the host, with `sim/main.c`'s demo state.

Mock:

    node design/shot-one.mjs "$PWD/design/<screen>-asbuilt.html" design/shots/<screen>-asbuilt-1x.png 1

(`shot-one.mjs` needs the absolute path; it prefixes `file://`.)

## Step 3 — diff

    python3 .claude/skills/c-to-html/diff.py sim/<target>.png design/shots/<screen>-asbuilt-1x.png <prefix>

It prints two percentages and writes `<prefix>-heat.png` and
`<prefix>-triptych.png` (truth | mock | heat). How to read it:

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

An as-built is wrong the moment `ui.c` changes. When touching a screen's C,
rerun this skill for that screen in the same change (CLAUDE.md: change
design and firmware together, then render both). Delete the as-built and
its shot once the diff has been reviewed; they are not kept in the repo.
