// test-web.mjs -- the browser front-end, driven by a real browser.
//
// The pin for docs/c4bb-storage.md M5's other half. test-drives.mjs
// covers the logic; this covers the part that only exists once there is
// a DOM and an IndexedDB: the panel draws, a medium can be made, the
// machine writes to it, and IT IS STILL THERE AFTER A RELOAD. That last
// one is the whole milestone, and there is no way to check it without a
// browser.
//
// Needs Playwright, which this repo does not vendor. Point it at one:
//
//   C4BB_PLAYWRIGHT=/path/to/node_modules/playwright \
//     node src/c4bb/tests/test-web.mjs
//
// It serves the repo itself, on a port unlikely to be in use, and takes
// it down again. Without Playwright it says so and exits 0 -- a test
// that cannot run is not a test that failed, and `make test-c4bb` has
// no browser.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';

const ROOT = new URL('../../..', import.meta.url).pathname;
const PORT = 8479;

let chromium;
try {
  const where = process.env.C4BB_PLAYWRIGHT;
  ({ chromium } = await import(where ? `${where}/index.mjs` : 'playwright'));
} catch {
  console.log('test-web: SKIPPED (no Playwright -- set C4BB_PLAYWRIGHT)');
  process.exit(0);
}

const TYPES = {
  '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript',
  '.css': 'text/css', '.json': 'application/json', '.uc': 'text/plain',
  '.hwd': 'text/plain', '.txt': 'text/plain',
};
const server = createServer(async (req, res) => {
  const p = join(ROOT, normalize(decodeURIComponent(req.url.split('?')[0])));
  try {
    const body = await readFile(p);
    res.writeHead(200, { 'content-type': TYPES[extname(p)] || 'application/octet-stream' });
    res.end(body);
  } catch { res.writeHead(404); res.end(); }
});
await new Promise(r => server.listen(PORT, r));

let fails = 0;
const ok = (name, cond, extra = '') => {
  if (cond) { console.log(`  ok   ${name}`); return; }
  console.log(`  FAIL ${name}${extra ? '\n       ' + extra : ''}`);
  fails++;
};

const browser = await chromium.launch();
const ctx = await browser.newContext();
const page = await ctx.newPage();
const errs = [];
page.on('pageerror', e => errs.push('pageerror: ' + e.message));
page.on('console', m => { if (m.type() === 'error') errs.push('console: ' + m.text()); });

const URL_ = `http://localhost:${PORT}/src/c4bb/web/index.html`;
const drivesUp = () => page.waitForFunction(
  () => document.querySelectorAll('#drives .drive').length === 3, null, { timeout: 30000 });
const term = () => page.$eval('#terminal', e => e.textContent);
const waitTerm = (s, ms = 120000) => page.waitForFunction(
  t => document.getElementById('terminal').textContent.includes(t), s, { timeout: ms });

try {
  await page.goto(URL_, { waitUntil: 'domcontentloaded' });
  await drivesUp();

  ok('the panel draws a row per drive',
     (await page.$$eval('#drives .drive', r => r.length)) === 3);
  const opts = await page.$$eval('#drives .drive:first-child .dsel option', o => o.map(x => x.value));
  ok('every shipped medium is offered', opts.length >= 5, opts.join(' '));
  const d0 = await page.$eval('#drives .drive:first-child .dinfo', e => e.textContent);
  ok('drive 0 comes with a disk in it', /file/.test(d0) && /RO/.test(d0), d0);

  // A blank medium, and it goes into a drive by itself.
  await page.click('#newmedium');
  await page.waitForFunction(
    () => document.querySelectorAll('#usermedium option').length === 1, null, { timeout: 10000 });
  const slot = await page.$$eval('#drives .dsel', s => s.findIndex(x => x.value.startsWith('m')));
  ok('a new blank medium lands in a free drive', slot === 1, `slot ${slot}`);

  // Boot the BIOS off drive 0, and let it find the climb disk.
  await page.selectOption('#program', '(BIOS)');
  await drivesUp();
  await page.click('#turbo');
  await waitTerm('bios: drive 0 has');
  ok('the BIOS probes the drives and boots one', (await term()).includes('bios: booting'));
  await waitTerm('A>');
  ok('C4DOS came up off the medium in drive 0', (await term()).includes('C4DOS version'));

  // Now make the machine WRITE to the blank one.
  await page.click('#terminal');
  await page.keyboard.type('RUN dostar.c4r x tools-src.tar\n');
  await waitTerm('dostar: extracted');
  await page.keyboard.type('RUN bbsave.c4r 1:\n');
  await waitTerm('bbsave:');
  ok('the machine wrote to the blank medium', /bbsave: [1-9][0-9]* files/.test(await term()),
     (await term()).split('\n').filter(l => l.startsWith('bbsave')).join(' '));

  // The milestone: close the page and open it again.
  await page.reload({ waitUntil: 'domcontentloaded' });
  await drivesUp();
  const after = await page.$eval('#usermedium', e => e.textContent);
  ok('and it is still there after a reload', /\([1-9][0-9]* files/.test(after), after);

  ok('no errors on the page', errs.length === 0, errs.join('\n       '));
} finally {
  await browser.close();
  server.close();
}

console.log(fails ? `test-web: ${fails} FAILED` : 'test-web: OK');
process.exit(fails ? 1 : 0);
