// Screenshot a whole page (not the 368x448 panel crop) — for the prototype
// frame, which deliberately sits inside a device bezel on a larger canvas.
import { chromium } from 'playwright';
const [,, file, out] = process.argv;
const b = await chromium.launch();
const p = await b.newPage({ viewport: { width: 460, height: 600 }, deviceScaleFactor: 2 });
await p.goto('file://' + file);
await p.waitForTimeout(500);
await p.screenshot({ path: out });
await b.close();
console.log('wrote', out);
