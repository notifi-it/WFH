import { chromium } from 'playwright';
const [,, file, out, scale] = process.argv;
const b = await chromium.launch();
const p = await b.newPage({ viewport: { width: 368, height: 448 }, deviceScaleFactor: Number(scale) || 1 });
await p.goto('file://' + file);
await p.waitForTimeout(300);
await p.screenshot({ path: out });
await b.close();
console.log('wrote', out);
