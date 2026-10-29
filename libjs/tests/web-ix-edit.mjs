// web-ix-edit.mjs - the C4IX desktop's clipboard, IDE and file box, in a
// browser (docs/c4ix-desktop.md, part fifteen): text selected in a
// Terminal with the mouse and copied from its Edit menu, pasted into the
// IDE (Traditional typing, the Blue scheme), the IDE's Settings, its
// Windows 95 Open box, the warning before a binary file, and Text
// Editor's Save As. Over CDP, in a tab of its own in one of the user's
// Chromes (cdp.mjs), against a page served from this tree:
//
//   node libjs/serve.mjs PORT --isolate &
//   node libjs/tests/web-ix-edit.mjs http://127.0.0.1:9222 https://PORT.code.home.stargazer.onl OUTDIR
//
// Every boot is ?persist=reset, so it leaves nothing in the page's c4ix
// store.

import { writeFileSync, mkdirSync } from 'node:fs';
import { Browser } from './cdp.mjs';

const [cdp = 'http://127.0.0.1:9222', base = 'http://127.0.0.1:8472', outdir = '/tmp/web-ix-edit'] = process.argv.slice(2);
mkdirSync(outdir, { recursive: true });
let fail = 0;
const ok = (what, cond, extra = '') => {
  console.log(`web-ix-edit: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-800) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
const sleep = ms => new Promise(r => setTimeout(r, ms));

const b = await Browser.connect(cdp);
console.log(`web-ix-edit: ${b.version}`);
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
  const dragAt = async (x0, y0, x1, y1) => {
    const p = await at(x0, y0), q = await at(x1, y1);
    await tab.mouse('mouseMoved', p.x, p.y); await tab.mouse('mousePressed', p.x, p.y, 'left', 1);
    for (let k = 1; k <= 5; k++) { await tab.mouse('mouseMoved', p.x + ((q.x - p.x) * k) / 5, p.y + ((q.y - p.y) * k) / 5, 'left', 1); await sleep(40); }
    await tab.mouse('mouseReleased', q.x, q.y, 'left', 0);
    await sleep(300);
  };
  const clickT = async (t, dbl = false) => { if (!t) return false; await clickAt(t.x + 5, t.y + 5); if (dbl) await clickAt(t.x + 5, t.y + 5); await sleep(400); return true; };
  const clickText = async (s, dbl = false) => { const all = await find(s); return clickT(all[all.length - 1], dbl); };
  const menu = async (title, item) => { await clickText(title); if (!await shownHas(item, 5000)) return false; return clickText(item); };
  const KEYS = { Enter: 13, Escape: 27, Insert: 45, Home: 36, End: 35 };
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
  const shot = async name => { writeFileSync(`${outdir}/${name}.png`, await tab.screenshot()); console.log(`web-ix-edit: screenshot ${outdir}/${name}.png`); };
  // the Go menu, and an item on it
  const go = async item => { const g = await find('Go'); await clickT(g[g.length - 1]); if (!await shownHas('Power Off...', 5000)) return false; return clickText(item); };

  await tab.goto(`${base}/libjs/web/?persist=reset`);
  const up = await tab.waitFor('window.c4m && window.c4m.display', 90000);
  await tab.eval('window.c4m.display.recordText = true');
  ok('?persist=reset: the page boots the C4IX desktop, a Terminal open', up && await shownHas(/^Terminal - task/, 120000));
  await sleep(1500);

  // ---- the Terminal: a selection, Edit > Copy ----
  await clickText(/^Terminal - task/);
  await tab.type('echo copied from a Terminal\n');
  ok('a line in the Terminal', await shownHas('copied from a Terminal'));
  {
    const t = (await find('copied from a Terminal')).filter(t => t.adv === 8).pop();
    await dragAt(t.x + 2, t.y + 6, t.x + 22 * 8 - 3, t.y + 6);
    ok('a drag selects it', await tab.waitFor(`${shown}.some(t => t.s === 'copied from a Terminal' && t.rgb === 0xffffff)`, 5000));
    await shot('ix-edit-1-terminal-selection');
    await menu('Edit', 'Copy   Ctrl+Shift+C');
  }

  // ---- the IDE: Traditional, Blue, the paste ----
  await go('IDE');
  ok('Go > IDE: Untitled - IDE, typing as Text Editor does', await shownHas('Untitled - IDE') && await shownHas(/^-- EDIT --/));
  await hold('v', 2);
  ok("Ctrl+V pastes the Terminal's text", await shownHas('copied from a Terminal', 5000));
  await hold('Enter');
  await tab.type('int main() { return 0; }');
  await hold('Home', 8);
  await sleep(500);
  await shot('ix-edit-2-ide-blue-selection');
  await menu('View', 'Settings...');
  ok('View > Settings: the scheme and the input mode', await shownHas('IDE Settings') && await shownHas('Input mode'));
  await shot('ix-edit-3-ide-settings');
  await clickText('OK');

  // ---- a binary file asks ----
  await hold('Escape');
  await tab.type('e! /bin/hello\n');
  ok('opening /bin/hello asks first', await shownHas(/does not look like text/, 5000));
  await shot('ix-edit-4-binary-warning');
  await clickText('Cancel');

  // ---- the Windows 95 Open box ----
  await menu('File', 'Open...');
  ok("File > Open: Windows 95's box, Look in and Files of type", await shownHas('Look in:') && await shownHas('Files of type:'));
  await clickText('All Files (*)');
  await shownHas('C Source (*.c;*.h)', 5000);
  await shot('ix-edit-5-open-types');
  await hold('Escape');
  await hold('Escape');

  // ---- Text Editor's Save As ----
  await go('Text Editor');
  await shownHas('Untitled - Text Editor');
  await hold('v', 2);
  await hold('s', 2);
  ok("Text Editor's Save As is the same box", await shownHas('Save in:') && await shownHas('Save as type:'));
  await shot('ix-edit-6-text-editor-save-as');
  await hold('Escape');
  ok('no page errors', tab.errors.length === 0, tab.errors.join('\n  '));
  await tab.goto(`${base}/libjs/web/?persist=reset`);
  ok('?persist=reset at the end: nothing left in the store', await tab.waitFor('window.c4m && window.c4m.display', 90000));
} finally {
  await tab.close();
  b.disconnect();
}
console.log(fail ? 'web-ix-edit: FAILED' : 'web-ix-edit: OK');
process.exit(fail);
