// web-puzzles.mjs - Simon Tatham's Puzzles on C4IX, in a browser
// (games/puzzles/c4/README.md): Net on the whole display, played with the
// mouse and the keyboard, its buttons, the desktop back when it ends, then
// in a Terminal window; and a few more games, for their text. Over CDP, in a
// tab of its own in one of the user's Chromes (cdp.mjs), against a page served
// from this tree:
//
//   node libjs/serve.mjs PORT --isolate &
//   node libjs/tests/web-puzzles.mjs http://127.0.0.1:9222 http://HOST:PORT OUTDIR
//
// ?persist=reset, so it leaves nothing in the page's c4ix store.

import { writeFileSync, mkdirSync } from 'node:fs';
import { Browser } from './cdp.mjs';

const [cdp = 'http://127.0.0.1:9222', base = 'http://127.0.0.1:8472', outdir = '/tmp/web-puzzles'] = process.argv.slice(2);
mkdirSync(outdir, { recursive: true });
let fail = 0;
const ok = (what, cond, extra = '') => {
  console.log(`web-puzzles: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-800) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

const b = await Browser.connect(cdp);
console.log(`web-puzzles: ${b.version}`);
const tab = await b.newTab(false);
try {
  const shown = 'window.c4m.display.shownText';
  const shownHas = (s, t = 20000) => tab.waitFor(`${shown}.some(t => ${s instanceof RegExp ? `${s}.test(t.s)` : `t.s.trimEnd() === ${JSON.stringify(s)}`})`, t);
  const find = async s => tab.eval(`${shown}.filter(t => ${s instanceof RegExp ? `${s}.test(t.s)` : `t.s.trimEnd() === ${JSON.stringify(s)}`}).map(t => ({ x: t.x, y: t.y, s: t.s }))`);
  const at = async (x, y) => {
    const w = await tab.eval("document.getElementById('display').width"), h = await tab.eval("document.getElementById('display').height");
    return tab.eval(`(() => { const r = document.getElementById('display').getBoundingClientRect();
      return { x: r.left + r.width * ${x / w}, y: r.top + r.height * ${y / h} }; })()`);
  };
  const clickAt = async (x, y, button = 'left') => {
    const p = await at(x, y);
    const bits = button === 'left' ? 1 : 2;
    await tab.mouse('mouseMoved', p.x, p.y);
    await tab.mouse('mousePressed', p.x, p.y, button, bits);
    await tab.mouse('mouseReleased', p.x, p.y, button, 0);
    await sleep(100);
  };
  const shot = async name => { const png = await tab.screenshot(); writeFileSync(`${outdir}/${name}.png`, png); console.log(`web-puzzles: screenshot ${name}`); return png; };
  // the display's pixels, summed: has what was drawn changed?
  const sum = () => tab.eval(`(() => { const c = document.getElementById('display'); const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data;
    let h = 0; for (let i = 0; i < d.length; i += 4) h = (Math.imul(h, 31) + d[i] + d[i + 1] * 3 + d[i + 2] * 7) | 0; return h; })()`);
  const changed = async (before, t = 4000) => {
    for (let n = 0; n < t / 100; n++) { if (await sum() !== before) return true; await sleep(100); }
    return false;
  };

  await tab.goto(`${base}/libjs/web/?persist=reset&mmu=1`);
  const up = await tab.waitFor('window.c4m && window.c4m.display', 90000);
  await tab.eval('window.c4m.display.recordText = true');
  ok('the page boots the C4IX desktop, a Terminal open', up && await shownHas(/^Terminal - task/, 120000));
  await sleep(1500);
  const H = await tab.eval("document.getElementById('display').height");
  const term = async () => { const t = (await find(/^Terminal - task/)).filter(t => t.y < H - 30).pop(); await clickAt(t.x + 5, t.y + 5); };
  await term();

  // ---- Net, the same game test-puzzles plays, on the whole display ----
  await tab.type('net 5x5:85a78294523ebdb592379c831\n');
  await sleep(6000);
  const w = await tab.eval("document.getElementById('display').width");
  ok('the display is the game\'s, 640 across', w === 640, w);
  await shot('net-1-start');
  // the board, twice its size, is centred under the buttons: 385 square at (124, 49)
  const OX = 124, OY = 49, TILE = 64, MARGIN = 32;
  const tile = (x, y) => [OX + MARGIN + TILE * x + TILE / 2, OY + MARGIN + TILE * y + TILE / 2];
  let s = await sum();
  await clickAt(...tile(2, 2));
  ok('a left click turns a tile', await changed(s));
  await sleep(600);
  await shot('net-2-click');
  s = await sum();
  await clickAt(...tile(0, 0), 'right');
  ok('a right click turns one the other way', await changed(s));
  await sleep(600);
  s = await sum();
  await tab.type('u');
  ok('u undoes', await changed(s));
  await sleep(600);
  s = await sum();
  for (const [key, vk] of [['ArrowRight', 39], ['ArrowDown', 40]]) {
    await tab.send('Input.dispatchKeyEvent', { type: 'rawKeyDown', key, code: key, windowsVirtualKeyCode: vk });
    await sleep(60);
    await tab.send('Input.dispatchKeyEvent', { type: 'keyUp', key, code: key, windowsVirtualKeyCode: vk });
    await sleep(150);
  }
  ok('the arrow keys move a cursor', await changed(s));
  await sleep(300);
  await shot('net-3-cursor');
  // the buttons: New Restart Undo Redo Solve Type Quit, across the top
  const button = i => [4 + (i + 0.5) * (640 - 8) / 7, 10];
  s = await sum();
  await clickAt(...button(4));
  ok('Solve solves it', await changed(s));
  await sleep(2500);
  await shot('net-4-solved');
  s = await sum();
  await clickAt(...button(5));
  ok('Type gives the next size', await changed(s));
  await sleep(1000);
  await shot('net-5-type');
  s = await sum();
  await clickAt(...button(0));
  ok('New makes a new game', await changed(s));
  await sleep(800);
  await clickAt(...button(6));
  await sleep(4000);
  ok('Quit: the desktop has its display back', await shownHas(/^Terminal - task/, 30000));
  await shot('net-6-desktop');

  // ---- --window: in the Terminal it was started from ----
  await term();
  await tab.eval(`${shown}.length = 0`).catch(() => {});
  await tab.type('net --window 5x5:85a78294523ebdb592379c831\n');
  await sleep(5000);
  await shot('net-7-window');
  // the middle of the board, at its place in the window on an 800x600 desktop
  let ws = await sum();
  await clickAt(425, 266);
  ok('a click in the window turns a tile', await changed(ws));
  await sleep(800);
  ws = await sum();
  await tab.type('q');
  ok('q in the window: the Terminal again', await changed(ws));
  await sleep(2000);

  // ---- the rest, for their text and their shapes ----
  for (const g of ['mines', 'solo', 'keen', 'untangle', 'map', 'loopy', 'tracks']) {
    await term();
    await tab.type(`${g}\n`);
    await sleep(g === 'keen' || g === 'loopy' ? 12000 : 7000);
    await shot(`game-${g}`);
    await tab.key('Escape');
    await sleep(3000);
  }
} catch (e) {
  ok('the run', false, e && e.stack || e);
} finally {
  await tab.close();
  b.disconnect();
}
console.log(fail ? 'web-puzzles: FAILED' : 'web-puzzles: OK');
process.exit(fail);
