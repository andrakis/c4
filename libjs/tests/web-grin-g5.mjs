// web-grin-g5.mjs - grin's G5 browser check (docs/grin-plan.md G5):
// Wolfenstein 3D full screen from Program Manager, played a little and
// quit back to grin; then a DOS Prompt with a pipe, a redirect onto B:
// and TYPE. Over CDP, in a tab of its own in one of the user's Chromes
// (cdp.mjs), against a page served from this tree:
//
//   node libjs/serve.mjs PORT &
//   node libjs/tests/web-grin-g5.mjs http://127.0.0.1:9222 https://PORT.code.home.stargazer.onl OUTDIR
//
// (through the https proxy rather than http://HOST:PORT: a plain-http page
// on another host is not a secure context, and has no SharedArrayBuffer)
//
// It starts and ends with ?persist=reset, so it leaves nothing in the
// page's c4dos store.

import { writeFileSync, mkdirSync } from 'node:fs';
import { Browser } from './cdp.mjs';

const [cdp = 'http://127.0.0.1:9222', base = 'http://127.0.0.1:8472', outdir = '/tmp/web-grin-g5'] = process.argv.slice(2);
mkdirSync(outdir, { recursive: true });
let fail = 0;
const ok = (what, cond, extra = '') => {
  console.log(`web-grin-g5: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-800) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

const b = await Browser.connect(cdp);
console.log(`web-grin-g5: ${b.version}`);
const tab = await b.newTab(false);
try {
  const shown = 'window.c4m.display.shownText';
  const shownHas = (s, t = 20000) => tab.waitFor(`${shown}.some(t => t.s.trimEnd() === ${JSON.stringify(s)})`, t);
  const shownRe = (re, t = 20000) => tab.waitFor(`${shown}.some(t => ${re}.test(t.s.trimEnd()))`, t);
  const find = async s => tab.eval(`${shown}.filter(t => t.s.trimEnd() === ${JSON.stringify(s)})`);
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
  const clickT = async (t, dbl = false) => { if (!t) return false; await clickAt(t.x + 5, t.y + 5); if (dbl) await clickAt(t.x + 5, t.y + 5); await sleep(400); return true; };
  const clickText = async (s, dbl = false) => { const all = await find(s); return clickT(all[all.length - 1], dbl); };
  // Program Manager to the front by its title, a group from its list, an item
  const item = async (group, s) => {
    await clickText('Program Manager');
    await clickT((await find(group)).sort((a, c) => a.x - c.x)[0]);
    const all = await find(s);
    return clickT(all.filter(t => t.x > 140)[0] || all[all.length - 1], true);
  };
  // a key held for ms (the arrows and Ctrl, which cdp.mjs's key() does not name)
  const KEYS = {
    ArrowUp: 38, ArrowDown: 40, ArrowLeft: 37, ArrowRight: 39, Enter: 13, Escape: 27, Control: 17, y: 89, Y: 89, ' ': 32,
  };
  const hold = async (key, ms = 120) => {
    const vk = KEYS[key] || key.toUpperCase().charCodeAt(0);
    const text = key === 'Enter' ? '\r' : key.length === 1 ? key : undefined;
    const code = key === 'Control' ? 'ControlLeft' : key === ' ' ? 'Space' : key.length === 1 ? `Key${key.toUpperCase()}` : key;
    await tab.send('Input.dispatchKeyEvent', { type: text ? 'keyDown' : 'rawKeyDown', key, code, windowsVirtualKeyCode: vk, text });
    await sleep(ms);
    await tab.send('Input.dispatchKeyEvent', { type: 'keyUp', key, code, windowsVirtualKeyCode: vk });
    await sleep(150);
  };
  const shot = async name => { writeFileSync(`${outdir}/${name}.png`, await tab.screenshot()); console.log(`web-grin-g5: screenshot ${outdir}/${name}.png`); };
  const frames = () => tab.eval('window.c4m.display.framesDrawn | 0');
  const boot = async query => {
    await tab.goto(`${base}/libjs/web/?system=c4dos${query}`);
    const up = await tab.waitFor('window.c4m && document.getElementById("terminal").innerText.includes("Starting grin")', 90000);
    await tab.eval('window.c4m.display.recordText = true');
    await sleep(500);
    await clickAt(620, 300);               // the display gets the keyboard, and grin draws everything again
    return up && await shownHas('Program Manager');
  };

  ok('?persist=reset: the page boots into grin', await boot('&persist=reset'));
  ok('the disk has REDIR.SYS TAP', await tab.waitFor('document.getElementById("terminal").innerText.includes("with the console tap")', 5000));

  // ---- Wolfenstein 3D, full screen --------------------------------------------------
  await clickText('Games'); await sleep(300);
  await shot('grin-g5-1-games');
  const f0 = await frames();
  await item('Games', 'Wolfenstein 3D');
  ok('Games > Wolfenstein 3D: grin gives it the display, and its frames arrive',
    await tab.waitFor(`window.c4m.display.framesDrawn > ${f0} + 5 && !${shown}.some(t => t.s.trimEnd() === 'Program Manager')`, 90000));
  await sleep(8000);
  await shot('grin-g5-2-wolf3d-signon');
  // the sign-on, the PC-13 screen, the title; on a fresh config the menu
  // comes up on Read This!, which the last Enter may already have opened:
  // Esc leaves it, or, at the menu, asks to quit, which N declines. The
  // cursor is on Read This! either way, and four down is New Game.
  for (let i = 0; i < 3; i++) { await hold('Enter'); await sleep(4000); }
  await hold('Escape'); await sleep(2500);
  await hold('n'); await sleep(1500);
  for (let i = 0; i < 4; i++) { await hold('ArrowDown'); await sleep(600); }
  await shot('grin-g5-3-wolf3d-menu');
  // New Game, the episode, the difficulty
  for (let i = 0; i < 3; i++) { await hold('Enter'); await sleep(2500); }
  await sleep(4000);
  await shot('grin-g5-4-wolf3d-floor1');
  // a little play: forward, a turn, a shot
  await hold('ArrowUp', 1200);
  await hold('ArrowRight', 700);
  await hold('ArrowUp', 800);
  await hold('Control', 150);
  await sleep(800);
  await hold('ArrowLeft', 1400);
  await hold('ArrowUp', 1000);
  await sleep(500);
  await shot('grin-g5-5-wolf3d-playing');
  // Esc for the menu: Back to Game, and Quit under it
  await hold('Escape'); await sleep(2000);
  await hold('ArrowUp'); await sleep(800);
  await shot('grin-g5-6-wolf3d-quit');
  await hold('Enter'); await sleep(1500);
  await hold('y');
  ok('quit from its menu: grin takes the display back and draws Program Manager', await shownHas('Program Manager', 30000));
  await sleep(1000);
  await shot('grin-g5-7-back-in-grin');

  // ---- a DOS Prompt: a pipe, a redirect onto B:, TYPE --------------------------------
  await item('Main', 'DOS Prompt');
  ok('Main > DOS Prompt opens DOS Prompt 1', await shownHas('DOS Prompt 1') && await shownHas('EXIT closes it.'));
  await tab.type('DIR | DOSFILT\n');
  ok('DIR | DOSFILT in it: the listing through the filter', await shownRe('/^> WOLF3D\\.C4R$/i') && await shownRe('/^dosfilt: \\d+ lines$/'));
  await tab.type('ECHO typed in a DOS Prompt window > B:G5.TXT\n');
  await tab.type('TYPE B:G5.TXT\n');
  ok('ECHO ... > B:G5.TXT, then TYPE B:G5.TXT', await shownHas('typed in a DOS Prompt window'));
  await tab.type('HELLO\n');
  ok("HELLO: a c4cc program's printf, through the console tap", await shownHas('yello'));
  await sleep(500);
  await shot('grin-g5-8-dos-prompt');
  await tab.type('EXIT\n');
  ok('EXIT closes it', await tab.waitFor(`!${shown}.some(t => t.s.trimEnd() === 'DOS Prompt 1')`, 10000));
  ok('no page errors', tab.errors.length === 0, tab.errors.join('\n  '));
  ok('?persist=reset at the end: nothing left in the store', await boot('&persist=reset'));
} finally {
  await tab.close();
  b.disconnect();
}
console.log(fail ? 'web-grin-g5: FAILED' : 'web-grin-g5: OK');
process.exit(fail);
