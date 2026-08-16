# WFH board — working notes

A desk tracker on a Waveshare ESP32-S3-Touch-AMOLED-1.8. `PLAN.md` is the
spec. This file is the stuff that cost time and is invisible from the code.

## Verifying a display

**Logs are not evidence that a screen is right.** A panel with no power, a
framebuffer full of garbage, and a font that renders nothing all produce clean
logs: `panel up: 368x448`, no errors, every SPI write returning `ESP_OK`. This
was claimed as "deployed and verified" twice on log output alone, and both
times the screen was wrong.

Three ways to look at the UI, and what each one lies about:

| | shows | lies about |
|---|---|---|
| `design/*.html` | what the design *should* be | what the firmware actually draws |
| `make -C sim grid` | what the firmware draws, correct colour | it is not the panel |
| `tools/grab-screen.sh` | the board's own framebuffer | **colour** — decodes wrongly |

Use the simulator for "does this look right". Use the capture only to confirm
the board is running what you think it is. Hours went into chasing colour
bands that only ever existed in that capture — and the person looking at the
actual panel kept saying the colours were fine. **When the instrument and the
human disagree about what is on a screen, the human is right.**

## Changing the UI

- Change `design/*.html` and the firmware together, then render both. They
  drift silently otherwise, and a stale mock is worse than none.
- `design/prototype-standalone.html` is generated (fonts inlined). Regenerate
  it or you are reviewing an old design.
- Transcribe the design element by element, not by eye. The countdown sat
  bottom-right on the board and top-right in the design for days; it was
  stealing the dot row's width, and no amount of resizing the dots fixed it.

## LVGL

- **Never compute geometry from `lv_obj_get_content_width/height()` during
  build.** Layout has not run, the answer is meaningless, and if the value is
  only recomputed on a state change it never corrects itself. Use the
  constants that define the tile.
- **Fonts and images must be generated for LVGL 9.** `lv_font_conv` 1.5.3
  emits the LVGL 8 glyph API: it compiles clean and renders *nothing at all*.
  Same class of trap for images — write converters against the struct in
  `firmware/managed_components/lvgl__lvgl`, not against a tool's idea of it.
- LVGL's bundled fonts carry the `LV_SYMBOL_*` glyphs. A custom ASCII-only
  font does not, so symbol labels must keep the bundled font.
- `clip_corner` forces a per-pixel mask layer on every redraw. Invisible at
  1 Hz, starves the LVGL task at 30fps and trips the watchdog.
- Animate as little as possible: the waterline moves, the body does not. A
  moving surface is geometry (555us/frame); a fluid is per-pixel work.

## This board

- It is a **V1**: SH8601 display, FT5x06-family touch. Confirmed from the
  factory firmware's strings and an I2C probe.
- **Pin the BSP to `~1.1.4`.** The 2.x line drives V2 silicon (CO5300 +
  CST816S) and aborts on this hardware.
- **The BSP creates the I/O expander and never drives a pin of it.** Three
  panel-critical lines hang off it, and nothing reports the omission:
  `EXIO0` = LCD_RESET, `EXIO1` = DSI_PWR_EN (**no power, black screen**),
  `EXIO2` = TP_RESET (touch never answers, `bsp_display_start` aborts).
  Drive them before `bsp_display_start()`.
- An I2C scan is the cheapest board-health check there is. `0x18` ES8311,
  `0x20` TCA9554, `0x34` AXP2101, `0x38` touch, `0x51` PCF85063, `0x6B`
  QMI8658. Touch absent means held in reset, not missing.
- `sdkconfig.defaults` only seeds a *fresh* `sdkconfig`. Editing it does
  nothing to an existing build — delete `sdkconfig` and rebuild.
- After a crash loop the USB-Serial/JTAG can stop answering entirely. No
  software reset recovers it; it needs BOOT held while replugging USB.

## Storage

SQLite was measured on the board and cut: ~3s per insert at 688 rows, and
`journal_mode=WAL` returns no row on that port. One append-only file per day
does 18.9ms p50, flat to 15k rows. Keep the current day's file handle open —
opening a file costs a linear directory scan, so an open-per-write makes every
tap pay for every day ever recorded.

## Tests

`make -C firmware/test` runs `derive` and the card on the host, no board.
**A suite that passes first try has not been shown to work** — break the code
on purpose and confirm the tests catch it. The mutations that must fail are
listed in `fixtures/derive/README.md`. The DST fixtures assert hand-computed
epoch constants; never regenerate them from program output.

## Shell

`cd foo && cmd` silently skips `cmd` when the shell is already in `foo`. This
has produced stale artifacts and "fixes" that never ran more than once. Prefer
absolute paths, or check the result rather than the exit code of the chain.
