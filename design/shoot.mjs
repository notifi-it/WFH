// Screenshot harness for board designs at exact device geometry.
//
//   node design/shoot.mjs                    → all modes, 1x + 3x
//   node design/shoot.mjs board.html full    → one file, one mode
//
// Renders at 368×448 (the panel itself, not a browser window) so the PNG is
// pixel-exact to the device. The 3x pass is for looking at fine detail —
// grain especially — without squinting.

import { chromium } from 'playwright';
import { fileURLToPath } from 'node:url';
import { dirname, join, basename } from 'node:path';
import { mkdirSync } from 'node:fs';

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, 'shots');
mkdirSync(OUT, { recursive: true });

const W = 368, H = 448;

// `freeze` pins the grain animation to a fixed step so two screenshots taken
// seconds apart are actually comparable — otherwise you're diffing noise phase.
const MODES = [
  { name: 'full',    mode: '',        freeze: '2' },   // as designed, web app
  { name: 'device',  mode: 'device',  freeze: '2' },   // no blend mode — see notes
  { name: 'nograin', mode: 'nograin', freeze: '2' },   // control
];

const file = process.argv[2] ?? 'board.html';
const only = process.argv[3];
const modes = only ? MODES.filter(m => m.name === only) : MODES;

const browser = await chromium.launch();

for (const scale of [1, 3]) {
  const ctx = await browser.newContext({
    viewport: { width: W, height: H },
    deviceScaleFactor: scale,
  });
  const page = await ctx.newPage();

  for (const { name, mode, freeze } of modes) {
    await page.goto(`file://${join(HERE, file)}`);
    await page.evaluate(([m, f]) => {
      if (m) document.documentElement.dataset.mode = m;
      document.documentElement.dataset.freeze = f;
    }, [mode, freeze]);

    // Let fonts settle and the (now frozen) grain paint.
    await page.waitForTimeout(250);

    const stem = basename(file, '.html');
    const out = join(OUT, `${stem}-${name}${scale > 1 ? `@${scale}x` : ''}.png`);
    await page.locator('.panel').screenshot({ path: out });
    console.log(`  ${out.replace(HERE + '/', '')}`);
  }
  await ctx.close();
}

await browser.close();
console.log('\ndone');
