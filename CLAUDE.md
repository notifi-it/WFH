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
| `design/*.html` | the design | what the firmware actually draws |
| `make -C sim grid` | what the firmware draws, correct colour | it is not the panel |
| `tools/grab-screen.sh` | the board's own framebuffer | nothing, **if it prints `clean`** |

Use the simulator for "does this look right"; use the capture to confirm the
board is running what you think it is. The capture's colour used to decode
wrongly: the dump went out the *secondary* console (USB-Serial/JTAG), which
silently drops bytes when its 64-byte FIFO backs up, and one lost byte
sheared everything after it. Hours went into chasing colour bands that only
ever existed in that capture — the person looking at the panel kept saying
the colours were fine. Fixed by making USB-Serial/JTAG the primary console
and framing the dump per row; lost rows now come out magenta and the tool
reports them. A capture that reports dropped rows is not evidence of
anything — regrab it. **When the instrument and the human disagree about
what is on a screen, the human is right.**

## Changing the UI

- Change `design/*.html` and the firmware together, then render both, and
  transcribe the design element by element rather than by eye. They drift
  silently otherwise: the countdown sat bottom-right on the board and
  top-right in the design for days, stealing the dot row's width, and no
  amount of resizing the dots fixed it.

## LVGL

- **Never compute geometry from `lv_obj_get_content_width/height()` during
  build.** Layout has not run, the answer is meaningless, and if the value is
  only recomputed on a state change it never corrects itself. Use the
  constants that define the tile.
- **Fonts and images must be generated for LVGL 9.** `lv_font_conv` 1.5.3
  emits the LVGL 8 glyph API: it compiles clean and renders *nothing at all*.
  Same class of trap for images — write converters against the struct in
  `firmware/managed_components/lvgl__lvgl`, not against a tool's idea of it.
- The firmware uses only LVGL's bundled Montserrat fonts (14/16/28/36/48).
  They carry the `LV_SYMBOL_*` glyphs; a generated ASCII-only font does not,
  so the X and the history marks would render blank under it.
- **Never delete an object from inside its own event handler.** The feed's
  Undo rebuilt the list with `lv_obj_clean()` in the button's click
  callback; LVGL kept walking the freed button and the UI froze on the
  next tap, mutex held, no panic. Defer with `lv_async_call()`. The tick
  task now reboots the board after 15s without the display lock, so a
  repeat of this class costs a restart instead of an unplug.
- `clip_corner` forces a per-pixel mask layer on every redraw. Invisible at
  1 Hz, starves the LVGL task at 30fps and trips the watchdog.
- Animate as little as possible. The shipped waterline (`design/board-v4c`)
  is a static tinted wash whose height is the fraction elapsed, plus a thin
  crest: an `lv_line` of 61 float points, two sines, 30fps. The body never
  animates. A moving polyline is geometry (555us/frame); a fluid is
  per-pixel work.

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
- **The WiFi radio and the QSPI panel cannot be alive at the same time.**
  With the radio up the panel tears into white bands and then freezes, while
  LVGL's own snapshot stays clean. Quiescing every panel write during the
  radio window did not help; the panel desyncs regardless. SNTP therefore
  runs in `app_main` *before* `panel_power_up()`, blocking, and the radio is
  stopped before the panel gets power (`wifi_time.c`).
- **The panel's draw buffer must be internal DMA RAM, reserved before
  WiFi.** The BSP allocates it with default caps, which at >16KB means
  PSRAM. The SPI driver cannot DMA from PSRAM, so it allocates a private
  internal bounce buffer per ~32KB chunk — and once WiFi has taken its
  share of internal RAM those allocations fail intermittently. The chunk
  is dropped and the panel keeps its old rows: two frames interleaved in
  ~44-row bands, with LVGL's snapshot perfectly clean. The log says
  `setup_dma_priv_buffer: Failed to allocate priv TX buffer`. `main.c`
  reserves 50 rows of internal DMA RAM before WiFi and installs it over the
  port's token buffer with `lv_display_set_buffers()`; freeing it for the
  port to re-take was tried and failed one boot in three, because the port
  allocates its context struct first and that lands in the hole.
  `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` keeps WiFi out of internal RAM.
- **`bsp_display_start()` registers this SPI panel with the LVGL port as an
  RGB display.** For RGB panels the port reports the draw buffer free the
  moment a transfer is queued — right for a memory-mapped framebuffer,
  wrong for a DMA still streaming from the buffer. It only ever worked
  because the PSRAM bounce path copied the pixels out synchronously; give
  it a DMA-capable buffer and LVGL paints the next chunk over the one in
  flight (pink bands, thin slivers of the real screen). `main.c` brings the
  display up itself with `lvgl_port_add_disp()`, so the buffer is released
  from the transfer-done callback.
- **Stop the radio, do not `esp_wifi_deinit()` it.** Deinit frees the
  driver's internal RAM; the panel bring-up then allocates LVGL's tick mutex
  at the WiFi task's old address and the driver's teardown writes over it —
  seen as an interrupt-WDT panic spinning in `lvgl_port_tick_increment`,
  every boot, right after `LCD panel create success`. Decode with
  `xtensa-esp32s3-elf-addr2line -pfiaC -e build/wfh.elf <addrs>`.

## Storage

SQLite was measured on the board and cut: ~3s per insert at 688 rows, and
`journal_mode=WAL` returns no row on that port. One append-only file per day
does 18.9ms p50, flat to 15k rows. Keep the current day's file handle open —
opening a file costs a linear directory scan, so an open-per-write makes every
tap pay for every day ever recorded.

## Tests

`make -C firmware/test` runs `derive` and the card on the host, no board. The
`test_derive` and `test_card` binaries and their `.dSYM` directories are build
products and gitignored. **A suite that passes first try has not been shown
to work** — break the code on purpose and confirm the tests catch it. The
mutations that must fail are listed in `fixtures/derive/README.md`. The DST
fixtures assert hand-computed epoch constants; never regenerate them from
program output.

## Shell

`cd foo && cmd` silently skips `cmd` when the shell is already in `foo`. This
has produced stale artifacts and "fixes" that never ran more than once. Prefer
absolute paths, or check the result rather than the exit code of the chain.
