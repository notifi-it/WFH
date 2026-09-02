# design/

HTML mocks of the board's screens at device geometry, plus the source art
for the firmware's icons. Everything renders in a browser; nothing here
runs on the board.

## Device geometry

- Panel: 368×448, portrait. Every mock sets `html, body` to exactly that.
- Type: Geist 500 and 700 (`fonts/Geist-*.woff2` for the browser,
  `fonts/Geist-*.ttf` for `tools/gen-fonts.sh`). The firmware currently draws
  text with LVGL's bundled Montserrat, not Geist — see CLAUDE.md on
  `lv_font_conv` emitting the LVGL 8 glyph API. Text in a mock therefore
  shows the intended weight and size, not the glyphs the board draws.
- Everything a mock draws must be rects, alpha, text, arcs, or A8 icons.
  The board's software renderer has no blend modes.

## The v4 set

| file | screen | firmware builder | sim target |
|---|---|---|---|
| `board-v4c.html` | six-tile grid: tint per action, waterline fill, icon, dot row | `build_grid` | `grid` |
| `popup-v4.html` | one action's page: Done / Skip / +15m (shown for water) | `build_popup` | `popup` |
| `card-v4.html` | due-now collision: one row per due action, Confirm / +15m / X | `build_card` | `card` |
| `stretch-v4.html` | guided step with countdown ring, step 2 of 3 paused on Start | `build_flow` | `stretch` |
| `roll-v4.html` | the single-step roll flow, paused on Start | `build_flow` | `rollflow` |
| `history-v4.html` | one action's slots for the day, with Undo; `#water` shows a busy day | `build_history` | `history` |

The mocks are the design. `firmware/main/ui.c` transcribes them; `sim/`
renders that transcription on the host. Change a mock and the firmware in
the same commit, and re-render both — CLAUDE.md, "Changing the UI".

## Rendering a mock

```bash
cd design && npm i playwright     # once
node design/shot-one.mjs <absolute path to .html> <out.png> <scale>
```

`shot-one.mjs` prefixes `file://`, so the path must be absolute. `scale` is
the device pixel ratio; the panel is 1x, so judge at 1. `HASH` is appended
to the URL: `HASH="#water" node design/shot-one.mjs "$PWD/design/history-v4.html" design/shots/history-v4-water-1x.png 1`
renders history-v4's busy variant.

`shots/` holds one 1x render per mock (`<mock>-1x.png`) plus
`history-v4-water-1x.png` and the icon sheet at 1x and 3x. Re-render a shot
whenever its mock changes.

## Icons

`icons.html` is the source of truth for the firmware's icons: six MingCute
outlines (walk, glass-cup, warm-up, cookie, bowl-2, yoga), white on
transparent, 64px, one `<div class="ico" id="<action>">` each.

```
icons.html ──shoot-icons.mjs──▶ icons/<action>.png ──tools/gen-icons.py──▶ firmware/main/icons/icons.c
```

- `cd design && node shoot-icons.mjs` screenshots each `#id` element to
  `icons/<id>.png` with a transparent background.
- `python3 tools/gen-icons.py` reads those PNGs' alpha channel and writes
  LVGL 9 A8 image descriptors. One asset per action; the firmware tints it
  per action at runtime and scales it for the popup (88px) and card (40px).
- `board-v4c.html` inlines the same SVG markup so the mock and the board
  show the same glyphs. When an icon changes, change it in `icons.html`,
  paste the new markup into `board-v4c.html`, and run both steps above.

## As-built mocks

`<screen>-asbuilt.html` files are transient: the `c-to-html` skill
(`.claude/skills/c-to-html/SKILL.md`) generates one from `ui.c`, pixel-diffs
it against the `sim/` render, and it is deleted after review. None are
checked in.
