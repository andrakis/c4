// test-web.mjs - the c4m.js page in one of the user's real browsers.
//
//   node libjs/tests/test-web.mjs               # C4IX and C4KE legs
//   node libjs/tests/test-web.mjs --gui         # the display legs (M3/M4)
//   LIBJS_CDP=http://127.0.0.1:9222 node libjs/tests/test-web.mjs
//
// Raw CDP (tests/cdp.mjs), no Playwright and never a local headless
// browser: the endpoints are the user's own Chromes, 9224 (the 3060 Ti
// desktop) and then 9222 (the Chromebook). If neither answers, this says
// so and fails; it does not go looking for another browser.
//
// The page is served from here and reached through the code-server proxy
// (https://PORT.code.home.stargazer.onl), falling back to the LAN
// address. If the proxy asks for a login, the code-server password from
// ~/.config/code-server/config.yaml is used (authorised 2026-09-11).
//
// Only the tab this opens is closed.

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { homedir, networkInterfaces } from 'node:os';
import { join } from 'node:path';
import { Browser } from './cdp.mjs';
import { makeServer } from '../serve.mjs';

const PORT = Number(process.env.LIBJS_PORT || 8473);
const GUI = process.argv.includes('--gui');
const SHOTS = process.env.LIBJS_SHOTS || null;
const CDPS = process.env.LIBJS_CDP ? [process.env.LIBJS_CDP] : ['http://127.0.0.1:9224', 'http://127.0.0.1:9222'];

const lan = Object.values(networkInterfaces()).flat()
  .find(n => n && n.family === 'IPv4' && !n.internal && !n.address.startsWith('172.17.'))?.address || '127.0.0.1';
const BASES = process.env.LIBJS_URL ? [process.env.LIBJS_URL]
  : [`https://${PORT}.code.home.stargazer.onl`, `http://${lan}:${PORT}`];

let password = null;
try {
  const m = /^password:\s*(.+)$/m.exec(readFileSync(join(homedir(), '.config/code-server/config.yaml'), 'utf8'));
  if (m) password = m[1].trim();
} catch { /* no code-server config */ }

let fails = 0;
const ok = (name, cond, extra = '') => {
  console.log(`  ${cond ? 'ok  ' : 'FAIL'} ${name}${!cond && extra ? '\n       ' + extra : ''}`);
  if (!cond) fails++;
  return cond;
};

const server = makeServer();
await new Promise(r => server.listen(PORT, '0.0.0.0', r));

let browser = null, cdp = null;
for (const c of CDPS) {
  try { browser = await Browser.connect(c); cdp = c; break; } catch { /* next */ }
}
if (!browser) {
  console.log(`test-web: no browser answered on ${CDPS.join(' or ')}.`);
  console.log('          The CDP tunnel to the desktop or the Chromebook is down; nothing was run.');
  server.close();
  process.exit(2);
}
console.log(`test-web: ${browser.version} at ${cdp}`);

const tab = await browser.newTab();
const shot = async name => {
  if (!SHOTS) return;
  mkdirSync(SHOTS, { recursive: true });
  writeFileSync(join(SHOTS, `${name}.png`), await tab.screenshot());
};

// Open the page, through the code-server login if the proxy asks.
async function open(query) {
  for (const base of BASES) {
    const url = `${base}/libjs/web/?${query}`;
    try {
      await tab.goto(url);
      if (await tab.eval('/\\/login/.test(location.pathname)')) {
        if (!password) continue;
        await tab.eval(`(() => { const i = document.querySelector('input[name=password]'); i.value = ${JSON.stringify(password)};
                                 i.form.requestSubmit ? i.form.requestSubmit() : i.form.submit(); })()`);
        await tab.waitFor('!/\\/login/.test(location.pathname)', 15000);
        console.log('  (signed the browser into code-server)');
        await tab.goto(url);
      }
      if (await tab.waitFor('!!window.c4m', 15000)) return base;
    } catch (e) { console.log(`  (${base}: ${e.message})`); }
  }
  return null;
}
const term = () => tab.eval('document.getElementById("terminal").innerText');
const waitTerm = (s, t = 30000) => tab.waitFor(`document.getElementById("terminal").innerText.includes(${JSON.stringify(s)})`, t);
const focusTerm = () => tab.eval('document.getElementById("terminal").focus()');
const sleep = ms => new Promise(r => setTimeout(r, ms));

try {
  if (!GUI) {
    // ---- C4IX -----------------------------------------------------------
    console.log('test-web: C4IX');
    const base = await open('system=c4ix');
    if (!ok('the page loads and window.c4m exists', !!base, BASES.join(', '))) throw new Error('no page');
    console.log(`  (served at ${base})`);
    ok('C4IX boots to its shell', await waitTerm('c4ix-sh', 90000) && await waitTerm('c4ix:/$', 30000),
       (await term()).slice(-400));
    await focusTerm();
    await tab.type('ps\n');
    ok('typed ps lists the tasks', await waitTerm('tasks,', 30000), (await term()).slice(-400));
    await sleep(3000);
    const idle = await tab.eval('window.c4m.status');
    ok(`idle at the prompt: ${idle.state}, cpu ${Math.round(idle.busy * 100)}%`,
       (idle.state === 'sleeping' || idle.state === 'blocked') && idle.busy < 0.25, JSON.stringify(idle));
    await tab.type('spin\n');
    ok('spin starts', await waitTerm('spin: tick 2/', 30000));
    await tab.key('c', { ctrl: true });
    ok('Ctrl-C cancels it', await waitTerm('interrupt: cancelling task', 15000), (await term()).slice(-300));
    await tab.type('echo after-the-interrupt\n');
    ok('the shell still answers', await waitTerm('after-the-interrupt\n', 15000) || await waitTerm('after-the-interrupt', 1));
    const busy = await tab.eval('window.c4m.status');
    console.log(`  (${busy.ips} inst/s at the last status, simulated ${Math.round(busy.simMs / 1000)}s)`);
    await shot('c4ix');
    ok('no page errors', tab.errors.length === 0, tab.errors.join('\n       '));

    // ---- C4KE -----------------------------------------------------------
    console.log('test-web: C4KE');
    tab.errors.length = 0;
    await open('system=c4ke');
    ok('C4KE boots to c4sh', await waitTerm('c4sh>', 90000), (await term()).slice(-400));
    await focusTerm();
    await tab.type('hello.c4r\n');
    ok('runs a program from the disk', await waitTerm('yello', 30000));
    await shot('c4ke');
    ok('no page errors', tab.errors.length === 0, tab.errors.join('\n       '));
  } else {
    await import('./test-web-gui.mjs').then(m => m.run({ tab, open, ok, term, waitTerm, focusTerm, sleep, shot }));
  }
} catch (e) {
  ok(`gate ran to the end (${e.message})`, false);
} finally {
  await tab.close();
  browser.disconnect();
  server.close();
}
console.log(fails ? `test-web: ${fails} FAILED` : 'test-web: OK');
process.exit(fails ? 1 : 0);
