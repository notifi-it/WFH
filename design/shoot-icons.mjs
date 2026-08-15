// Render each icon to a transparent PNG at device size for A8 conversion.
import { chromium } from 'playwright';
import { mkdirSync } from 'node:fs';
const IDS = ['stand','water','roll','snack','lunch','stretch'];
mkdirSync('icons', { recursive: true });
const b = await chromium.launch();
const p = await b.newPage({ viewport: { width: 100, height: 300 }, deviceScaleFactor: 1 });
await p.goto('file://' + process.cwd() + '/icons.html');
for (const id of IDS) {
  await p.locator('#' + id).screenshot({ path: `icons/${id}.png`, omitBackground: true });
}
await b.close();
console.log('rendered', IDS.length, 'icons');
