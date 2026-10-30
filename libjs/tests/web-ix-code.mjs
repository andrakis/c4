// web-ix-code.mjs - the IDE's Code pane in a browser (docs/c4ix-desktop.md,
// part eighteen): a C file written in the IDE, View > Code, and the pane
// at each of the desktop's sizes (beside the text at 800x600 and
// 1024x768, below it at 640x480), with a screenshot of each. Over CDP, in
// a tab of its own in one of the user's Chromes (cdp.mjs), against a page
// served from this tree:
//
//   node libjs/serve.mjs PORT --isolate &
//   node libjs/tests/web-ix-code.mjs http://127.0.0.1:9222 https://PORT.code.home.stargazer.onl OUTDIR
//
// It boots with ?persist=reset, and leaves the desktop at 800x600.

import { writeFileSync, mkdirSync } from 'node:fs';
import { Browser } from './cdp.mjs';

const [cdp = 'http://127.0.0.1:9222', base = 'http://127.0.0.1:8472', outdir = '/tmp/web-ix-code'] = process.argv.slice(2);
mkdirSync(outdir, { recursive: true });
let fail = 0;
const ok = (what, cond, extra = '') => {
  console.log(`web-ix-code: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-800) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

const b = await Browser.connect(cdp);
console.log(`web-ix-code: ${b.version}`);
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
  const clickT = async t => { if (!t) return false; await clickAt(t.x + 5, t.y + 5); await sleep(400); return true; };
  const clickText = async s => { const all = await find(s); return clickT(all[all.length - 1]); };
  const KEYS = { Enter: 13, Escape: 27, Home: 36, End: 35 };
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
  const shot = async name => { writeFileSync(`${outdir}/${name}.png`, await tab.screenshot()); console.log(`web-ix-code: screenshot ${outdir}/${name}.png`); };
  const go = async item => { const g = await find('Go'); await clickT(g[g.length - 1]); if (!await shownHas('Power Off...', 5000)) return false; return clickText(item); };

  await tab.goto(`${base}/libjs/web/?persist=reset&mmu=1`);
  const up = await tab.waitFor('window.c4m && window.c4m.display', 90000);
  await tab.eval('window.c4m.display.recordText = true');
  ok('?persist=reset: the page boots the C4IX desktop', up && await shownHas(/^Terminal - task/, 120000));
  await sleep(1500);

  // ---- a C file in the IDE, and its code ----
  await go('IDE');
  ok('Go > IDE', await shownHas('Untitled - IDE'));
  await hold('Escape');
  await tab.type('e /tmp/sq.c\n');
  await sleep(500);
  await tab.type('#include "c4ix_user.h"\n\nint sq(int a) {\nreturn a * a;\n}\n\nint main(int argc, char **argv) {\nint i;\ni = 0;\nwhile (i < argc) {\nuprintf("%d: %s\\n", sq(i), argv[i]);\n++i;\n}\nreturn 0;\n}\n');
  await hold('s', 2);
  ok('the file, written', await shownHas(/"\/tmp\/sq\.c" written/, 5000));
  await hold('Escape');
  await tab.type('code\n');
  ok(':code compiles it and shows main', await shownHas(/^Code: main \(\d+ words\)/, 60000));
  await sleep(800);
  await shot('ix-code-1-800x600');

  // ---- each size ----
  for (const [w, h] of [[640, 480], [1024, 768], [800, 600]]) {
    await go('Settings');
    await shownHas('Screen size', 5000);
    await clickText(`${w}x${h}`);
    await tab.waitFor(`document.getElementById('display').width === ${w}`, 10000);
    await sleep(1000);
    await hold('Escape');
    // the IDE's title raises it
    const H = await tab.eval("document.getElementById('display').height");
    await clickT((await find('sq.c - IDE')).filter(t => t.y < H - 30).pop());
    await sleep(1200);
    ok(`${w}x${h}: the pane is there`, await shownHas(/^Code: main/, 5000));
    await shot(`ix-code-2-${w}x${h}`);
  }
  ok('no page errors', tab.errors.length === 0, tab.errors.join('\n  '));
  await tab.goto(`${base}/libjs/web/?persist=reset&mmu=1`);
  ok('?persist=reset at the end: nothing left in the store', await tab.waitFor('window.c4m && window.c4m.display', 90000));
} finally {
  await tab.close();
  b.disconnect();
}
console.log(fail ? 'web-ix-code: FAILED' : 'web-ix-code: OK');
process.exit(fail);
