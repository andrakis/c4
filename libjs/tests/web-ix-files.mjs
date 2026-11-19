// web-ix-files.mjs - the C4IX desktop's Files in the browser
// (docs/c4ix-desktop.md, part fifteen): files given to the page's Uploads
// listed in /home, and the list sorted by its column headers and View >
// Sort; the splitter between the folder tree and the list, dragged; and
// the right click's menus, Open in IDE among them for a source or a
// program. Over CDP, in a tab of its own in one of the user's Chromes
// (cdp.mjs), against a page served from this tree:
//
//   node libjs/serve.mjs PORT --isolate &
//   node libjs/tests/web-ix-files.mjs http://127.0.0.1:9222 https://PORT.code.home.stargazer.onl OUTDIR
//
// The uploads are put in the page's store for the check and taken out
// again after it, and every boot is ?persist=0, so the system's files are
// neither read from nor kept in that origin's store.

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
  const page = `${base}/libjs/web/?system=c4ix&persist=0&mmu=1`;
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
  // the tree starts with only the root open: /usr, not /usr/src or /usr/docs
  ok('the tree starts with only the root open', (await find('usr')).length > 0 && !(await find('src')).length && !(await find('docs')).length);
  // ... with its folders by name: the tree's string of each is the leftmost
  const treeYs = [];
  for (const n of ['bin', 'build', 'home', 'tmp', 'usr']) treeYs.push(((await find(n)).sort((p, q) => p.x - q.x)[0] || { y: -1 }).y);
  ok("the tree lists the root's folders by name", treeYs.every((y, i) => y >= 0 && (i === 0 || y > treeYs[i - 1])), treeYs.join(' '));
  await shot('root');
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
  // ---- the splitter between the tree and the list ----
  // (a window's title is the highest string of it: its taskbar button, lower,
  // would minimise a window already in front)
  const sleepUntil = async (f, t = 8000) => { const end = Date.now() + t; while (Date.now() < end) { if (await f()) return true; await sleep(250); } return !!await f(); };
  const pressAt = async (x, y, button) => {
    const p = await at(x, y);
    await tab.mouse('mouseMoved', p.x, p.y);
    await tab.mouse('mousePressed', p.x, p.y, button, button === 'right' ? 2 : 1);
    await tab.mouse('mouseReleased', p.x, p.y, button, 0);
    await sleep(400);
  };
  const dragAt = async (x0, y0, x1, y1) => {
    const p = await at(x0, y0), q = await at(x1, y1);
    await tab.mouse('mouseMoved', p.x, p.y); await tab.mouse('mousePressed', p.x, p.y, 'left', 1);
    for (let k = 1; k <= 5; k++) { await tab.mouse('mouseMoved', p.x + ((q.x - p.x) * k) / 5, p.y + ((q.y - p.y) * k) / 5, 'left', 1); await sleep(40); }
    await tab.mouse('mouseReleased', q.x, q.y, 'left', 0);
    await sleep(400);
  };
  {
    const t = (await find('Files - /home')).sort((p, q) => p.y - q.y)[0];
    const wx = t.x - 27, wy = t.y - 7;
    const nx = (await inList('alpha.txt')).x;
    const sx = wx + 4 + 2 + 180 + 3, sy = wy + 200;
    await dragAt(sx, sy, sx + 80, sy);
    ok('dragging the bar between the tree and the list widens the tree', await sleepUntil(async () => (await inList('alpha.txt'))?.x === nx + 80), JSON.stringify(await inList('alpha.txt')));
    await shot('splitter');
    // ---- the right click's menus ----
    const rclick = async n => { const c = await inList(n); await pressAt(c.x + 5, c.y + 5, 'right'); };
    await rclick('alpha.txt');
    ok('a right click on a text file: Open, Rename, Delete, but not Open in IDE',
      await shownHas('Rename', 5000) && await shownHas('Delete', 5000) && !(await find('Open in IDE')).length);
    await shot('context-menu-text');
    await clickT((await find('Delete')).pop());
    ok('Delete asks first', await shownHas('Delete File', 5000) && await shownHas("Delete 'alpha.txt'?", 5000));
    await shot('context-menu-delete-asks');
    await clickT((await find('No')).pop());
    ok('and No leaves it', await sleepUntil(async () => !(await find('Delete File')).length) && !!await inList('alpha.txt'));
    await rclick('charlie.c');
    ok('on a C source: Open in IDE', await shownHas('Open in IDE', 5000));
    await shot('context-menu-source');
    await clickT((await find('Open in IDE')).pop());
    ok('which opens it in the IDE', await shownHas('charlie.c - IDE', 15000));
    await shot('open-source-in-ide');
    await clickT((await find('File')).pop());
    await shownHas('Close', 5000);
    await clickT((await find('Close')).pop());
    await sleepUntil(async () => !(await find('charlie.c - IDE')).length);
    // a program, in /bin: Open in IDE is its code in the IDE's Code tab
    await clickT((await find('Files - /home')).sort((p, q) => p.y - q.y)[0]);
    await tab.key('Backspace');
    await shownHas('Files - /', 8000);
    await clickT(await inList('bin'), true);
    ok('/bin', await shownHas('Files - /bin', 8000) && await sleepUntil(async () => !!await inList('cat')));
    await rclick('cat');
    ok('on a program: Open in IDE', await shownHas('Open in IDE', 5000));
    await shot('context-menu-program');
    await clickT((await find('Open in IDE')).pop());
    ok("which shows the program's code in the IDE's Code tab", await shownHas('cat - IDE', 20000) &&
      await tab.waitFor(`${shown}.some(t => /^main \\(\\d+ words/.test(t.s))`, 20000).then(v => !!v, () => false));
    await shot('open-program-in-ide');
    await clickT((await find('File')).pop());
    await shownHas('Close', 5000);
    await clickT((await find('Close')).pop());
    await sleepUntil(async () => !(await find('cat - IDE')).length);
    // the splitter back where it was
    const t2 = (await find('Files - /bin')).sort((p, q) => p.y - q.y)[0];
    await dragAt(t2.x - 27 + 4 + 2 + 260 + 3, t2.y - 7 + 200, t2.x - 27 + 4 + 2 + 180 + 3, t2.y - 7 + 200);
  }
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
