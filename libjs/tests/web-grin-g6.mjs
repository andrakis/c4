// web-grin-g6.mjs - grin's G6 browser check (docs/grin-plan.md G6): Doom
// in a window of grin's beside a Clock and Notepad, typing in Notepad
// while Doom's demo plays; its Size menu; End Task closing it. Then each
// game's frames per second, in a window and full screen (its Program
// Manager item set to full screen by Properties), counted as the 8-bit
// frames the page draws. Over CDP, in a tab of its own in one of the
// user's Chromes (cdp.mjs), against a page served from this tree:
//
//   node libjs/serve.mjs PORT &
//   node libjs/tests/web-grin-g6.mjs http://127.0.0.1:9222 https://PORT.code.home.stargazer.onl OUTDIR [SECONDS [WHAT]]
//
// WHAT: a comma-separated list of what to do, of "doom" (the first part)
// and the games' names; all of it when it is left out.
//
// (through the https proxy rather than http://HOST:PORT: a plain-http page
// on another host is not a secure context, and has no SharedArrayBuffer)
//
// Every boot is ?persist=reset, so it leaves nothing in the page's c4dos
// store.

import { writeFileSync, mkdirSync } from 'node:fs';
import { Browser } from './cdp.mjs';

const [cdp = 'http://127.0.0.1:9222', base = 'http://127.0.0.1:8472', outdir = '/tmp/web-grin-g6', secs = '15', what = ''] = process.argv.slice(2);
const SECS = +secs;
const want = s => !what || what.split(',').includes(s);
mkdirSync(outdir, { recursive: true });
let fail = 0;
const ok = (what, cond, extra = '') => {
  console.log(`web-grin-g6: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-800) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

const b = await Browser.connect(cdp);
console.log(`web-grin-g6: ${b.version}`);
const tab = await b.newTab(false);
const fps = {};
try {
  const shown = 'window.c4m.display.shownText';
  const shownHas = (s, t = 20000) => tab.waitFor(`${shown}.some(t => t.s.trimEnd() === ${JSON.stringify(s)})`, t);
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
  const pick = async (group, s) => {
    await clickText('Program Manager');
    await clickT((await find(group)).sort((a, c) => a.x - c.x)[0]);
    const all = await find(s);
    return all.filter(t => t.x > 140)[0] || all[all.length - 1];
  };
  const item = async (group, s) => clickT(await pick(group, s), true);
  const KEYS = { ArrowUp: 38, ArrowDown: 40, ArrowLeft: 37, ArrowRight: 39, Enter: 13, Escape: 27, Control: 17, F10: 121, y: 89, Y: 89, ' ': 32 };
  const hold = async (key, ms = 120, mods = 0) => {
    const vk = KEYS[key] || key.toUpperCase().charCodeAt(0);
    const text = key === 'Enter' ? '\r' : key.length === 1 ? key : undefined;
    const code = key === 'Control' ? 'ControlLeft' : key === ' ' ? 'Space' : key.length === 1 ? `Key${key.toUpperCase()}` : key;
    await tab.send('Input.dispatchKeyEvent', { type: text ? 'keyDown' : 'rawKeyDown', key, code, windowsVirtualKeyCode: vk, text, modifiers: mods });
    await sleep(ms);
    await tab.send('Input.dispatchKeyEvent', { type: 'keyUp', key, code, windowsVirtualKeyCode: vk, modifiers: mods });
    await sleep(150);
  };
  // the Task List (Ctrl+Esc): a window brought to the front, or ended
  const task = async (title, button) => {
    await hold('Escape', 80, 2);
    if (!await shownHas('End Task', 5000)) return false;
    const rows = (await find(title)).sort((a, c) => a.y - c.y);
    await clickT(rows[rows.length - 1]);
    await clickText(button);
    await sleep(800);
    return true;
  };
  const shot = async name => { writeFileSync(`${outdir}/${name}.png`, await tab.screenshot()); console.log(`web-grin-g6: screenshot ${outdir}/${name}.png`); };
  // the 8-bit frames the page has drawn (GUI_BLIT8, which the machine's
  // display device hands the page as RGB, command 102): a game's frames,
  // in a window or full screen alike
  const count = () => tab.eval('window.c4m.display.rgbFrames | 0');
  const instrument = () => tab.eval(`(() => { const d = window.c4m.display; if (d.rgbWrapped) return true;
    const draw = d.draw.bind(d); d.rgbFrames = 0; d.rgbWrapped = true;
    d.draw = words => { for (let i = 0; i < words.length && words[i] > 0; i += words[i]) if (words[i + 1] === 102) d.rgbFrames++; return draw(words); };
    return true; })()`);
  const measure = async what => {
    const n0 = await count(), t0 = Date.now();
    await sleep(SECS * 1000);
    const n = await count() - n0, s = (Date.now() - t0) / 1000;
    const r = Math.round(n / s * 10) / 10;
    console.log(`web-grin-g6: fps ${what}: ${n} frames in ${s.toFixed(1)} s = ${r}`);
    fps[what] = r;
    return r;
  };
  const boot = async () => {
    await tab.goto(`${base}/libjs/web/?system=c4dos&persist=reset`);
    const up = await tab.waitFor('window.c4m && document.getElementById("terminal").innerText.includes("Starting grin")', 90000);
    await tab.eval('window.c4m.display.recordText = true');
    await instrument();
    await sleep(500);
    await clickAt(780, 590);               // the display gets the keyboard
    return up && await shownHas('Program Manager');
  };

  let n0 = 0;
  if (want('doom')) {
  ok('?persist=reset: the page boots into grin', await boot());
  // 800x600, where a game's window is 2x
  await item('Main', 'Control Panel');
  await shownHas('Display');
  await clickText('Display', true);
  await shownHas('800 x 600');
  await clickText('800 x 600'); await clickText('OK');
  ok('Control Panel > Display: 800x600', await tab.waitFor("document.getElementById('display').width === 800", 10000));
  await task('Control Panel', 'End Task');

  // ---- Doom in a window, beside a Clock and Notepad -----------------------------------
  await item('Accessories', 'Clock');
  ok('Accessories > Clock', await shownHas('Settings'));
  await item('Main', 'Notepad');
  ok('Main > Notepad', await shownHas('Untitled - Notepad'));
  n0 = await count();
  await item('Games', 'Doom');
  ok('Games > Doom: it plays in a window of grin\'s, Program Manager still drawn',
    await tab.waitFor(`(window.c4m.display.rgbFrames | 0) > ${n0} + 20`, 90000) && await shownHas('Size') && await shownHas('Program Manager'));
  await sleep(3000);
  await shot('grin-g6-1-doom-window');
  await measure('Doom, window 2x');
  // typing in Notepad while the demo plays
  ok('the Task List brings Notepad to the front', await task('Untitled - Notepad', 'Switch To'));
  await tab.type('Typed in Notepad while Doom plays.');
  n0 = await count();
  ok('typing in Notepad while Doom\'s demo plays: Notepad shows it', await shownHas('Typed in Notepad while Doom plays.'));
  await sleep(2000);
  ok('... and Doom drew on meanwhile', await count() > n0 + 5);
  await shot('grin-g6-2-typing-in-notepad');
  // the Size menu: 1x, and back to 2x
  await task('Doom', 'Switch To');
  await clickText('Size');
  ok('Doom\'s Size menu', await shownHas('1x  320 x 200'));
  await clickText('1x  320 x 200');
  await sleep(2500);
  await shot('grin-g6-3-doom-1x');
  await measure('Doom, window 1x');
  await clickText('Size');
  await shownHas('2x  640 x 400');
  await clickText('2x  640 x 400');
  await sleep(1000);
  // the Task List's End Task closes it, and Doom ends
  ok('Ctrl+Esc over Doom: the Task List, End Task', await task('Doom', 'End Task'));
  n0 = await count();
  await sleep(2500);
  ok('End Task: Doom\'s window goes, and no more frames come', !(await shownHas('Size', 3000)) && await count() === n0);
  await shot('grin-g6-4-after-doom');
  ok('grin carries on: the Clock and Notepad are still there', await shownHas('Typed in Notepad while Doom plays.'));
  }

  // ---- frames per second, each game, in a window and full screen -----------------------
  // Doom's demo plays by itself; Wolfenstein 3D goes into a new game
  // (floor 1, standing); Tyrian and Rise of the Triad are left to their
  // own openings and demos. Each from a fresh boot.
  const games = [
    { name: 'Doom', start: async () => sleep(4000) },
    { name: 'Wolfenstein 3D', start: async () => {
      await sleep(8000);
      for (let i = 0; i < 3; i++) { await hold('Enter'); await sleep(4000); }
      await hold('Escape'); await sleep(2500);
      await hold('n'); await sleep(1500);
      for (let i = 0; i < 4; i++) { await hold('ArrowDown'); await sleep(600); }
      for (let i = 0; i < 3; i++) { await hold('Enter'); await sleep(2500); }
      await sleep(4000);
    } },
    { name: 'Tyrian', start: async () => sleep(20000) },
    { name: 'Rise of the Triad', start: async () => sleep(20000) },
  ];
  for (const mode of ['window', 'full screen']) {
    for (const g of games) {
      if (!want(g.name)) continue;
      ok(`fresh boot for ${g.name}, ${mode}`, await boot());
      if (mode === 'full screen') {
        await clickT(await pick('Games', g.name));
        await clickText('File'); await shownHas('Properties...'); await clickText('Properties...');
        await shownHas('Program Item Properties');
        await clickText('DOS program, full screen');
        await hold('Enter');
        await sleep(800);
      }
      n0 = await count();
      await item('Games', g.name);
      // (one is enough: Wolfenstein 3D's sign-on draws one and waits for a key)
      if (!ok(`${g.name}, ${mode}: its frames arrive`, await tab.waitFor(`(window.c4m.display.rgbFrames | 0) > ${n0}`, 120000))) continue;
      await g.start();
      const tag = g.name.toLowerCase().replace(/[^a-z0-9]+/g, '-');
      await shot(`grin-g6-fps-${tag}-${mode === 'window' ? 'window' : 'full'}`);
      await measure(`${g.name}, ${mode}`);
    }
  }
  ok('no page errors', tab.errors.length === 0, tab.errors.join('\n  '));
  ok('?persist=reset at the end: nothing left in the store', await boot());
} finally {
  await tab.close();
  b.disconnect();
}
console.log('web-grin-g6: fps ' + JSON.stringify(fps));
console.log(fail ? 'web-grin-g6: FAILED' : 'web-grin-g6: OK');
process.exit(fail);
