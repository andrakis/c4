// web-ix-fonts.mjs - Text Editor's Word Wrap and the fonts, in a browser
// (docs/c4ix-desktop.md, part twenty-two): at each of the desktop's sizes,
// Settings with the interface in another face and size, Text Editor with
// a long paragraph folded (and once not), and the IDE in another of its
// fonts, with a screenshot of each. Over CDP, in a tab of its own in one
// of the user's Chromes (cdp.mjs), against a page served from this tree:
//
//   node libjs/serve.mjs PORT --isolate &
//   node libjs/tests/web-ix-fonts.mjs http://127.0.0.1:9222 https://PORT.code.home.stargazer.onl OUTDIR
//
// It boots with ?persist=reset, and boots so again at the end, leaving nothing kept.

import { writeFileSync, mkdirSync } from 'node:fs';
import { Browser } from './cdp.mjs';

const [cdp = 'http://127.0.0.1:9222', base = 'http://127.0.0.1:8472', outdir = '/tmp/web-ix-fonts'] = process.argv.slice(2);
mkdirSync(outdir, { recursive: true });
let fail = 0;
const ok = (what, cond, extra = '') => {
  console.log(`web-ix-fonts: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-800) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

// what each size shows: the interface's face and size, Text Editor's, the IDE's
const SIZES = [
  { w: 640, h: 480, ui: ['Tahoma', 'Small'], note: ['Consolas', 'Small'], ide: ['Consolas', 'Small'] },
  { w: 800, h: 600, ui: ['Verdana', 'Normal'], note: ['Courier New', 'Normal'], ide: ['Courier New', 'Normal'] },
  { w: 1024, h: 768, ui: ['Georgia', 'Large'], note: ['Lucida Console', 'Large'], ide: ['Lucida Console', 'Large'] },
];
const PARA = 'Word Wrap folds a line wider than the window onto the rows below, after the last space that fits; ' +
  'without it the line runs on past the edge, and the text scrolls across under a bar. The fonts come in three faces ' +
  'and three sizes, for the desktop, the IDE and Text Editor each.';

const b = await Browser.connect(cdp);
console.log(`web-ix-fonts: ${b.version}`);
const tab = await b.newTab(false);
try {
  const shown = 'window.c4m.display.shownText';
  const shownHas = (s, t = 20000) => tab.waitFor(`${shown}.some(t => ${s instanceof RegExp ? `${s}.test(t.s)` : `t.s.trimEnd() === ${JSON.stringify(s)}`})`, t);
  const find = async s => tab.eval(`${shown}.filter(t => ${s instanceof RegExp ? `${s}.test(t.s)` : `t.s.trimEnd() === ${JSON.stringify(s)}`})`);
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
  const clickT = async (t, dx = 5) => { if (!t) return false; await clickAt(t.x + dx, t.y + 5); await sleep(400); return true; };
  const clickText = async (s, dx = 5) => { const all = await find(s); return clickT(all[all.length - 1], dx); };
  const KEYS = { Enter: 13, Escape: 27, Home: 36, End: 35 };
  const hold = async (key, mods = 0) => {
    const vk = KEYS[key] || key.toUpperCase().charCodeAt(0);
    const text = mods & 2 ? undefined : key === 'Enter' ? '\r' : key.length === 1 ? key : undefined;
    const code = key.length === 1 ? `Key${key.toUpperCase()}` : key;
    await tab.send('Input.dispatchKeyEvent', { type: text ? 'keyDown' : 'rawKeyDown', key, code, windowsVirtualKeyCode: vk, text, modifiers: mods });
    await sleep(60);
    await tab.send('Input.dispatchKeyEvent', { type: 'keyUp', key, code, windowsVirtualKeyCode: vk, modifiers: mods });
    await sleep(150);
  };
  const shot = async name => { await sleep(800); writeFileSync(`${outdir}/${name}.png`, await tab.screenshot()); console.log(`web-ix-fonts: screenshot ${outdir}/${name}.png`); };
  const go = async item => { const g = await find('Go'); await clickT(g[g.length - 1]); if (!await shownHas('Power Off...', 5000)) return false; return clickText(item); };
  const H = () => tab.eval("document.getElementById('display').height");
  // a window raised by a click on its taskbar button (its title may be under another)
  const raise = async re => { const h = await H(); return clickT((await find(re)).filter(t => t.y >= h - 30).pop()); };
  // a menu of the topmost window, and an item of it
  const menu = async (title, item) => { await clickText(title); await shownHas(item, 3000); return clickText(item); };
  // the paragraph's rows on the screen: strings that are a piece of it
  const paraRows = () => tab.eval(`${shown}.filter(t => t.s.trim().length > 12 && ${JSON.stringify(PARA)}.includes(t.s.trim())).length`);

  await tab.goto(`${base}/libjs/web/?persist=reset&mmu=1`);
  const up = await tab.waitFor('window.c4m && window.c4m.display', 90000);
  await tab.eval('window.c4m.display.recordText = true');
  ok('?persist=reset: the page boots the C4IX desktop', up && await shownHas(/^Terminal - task/, 120000));
  await sleep(1500);

  let first = true;
  for (const { w, h, ui, note, ide } of SIZES) {
    // ---- Settings: the size, then the interface's font ----
    await go('Settings');
    ok(`${w}x${h}: Settings has Font and Font size`, await shownHas('Font size', 5000));
    await clickText(`${w}x${h}`);
    await tab.waitFor(`document.getElementById('display').width === ${w}`, 10000);
    await sleep(1000);
    await clickText(ui[0], -8);
    await clickText(ui[1], -8);
    ok(`${w}x${h}: the sample in ${ui.join(', ')}`, await shownHas('The quick brown fox', 5000));
    await shot(`fonts-1-settings-${w}x${h}`);
    await hold('Escape');

    // ---- Text Editor: a paragraph, folded ----
    if (first) {
      await go('Text Editor');
      ok('Go > Text Editor', await shownHas('Untitled - Text Editor'));
      await tab.type(PARA + '\n\n' + PARA);
    } else await raise(/ - Text Editor$/);
    await menu('Format', note[0]);
    await menu('Format', note[1]);
    await sleep(800);
    ok(`${w}x${h}: Text Editor, folded, in ${note.join(', ')}`, await shownHas(/^Word Wrap folds a line /, 5000) && await paraRows() >= 6, await paraRows());
    await shot(`fonts-2-note-${w}x${h}`);
    if (w === 800) {
      await menu('Format', 'Word Wrap');
      await hold('Home');
      await sleep(800);
      ok('Word Wrap off: a row a line', await paraRows() === 2, await paraRows());
      await shot(`fonts-3-nowrap-${w}x${h}`);
      await menu('Format', 'Word Wrap');
    }

    // ---- the IDE, in its font ----
    if (first) {
      await go('IDE');
      ok('Go > IDE', await shownHas('Untitled - IDE'));
      await hold('Escape');
      await tab.type('e /usr/src/c4ix/examples/hello.c\n');
      ok('hello.c in the IDE', await shownHas('hello.c - IDE', 10000));
    } else await raise(/ - IDE$/);
    await menu('View', 'Settings...');
    await shownHas('IDE Settings', 5000);
    await clickText(ide[0], -8);
    await clickText(ide[1], -8);
    await clickText('OK');
    await sleep(500);
    ok(`${w}x${h}: the IDE in ${ide.join(', ')}`, !(await find('IDE Settings')).length);
    await shot(`fonts-4-ide-${w}x${h}`);
    first = false;
  }
  // the Code tab in Lucida Console, Large
  await hold('Escape');
  await tab.type('code\n');
  ok(':code: the Code tab in the IDE\'s font', await shownHas(/^main \(/, 60000));
  await shot('fonts-5-code-1024x768');

  ok('no page errors', tab.errors.length === 0, tab.errors.join('\n  '));
  await tab.goto(`${base}/libjs/web/?persist=reset&mmu=1`);
  ok('?persist=reset at the end: nothing left in the store', await tab.waitFor('window.c4m && window.c4m.display', 90000));
} finally {
  await tab.close();
  b.disconnect();
}
console.log(fail ? 'web-ix-fonts: FAILED' : 'web-ix-fonts: OK');
process.exit(fail);
