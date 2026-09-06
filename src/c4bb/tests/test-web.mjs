// test-web.mjs -- the browser front-end, in a real browser on real
// hardware.
//
// The pin for the halves of M5 and M6 that only exist once there is a
// DOM: the drive panel draws, a medium can be made, the machine writes
// to it and IT IS STILL THERE AFTER A RELOAD, and the firmware stages
// are as refusing here as they are on the command line.
//
// It talks to a browser over CDP rather than launching a headless one.
// The endpoint is whatever ~/git/Homeward's harness uses -- the user
// runs an SSH tunnel from the machine with the browser on it, outward
// to here, so 127.0.0.1:9222 reaches that Chrome's debug port.
//
//   make test-c4bb-web
//   C4BB_CDP=http://127.0.0.1:9222 node src/c4bb/tests/test-web.mjs
//   node src/c4bb/tests/test-web.mjs --local     # a headless one here
//
// HOW THE BROWSER REACHES THE PAGE. Not localhost: the one forwarded
// port is Homeward's dev server and is in use. Two ways that do work,
// tried in this order:
//
//   https://PORT.code.home.stargazer.onl   the house convention -- that
//       host reaches localhost:PORT here. HTTPS, so no question about
//       any API. It goes through code-server, so the browser needs a
//       code-server session; without one it lands on /login and this
//       falls through to the next.
//   http://LAN-IP:PORT                     always available.
//
// c4bb needs no secure context -- plain ES modules and IndexedDB both
// work on a LAN origin -- which is why the fallback is a fallback and
// not a defeat. Homeward cannot do this; its module loading trips on a
// non-secure origin. The IndexedDB claim is asserted below rather than
// assumed, because every medium depends on it.
//
// CARE WITH SOMEBODY ELSE'S BROWSER. That window is the user's, opened
// by hand, and closing it ends the session. So: the tabs are counted
// before and after, only the page this opens is closed, and
// browser.close() on a CDP connection disconnects rather than killing
// anything.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';
import { networkInterfaces } from 'node:os';

const ROOT = new URL('../../..', import.meta.url).pathname;
const PORT = Number(process.env.C4BB_PORT || 8479);
const CDP = process.env.C4BB_CDP || 'http://127.0.0.1:9222';
const LOCAL = process.argv.includes('--local');

// Where the browser has to reach us. A remote one cannot use loopback.
function lanAddress () {
  for (const list of Object.values(networkInterfaces())) {
    for (const n of list || []) {
      if (n.family === 'IPv4' && !n.internal && !n.address.startsWith('172.17.')) return n.address;
    }
  }
  return '127.0.0.1';
}
const RELAY = `https://${PORT}.code.home.stargazer.onl`;
const LAN = `http://${lanAddress()}:${PORT}`;
const BASES = process.env.C4BB_URL ? [process.env.C4BB_URL]
            : LOCAL ? [`http://localhost:${PORT}`]
            : [RELAY, LAN];

// Playwright is not vendored here; the sibling checkouts have it, and
// Homeward's harness looks in the same places.
const PW = [
  process.env.C4BB_PLAYWRIGHT,
  process.env.DS_PLAYWRIGHT,
  '/home/code/git/Salient/node_modules/playwright/index.mjs',
  '/home/code/git/Breach/node_modules/playwright/index.mjs',
].filter(Boolean);
let chromium;
for (const p of PW) {
  try { ({ chromium } = await import(p)); break; } catch { /* next */ }
}
if (!chromium) {
  console.log(`test-web: SKIPPED (no Playwright -- tried ${PW.join(', ')})`);
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
await new Promise(r => server.listen(PORT, '0.0.0.0', r));

let fails = 0;
const ok = (name, cond, extra = '') => {
  if (cond) { console.log(`  ok   ${name}`); return; }
  console.log(`  FAIL ${name}${extra ? '\n       ' + extra : ''}`);
  fails++;
};

let browser, ctx, page, mine = null, tabsBefore = 0;
try {
  if (LOCAL) {
    browser = await chromium.launch({ args: ['--no-sandbox'] });
    ctx = await browser.newContext();
  } else {
    try {
      browser = await chromium.connectOverCDP(CDP, { timeout: 15000 });
    } catch (e) {
      console.log(`test-web: SKIPPED (no browser at ${CDP}: ${e.message})`);
      console.log('          start the tunnel, or run with --local');
      server.close();
      process.exit(0);
    }
    ctx = browser.contexts()[0];
    tabsBefore = ctx.pages().length;
  }
  page = mine = await ctx.newPage();
} catch (e) {
  console.log(`test-web: could not get a page: ${e.message}`);
  server.close();
  process.exit(1);
}

// Erase and Delete ask before they act, and nobody is there to answer.
page.on('dialog', d => d.accept().catch(() => {}));

const errs = [];
page.on('pageerror', e => errs.push('pageerror: ' + e.message));
page.on('console', m => { if (m.type() === 'error') errs.push('console: ' + m.text()); });
// The browser asks for a favicon nobody ships. Not a page error.
page.on('response', r => {
  if (r.status() >= 400 && !r.url().endsWith('/favicon.ico'))
    errs.push(`${r.status()} ${r.url()}`);
});

const drivesUp = () => page.waitForFunction(
  () => document.querySelectorAll('#drives .drive').length === 3, null, { timeout: 30000 });

// Load the page, trying each way in until one of them is actually the
// page rather than somebody's login form.
async function open () {
  let last = '';
  for (const base of BASES) {
    const url = `${base}/src/c4bb/web/index.html`;
    try {
      await page.goto(url, { waitUntil: 'domcontentloaded', timeout: 30000 });
      if (page.url().includes('/login')) {
        console.log(`test-web: ${base} wants a code-server login; trying the next`);
        continue;
      }
      await drivesUp();
      console.log(`test-web: ${LOCAL ? 'local browser' : CDP} -> ${url}`);
      // Anything logged on the way in belongs to a page we walked away
      // from -- code-server's login form 401s on its own PWA manifest,
      // which is its business and not ours. The count starts here.
      errs.length = 0;
      return url;
    } catch (e) { last = e.message.split('\n')[0]; }
  }
  throw new Error(`no way in. Tried ${BASES.join(', ')}${last ? ' -- ' + last : ''}`);
}
const term = () => page.$eval('#terminal', e => e.textContent);
const waitTerm = (s, ms = 180000) => page.waitForFunction(
  t => document.getElementById('terminal').textContent.includes(t), s, { timeout: ms });

try {
  await open();

  // ---- M5: the drives -----------------------------------------------
  ok('the panel draws a row per drive',
     (await page.$$eval('#drives .drive', r => r.length)) === 3);
  const opts = await page.$$eval('#drives .drive:first-child .dsel option', o => o.map(x => x.value));
  ok('every shipped medium is offered', opts.length >= 5, opts.join(' '));
  const d0 = await page.$eval('#drives .drive:first-child .dinfo', e => e.textContent);
  ok('drive 0 comes with a disk in it', /file/.test(d0) && /RO/.test(d0), d0);

  // The whole reason the store exists. Asserted, not assumed: this is
  // a plain http:// LAN origin, and if IndexedDB were unavailable here
  // every medium would be a lie.
  ok('IndexedDB is available on this origin',
     'ok' === await page.evaluate(() => new Promise(r => {
       try {
         const q = indexedDB.open('c4bb-probe', 1);
         q.onsuccess = () => { q.result.close(); r('ok'); };
         q.onerror = () => r('blocked');
       } catch { r('threw'); }
     })));

  // Written to survive a browser that has run this before. The media
  // are kept in that browser, which is the point of them, so the state
  // this starts in is whatever the last run left -- and a test that
  // needs a fresh IndexedDB would have to wipe somebody's data to get
  // one.
  const emptyBefore = await page.$$eval('#drives .dsel', s => s.map(x => x.value === ''));
  const nBefore = await page.$$eval('#usermedium option', o => o.length);
  await page.click('#newmedium');
  await page.waitForFunction(
    n => document.querySelectorAll('#usermedium option').length === n + 1, nBefore, { timeout: 10000 });
  const slots = await page.$$eval('#drives .dsel', s => s.map(x => x.value));
  const landed = slots.findIndex((v, i) => emptyBefore[i] && v && !v.startsWith('rom:'));
  ok('a new blank medium lands in a free drive', landed >= 0, `slots ${slots.join(' ')}`);
  const mineId = landed >= 0 ? slots[landed] : null;

  // ---- M6: the firmware stages ---------------------------------------
  ok('all four firmware stages are offered',
     (await page.$$eval('#firmware option', o => o.length)) === 4);

  await page.selectOption('#program', '(BIOS)');
  await drivesUp();

  // A machine that has not been built cannot boot, however good the
  // disk in it is -- which is the milestone, and it has to be true
  // here as well as on the command line.
  await page.selectOption('#firmware', 'fw-ram.c4r');
  await drivesUp();
  await page.click('#turbo');
  await waitTerm('cannot read a drive', 60000);
  ok('the half-built firmware refuses the drive it can see',
     !(await term()).includes('booting'));

  await page.selectOption('#firmware', 'fw-drives.c4r');
  await drivesUp();
  await page.click('#turbo');
  await waitTerm('cannot load an image', 60000);
  ok('the next stage sees the medium and still cannot start it',
     (await term()).includes('drive 0 has') && !(await term()).includes('booting'));

  // ---- and the finished machine does the whole thing -----------------
  await page.selectOption('#firmware', 'fw.c4r');
  await drivesUp();
  await page.click('#turbo');
  await waitTerm('bios: booting');
  ok('the BIOS probes the drives and boots one', (await term()).includes('bios: drive 0 has'));
  await waitTerm('A>');
  ok('C4DOS came up off the medium in drive 0', (await term()).includes('C4DOS version'));

  await page.click('#terminal');
  await page.keyboard.type('RUN dostar.c4r x tools-src.tar\n');
  await waitTerm('dostar: extracted');
  await page.keyboard.type('RUN bbsave.c4r B:\n');
  await waitTerm('bbsave:');
  ok('the machine wrote to the blank medium', /bbsave: [1-9][0-9]* files/.test(await term()),
     (await term()).split('\n').filter(l => l.startsWith('bbsave')).join(' '));

  // The milestone: close the page and open it again. Checked on the
  // medium THIS run made, by id -- "some medium has files on it" would
  // pass on one an earlier run left behind.
  await page.reload({ waitUntil: 'domcontentloaded' });
  await drivesUp();
  const label = await page.$eval(`#usermedium option[value="${mineId}"]`, e => e.textContent)
    .catch(() => '(not in the list)');
  ok('and it is still there after a reload', /\([1-9][0-9]* files/.test(label), label);

  ok('no errors on the page', errs.length === 0, errs.join('\n       '));

  // Take it away again. It is the user's browser and this is our
  // litter; it also exercises Delete, which nothing else does.
  await page.selectOption('#usermedium', mineId);
  await page.click('#dropmedium');
  await page.waitForFunction(
    id => ![...document.querySelectorAll('#usermedium option')].some(o => o.value === id),
    mineId, { timeout: 10000 });
  await page.reload({ waitUntil: 'domcontentloaded' });
  await drivesUp();
  const gone = await page.$$eval('#usermedium option', o => o.map(x => x.value));
  ok('and Delete really removes it', !gone.includes(mineId), gone.join(' '));
} catch (e) {
  console.log(`  FAIL ${e.message}`);
  fails++;
} finally {
  // Only the page this opened, and never the window.
  try { if (mine && !mine.isClosed()) await mine.close(); } catch { /* gone already */ }
  if (!LOCAL) {
    const tabsAfter = ctx.pages().length;
    ok("the user's tabs are as we found them", tabsAfter === tabsBefore,
       `${tabsBefore} before, ${tabsAfter} after`);
  }
  await browser.close();          // on CDP this disconnects, it does not kill
  server.close();
}

console.log(fails ? `test-web: ${fails} FAILED` : 'test-web: OK');
process.exit(fails ? 1 : 0);
