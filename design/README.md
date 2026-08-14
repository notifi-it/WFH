# Design loop

Renders board screens at exact device geometry (368×448) and writes PNGs, so
a design can be iterated on and *looked at* without flashing hardware — by you,
or by Claude, which can read the PNGs directly.

```bash
cd design
npm i playwright          # or: ln -sfn /opt/node22/lib/node_modules node_modules
node shoot.mjs            # all modes, 1x + 3x → shots/
node shoot.mjs board.html full     # one file, one mode
```

Judge at **1x**. The 3x pass is for inspecting grain and dot rendering, but
1x is what the panel actually shows and small-size legibility is the thing
that keeps going wrong.

## Why this and not the LVGL simulator

The SDL simulator compiles and runs the real firmware UI — higher fidelity,
but it's a build cycle and it can't run until §7 exists. This is HTML, so it
renders instantly and needs nothing but a browser. Use it to settle *what the
design is*; use the simulator later to confirm the port matches.

Both renderers are downstream of the same design. Neither is the source of
truth — this is where the look gets decided, and §7 transcribes it.

## Modes

`shoot.mjs` renders three variants of each screen:

| mode | what it shows |
|---|---|
| `full` | the design as specified |
| `device` | same compositing the board can actually do |
| `nograin` | control, to check the grain is doing anything at all |

`full` and `device` are currently **identical by design** — see the grain note
below. If they ever diverge, something has been added that the panel can't
render, which is exactly what that mode is there to catch.

The grain animation is frozen to a fixed step during screenshots
(`data-freeze`), so two shots taken seconds apart differ only where the design
differs, not in noise phase. Without that you can't diff anything.

## The grain, and why it is alpha noise

The first version used a grey luminance-noise tile with
`mix-blend-mode: overlay`, copied from the web prototype. **It rendered
nothing** — `full` and `nograin` came out pixel-identical.

Overlay's dark-side formula is `2 × base × blend`. With a tile background of
`#151a21` (base ≈ 0.08), even pure white noise only reaches 0.16, and at 8%
layer opacity that moves the pixel by about 2/255. Invisible on a dark UI, by
construction rather than by mistuning.

The fix is a texture that is **white with varying alpha**, composited
normally:

```
feTurbulence → feColorMatrix
  rgb  = 1,1,1                (flat white)
  alpha = R − 0.58            (threshold: only brighter noise shows)
```

This matters beyond the web app: **normal alpha compositing is all the board's
software renderer can do.** Blend modes are not available there. Choosing
alpha noise means one grain definition works in both places, and the `device`
mode above stays identical to `full` instead of being a downgrade.

Current values — `.20` on tiles, `.10` on the frame — were landed by
iteration. `.55/.30` blew out the blacks and swallowed the pending dots;
`.085` with overlay was invisible.
