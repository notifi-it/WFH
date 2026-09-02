# Board simulator

Runs the **real firmware UI** on the laptop and writes a PNG.

```bash
make -C sim grid       # sim/grid.png      the grid
make -C sim popup      # sim/popup.png     the action popup (water)
make -C sim card       # sim/card.png      the checklist card, four rows, two ticked
make -C sim stretch    # sim/stretch.png   the guided stretch flow
make -C sim rollflow   # sim/rollflow.png  the single-step flow (shoulder roll)
make -C sim history    # sim/history.png   one action's day (water)
```

It compiles `firmware/main/ui.c` unchanged against a memory display instead of
a panel; `stubs/` supplies just enough of ESP-IDF and the BSP to link. Because
it is the same file, it cannot drift the way a re-implementation would.

## Why this exists

There were three ways to look at this UI and each lied differently:

| | shows | lies about |
|---|---|---|
| `design/*.html` | what the design *should* be | what the firmware actually draws |
| `tools/grab-screen.sh` | the board's own framebuffer | nothing, if it prints `clean`; a capture that reports dropped rows is not evidence of anything |
| this | what the firmware draws, in correct colour | nothing much, but it is not the panel |

The capture tool is still useful for confirming the board is running what you
think it is. For "does this look right", use this.

## What is stubbed

`derive`, storage and inputs — the question here is what the screen looks like,
and `derive` has its own host tests. `sim/main.c` fakes a late morning at
12:20, chosen to exercise every dot state and a range of wash heights, with
a full timeline for water (done, done, skipped, open) so the history screen
has something to show.

## Gotchas paid for already

- `lv_conf.h` must have its `#if 0` guard flipped to `1`, or LVGL silently uses
  defaults — which enable only the 14px font.
- `LV_USE_STDLIB_MALLOC` must be `LV_STDLIB_CLIB`. On the builtin pool the app
  exhausts 64KB during `ui_build` and the allocator spins forever, which looks
  exactly like a layout hang.
- `lv_conf.h` is a Makefile dependency. It is not a source file, so without
  that line a config change silently does nothing.
