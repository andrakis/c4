// web-quake-zip.mjs - Quake on the page's bare machine from a games disk
// built with QUAKE_AS_ZIP=1 (games/quake/data/README.md): no pak0.pak on
// it, only quake106.zip and its recipe, so the worker makes pak0.pak when
// the game opens it (unpack.js), and the page keeps it (lazy.js). Twice:
// the first visit fetches the zip and makes it, the next has it from the
// page's cache and fetches neither. Over CDP, in a tab of its own:
//
//   QUAKE_AS_ZIP=1 make c4bb-images; node libjs/serve.mjs PORT &
//   node libjs/tests/web-quake-zip.mjs CDP BASE
import { Browser } from './cdp.mjs';

const [cdp = 'http://127.0.0.1:9222', base = 'https://8494.code.home.stargazer.onl'] = process.argv.slice(2);
let fail = 0;
const ok = (what, cond, extra = '') => {
  console.log(`web-quake-zip: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-800) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

const b = await Browser.connect(cdp);
console.log(`web-quake-zip: ${b.version}, ${base}`);
const tab = await b.newTab(false);
try {
  const visit = async () => {
    const t0 = Date.now();
    await tab.goto(`${base}/libjs/web/?system=quake&persist=0`);
    if (await tab.eval('location.pathname') !== '/libjs/web/') throw new Error('not the page: ' + await tab.eval('location.href'));
    for (let i = 0; i < 40 && !await tab.eval('!!window.c4m'); i++) await sleep(250);
    await tab.eval(`window.__out = ''; const td = new TextDecoder();
      window.c4m.on('out', b => { window.__out += td.decode(new Uint8Array(b), { stream: true }); })`);
    // until the game has read its pak (the disk's stats say what was fetched) and drawn a while
    let st = null;
    for (let i = 0; i < 240; i++) {
      await sleep(500);
      st = await tab.eval('JSON.stringify(window.c4m.status && window.c4m.status.disk || null)');
      const d = st && JSON.parse(st);
      if (d && (d.cached || (d.made && d.made.length))) break;
    }
    const at = Date.now() - t0;
    await sleep(8000);
    return { st: JSON.parse(st || 'null'), at, out: await tab.eval('window.__out'),
             state: await tab.eval('window.c4m.status && window.c4m.status.state') };
  };
  const first = await visit();
  console.log(`web-quake-zip: first visit: disk ${JSON.stringify(first.st)}, pak read ${first.at} ms after the page was opened, ${first.state}`);
  ok('the first visit fetched the zip, made pak0.pak from it and fetched none', first.st && first.st.made && first.st.made.length === 1 && first.st.bytes > 9e6 && first.st.bytes < 12e6, JSON.stringify(first.st));
  ok('... and Quake did not say it has no pak0', !/no pak0\.pak|could not be made/.test(first.out), first.out);
  ok('... and runs', first.state === 'running', first.state);
  // the page keeps it: the next visit has it from the cache
  await sleep(3000);
  const next = await visit();
  console.log(`web-quake-zip: next visit: disk ${JSON.stringify(next.st)}, pak read ${next.at} ms after the page was opened, ${next.state}`);
  ok('the next visit had pak0.pak from the cache, and fetched no zip', next.st && next.st.cached >= 1 && !(next.st.made || []).length && next.st.bytes < 5e6, JSON.stringify(next.st));
  ok('... and runs', next.state === 'running' && !/no pak0\.pak/.test(next.out), next.out);
} finally {
  await tab.close();
  b.disconnect();
}
process.exit(fail);
