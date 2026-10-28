// web-grin-g7.mjs - grin's G7 browser check (docs/grin.md): the startup
// chime on the page's sound card, Program Manager's group windows tiled,
// cascaded, minimised to an icon and maximised, the clipboard from Notepad
// to Calculator after Notepad has closed and from a DOS Prompt to Notepad,
// the screen saver's Test, and Control Panel's Sound. Over CDP, in a tab
// of its own in one of the user's Chromes (cdp.mjs), against a page served
// from this tree:
//
//   node libjs/serve.mjs PORT --isolate &
//   node libjs/tests/web-grin-g7.mjs http://127.0.0.1:9222 https://PORT.code.home.stargazer.onl OUTDIR
//
// (through the https proxy rather than http://HOST:PORT: a plain-http page
// on another host is not a secure context, and has no SharedArrayBuffer)
//
// Every boot is ?persist=reset, so it leaves nothing in the page's c4dos
// store.

import { writeFileSync, mkdirSync } from 'node:fs';
import { Browser } from './cdp.mjs';

const [cdp = 'http://127.0.0.1:9222', base = 'http://127.0.0.1:8472', outdir = '/tmp/web-grin-g7'] = process.argv.slice(2);
mkdirSync(outdir, { recursive: true });
let fail = 0;
const ok = (what, cond, extra = '') => {
  console.log(`web-grin-g7: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-800) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

const b = await Browser.connect(cdp);
console.log(`web-grin-g7: ${b.version}`);
const tab = await b.newTab(false);
try {
  // the sound card's key-ons, counted as the page is handed them (page.js's
  // SoundOut.play), from as early as the page has a machine: the chime
  // comes with grin's first frame
  await tab.send('Page.addScriptToEvaluateOnNewDocument', { source: `
    window.__keyOns = 0;
    const hook = setInterval(() => {
      const s = window.c4m && window.c4m.sound;
      if (!s || s.__hooked) return;
      const play = s.play.bind(s);
      s.play = (events, now) => {
        for (const e of events || []) if (e[0] === 1 && e[2] >= 0xb0 && e[2] <= 0xb8 && (e[3] & 0x20)) window.__keyOns++;
        return play(events, now);
      };
      s.__hooked = true;
      clearInterval(hook);
    }, 5);` });
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
  // a menu by its title, the last drawn (the window in front), and an item in it
  const menu = async (title, item) => { await clickText(title); if (!await shownHas(item, 5000)) return false; return clickText(item); };
  // Program Manager to the front by its title; a group by its Window
  // menu; an item in it
  const item = async (group, s) => {
    await clickText('Program Manager');
    const n = { Main: 1, Accessories: 2, Games: 3 }[group];
    await menu('Window', `${n} ${group}`);
    const all = await find(s);
    return clickT(all[all.length - 1], true);
  };
  const KEYS = { Enter: 13, Escape: 27, Insert: 45, Home: 36 };
  // a key, with CDP's modifiers (2 Ctrl, 8 Shift)
  const hold = async (key, mods = 0) => {
    const vk = KEYS[key] || key.toUpperCase().charCodeAt(0);
    const text = mods & 2 ? undefined : key === 'Enter' ? '\r' : key.length === 1 ? key : undefined;
    const code = key.length === 1 ? `Key${key.toUpperCase()}` : key;
    await tab.send('Input.dispatchKeyEvent', { type: text ? 'keyDown' : 'rawKeyDown', key, code, windowsVirtualKeyCode: vk, text, modifiers: mods });
    await sleep(60);
    await tab.send('Input.dispatchKeyEvent', { type: 'keyUp', key, code, windowsVirtualKeyCode: vk, modifiers: mods });
    await sleep(150);
  };
  const task = async (title, button) => {
    await hold('Escape', 2);
    if (!await shownHas('End Task', 5000)) return false;
    const rows = (await find(title)).sort((a, c) => a.y - c.y);
    await clickT(rows[rows.length - 1]);
    await clickText(button);
    await sleep(800);
    return true;
  };
  const shot = async name => { writeFileSync(`${outdir}/${name}.png`, await tab.screenshot()); console.log(`web-grin-g7: screenshot ${outdir}/${name}.png`); };

  await tab.goto(`${base}/libjs/web/?system=c4dos&persist=reset`);
  const up = await tab.waitFor('window.c4m && document.getElementById("terminal").innerText.includes("Starting grin")', 90000);
  await tab.eval('window.c4m.display.recordText = true');
  await sleep(1500);
  await clickAt(630, 470);                // the display gets the keyboard
  ok('?persist=reset: the page boots into grin', up && await shownHas('Program Manager'));
  ok('the startup chime: four notes on the page\'s sound card', await tab.waitFor('window.__keyOns >= 4', 10000), await tab.eval('window.__keyOns'));
  await shot('grin-g7-1-boot');

  // ---- the groups ----
  ok('Program Manager: Main open, Accessories and Games icons', await shownHas('Main') && await shownHas('Accessories') && await shownHas('Games'));
  await clickText('Program Manager');
  await menu('Window', '2 Accessories');
  await menu('Window', '3 Games');
  ok('Window > 2 Accessories and 3 Games restore them', await shownHas('Clock') && await shownHas('Doom'));
  await menu('Window', 'Tile');
  await sleep(500);
  await shot('grin-g7-2-tiled');
  await menu('Window', 'Cascade');
  await sleep(500);
  await shot('grin-g7-3-cascaded');
  // Main maximised by a double click on its title
  await clickText('Main', true);
  ok('a double click on Main\'s title maximises it: Program Manager - [Main]', await shownHas('Program Manager - [Main]', 5000));
  await shot('grin-g7-4-main-maximised');
  await clickText('Main', true);
  ok('... and again restores it', !(await shownHas('Program Manager - [Main]', 1500)));

  // ---- the clipboard ----
  await item('Main', 'Notepad');
  ok('Main > Notepad', await shownHas('Untitled - Notepad'));
  await tab.type('12*12=');
  await hold('Home', 8);                  // Shift+Home: the line selected
  await sleep(300);
  await shot('grin-g7-5-notepad-selection');
  await hold('c', 2);                     // Ctrl+C
  await task('Untitled * - Notepad', 'End Task');
  ok('Notepad copied 12*12= and was ended', !(await shownHas('Untitled * - Notepad', 1500)));
  await item('Accessories', 'Calculator');
  ok('Accessories > Calculator', await shownHas('Hex'));
  await hold('v', 2);                     // Ctrl+V
  ok('Ctrl+V in Calculator, after Notepad has gone: 144', await shownHas('144', 5000));
  await shot('grin-g7-6-calculator-pasted');
  await task('Calculator', 'End Task');
  await item('Main', 'DOS Prompt');
  ok('Main > DOS Prompt', await shownHas('DOS Prompt 1'));
  await tab.type('ECHO from the DOS Prompt\n');
  ok('... ECHO in it', await shownHas('from the DOS Prompt'));
  await menu('Edit', 'Copy');
  await item('Main', 'Notepad');
  await shownHas('Untitled - Notepad');
  await hold('Insert', 8);                // Shift+Ins
  ok("Shift+Ins in Notepad: the DOS Prompt's screen as text", await shownHas('A:\\>ECHO from the DOS Prompt', 5000));
  await shot('grin-g7-7-notepad-screen-pasted');
  await task('Untitled * - Notepad', 'End Task');
  if (await shownHas('No', 1500)) await clickText('No');

  // ---- the screen saver, and Sound ----
  await item('Main', 'Control Panel');
  await shownHas('Desktop');
  await clickText('Desktop', true);
  await shownHas('Marquee');
  await clickText('Marquee');
  await clickText('Test');
  ok("Desktop's Test: Marquee, saying grin's line", await shownHas('grin -- C4DOS in the manner of Windows 3.1', 8000));
  await sleep(1500);
  await shot('grin-g7-8-marquee');
  await hold('Home');
  ok('... a key ends it', await shownHas('Screen Saver', 5000));
  await clickText('Cancel');
  await clickText('Sound', true);
  ok('Control Panel > Sound', await shownHas('A chime when grin starts'));
  await shot('grin-g7-9-sound');
  const k0 = await tab.eval('window.__keyOns');
  await clickText('Test');
  ok("... its Test plays the chime", await tab.waitFor(`window.__keyOns >= ${k0} + 4`, 5000));
  await clickText('Cancel');
  ok('no page errors', tab.errors.length === 0, tab.errors.join('\n  '));
  await tab.goto(`${base}/libjs/web/?system=c4dos&persist=reset`);
  ok('?persist=reset at the end: nothing left in the store', await tab.waitFor('window.c4m && document.getElementById("terminal").innerText.includes("Starting grin")', 90000));
} finally {
  await tab.close();
  b.disconnect();
}
console.log(fail ? 'web-grin-g7: FAILED' : 'web-grin-g7: OK');
process.exit(fail);
