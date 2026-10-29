// web-ix-files.mjs - the C4IX desktop's Files in the browser
// (docs/c4ix-desktop.md, part fifteen): files given to the page's Uploads
// listed in /home, and the list sorted by its column headers and View >
// Sort. Over CDP, in a tab of its own in one of the user's Chromes
// (cdp.mjs), against a page served from this tree:
//
//   node libjs/serve.mjs PORT --isolate &
//   node libjs/tests/web-ix-files.mjs http://127.0.0.1:9222 https://PORT.code.home.stargazer.onl OUTDIR
//
// The uploads are put in the page's store for the check and taken out
// again after it, and every boot is ?persist=reset, so it leaves nothing
// behind in that origin's stores.

import { writeFileSync, mkdirSync } from 'node:fs';
import { Browser } from './cdp.mjs';

const [cdp = 'http://127.0.0.1:9222', base = 'http://127.0.0.1:8472', outdir = '/tmp/web-ix-files'] = process.argv.slice(2);
mkdirSync(outdir, { recursive: true });
let fail = 0;
const ok = (what, cond, extra = '') => {
  console.log(`web-ix-files: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-800) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));
const UPLOADS = [['bravo.dat', 500], ['charlie.c', 9000], ['alpha.txt', 3000]];
// the page's own store (page.js keeps one for the system's saved files and the uploads)
const store = 'window.c4m.store';

const b = await Browser.connect(cdp);
console.log(`web-ix-files: ${b.version}`);
const tab = await b.newTab(false);
try {
  const page = `${base}/libjs/web/?system=c4ix&persist=reset`;
  // window.c4m is the page's machine. The page can reload itself after it
  // is up (coi.js isolating it on a first visit, ../coi.js), so what needs
  // the machine is done in a waitFor, which tries again until it can.
  // (waitFor answers false when its time is up)
  await tab.goto(page);
  if (await tab.eval('location.pathname') !== '/libjs/web/') {
    ok('the page loads (this browser must be signed in to the proxy on that port)', false, await tab.eval('location.href'));
    throw new Error('not the page');
  }
  ok('the uploads go into the page\'s store', !!await tab.waitFor(`(async () => { const s = ${store};
    for (const [n, z] of ${JSON.stringify(UPLOADS)}) await s.backend.putFile('uploads', n, new TextEncoder().encode(('uploaded ' + n + '\\n').repeat(z)).slice(0, z));
    return true; })()`, 60000));
  await tab.goto(page);

  // the strings of the last frame shown, recorded by whichever page is up
  const shown = '(window.c4m.display.recordText = true, window.c4m.display.shownText)';
  const shownHas = (s, t = 20000) => tab.waitFor(`${shown}.some(t => t.s.trimEnd() === ${JSON.stringify(s)})`, t).then(v => !!v, () => false);
  const find = async s => tab.eval(`${shown}.filter(t => t.s.trimEnd() === ${JSON.stringify(s)})`).catch(() => []);
  const at = async (x, y) => {
    const w = await tab.eval("document.getElementById('display').width"), h = await tab.eval("document.getElementById('display').height");
    return tab.eval(`(() => { const r = document.getElementById('display').getBoundingClientRect();
      return { x: r.left + r.width * ${x / w}, y: r.top + r.height * ${y / h} }; })()`);
  };
  const clickAt = async (x, y) => {
    const p = await at(x, y);
    await tab.mouse('mouseMoved', p.x, p.y); await tab.mouse('mousePressed', p.x, p.y, 'left', 1); await tab.mouse('mouseReleased', p.x, p.y, 'left', 0);
    await sleep(60);
  };
  const clickT = async (t, dbl = false) => { if (!t) return false; await clickAt(t.x + 5, t.y + 5); if (dbl) await clickAt(t.x + 5, t.y + 5); await sleep(500); return true; };
  // in Files' list, right of its tree: the rightmost string of that text
  const inList = async s => (await find(s)).sort((p, q) => p.x - q.x).pop();
  const shot = async name => writeFileSync(`${outdir}/${name}.png`, await tab.screenshot());
  // the list's order, top to bottom
  const names = ['alpha.txt', 'bravo.dat', 'charlie.c'];
  const order = async () => {
    const ys = [];
    for (const n of names) { const t = await inList(n); if (!t) return 'missing ' + n; ys.push([n, t.y]); }
    return ys.sort((p, q) => p[1] - q[1]).map(p => p[0]).join(' ');
  };
  const orderIs = async (want, t = 8000) => {
    const end = Date.now() + t;
    while (Date.now() < end) { if (await order() === want) return true; await sleep(300); }
    return false;
  };

  ok('the desktop comes up', await shownHas('Filesystem', 180000));
  await sleep(1500);
  await clickT((await find('Filesystem')).pop(), true);
  ok('Filesystem opens Files at the root', await shownHas('Files - /'));
  ok('the root lists /home', !!await inList('home'), JSON.stringify(await find('home')));
  await clickT(await inList('home'), true);
  ok('/home lists the uploads, by name', await shownHas('Files - /home') && await orderIs('alpha.txt bravo.dat charlie.c'), await order());
  await shot('home-by-name');
  await clickT(await inList('Size'));
  ok("clicking Size's header sorts by size", await orderIs('bravo.dat alpha.txt charlie.c'), await order());
  await clickT(await inList('Size'));
  ok('and again, the largest first', await orderIs('charlie.c alpha.txt bravo.dat'), await order());
  await shot('home-by-size-descending');
  await clickT((await find('View')).pop());
  ok('View has the sort items', await shownHas('Sort by Type', 5000) && await shownHas('Descending', 5000));
  await shot('view-menu');
  await clickT((await find('Sort by Type')).pop());
  ok('View > Sort by Type, still descending: txt dat c', await orderIs('alpha.txt bravo.dat charlie.c'), await order());
  await clickT(await inList('Name'));
  ok("clicking Name's header: by name again", await orderIs('alpha.txt bravo.dat charlie.c'), await order());
  ok('no errors on the page', !tab.errors.length, tab.errors.join('\n'));
} finally {
  try {
    await tab.waitFor(`(async () => { const s = ${store};
      for (const [n] of ${JSON.stringify(UPLOADS)}) await s.backend.deleteFile('uploads', n); return true; })()`, 30000);
  } catch (e) { console.log(`web-ix-files: could not take the uploads out again: ${e.message}`); }
  await tab.close();
  b.disconnect();
}
console.log(`web-ix-files: ${fail ? 'FAILED' : 'all OK'}`);
process.exit(fail);
