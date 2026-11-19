// web-ix-edit.mjs - the C4IX desktop's clipboard, IDE and file box, in a
// browser (docs/c4ix-desktop.md, part fifteen): text selected in a
// Terminal with the mouse and copied from its Edit menu, pasted into the
// IDE (Traditional typing, the Blue scheme), the IDE's Settings, its
// Windows 95 Open box, the warning before a binary file, and Text
// Editor's Save As; then the IDE on its own source, edit.c: the wheel to
// the last line and back to the first, Find (Ctrl+F, F3, Shift+F3) and
// the file pane's splitter; and Text Editor's Find. Over CDP, in a tab of
// its own in one of the user's Chromes (cdp.mjs), against a page served
// from this tree:
//
//   node libjs/serve.mjs PORT --isolate &
//   node libjs/tests/web-ix-edit.mjs http://127.0.0.1:9222 https://PORT.code.home.stargazer.onl OUTDIR
//
// Every boot is ?persist=0: nothing is read from the page's c4ix store, or
// kept in it.

import { writeFileSync, mkdirSync, readFileSync } from 'node:fs';
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
  const KEYS = { Enter: 13, Escape: 27, Insert: 45, Home: 36, End: 35, F3: 114 };
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

  await tab.goto(`${base}/libjs/web/?persist=0&mmu=1`);
  const up = await tab.waitFor('window.c4m && window.c4m.display', 90000);
  await tab.eval('window.c4m.display.recordText = true');
  ok('?persist=0: the page boots the C4IX desktop, a Terminal open', up && await shownHas(/^Terminal - task/, 120000));
  await sleep(1500);

  // ---- the Terminal: a selection, Edit > Copy ----
  // its window's title, not its taskbar button (which would minimise it)
  const H = await tab.eval("document.getElementById('display').height");
  await clickT((await find(/^Terminal - task/)).filter(t => t.y < H - 30).pop());
  await tab.type('echo copied from a Terminal\n');
  ok('a line in the Terminal', await shownHas('copied from a Terminal'));
  {
    const t = (await find('copied from a Terminal')).pop();
    await dragAt(t.x + 2, t.y + 6, t.x + 22 * 8 - 3, t.y + 6);
    // the selection draws the text again over itself (the row's own string runs on in spaces)
    ok('a drag selects it', await tab.waitFor(`${shown}.filter(s => s.s.trimEnd() === 'copied from a Terminal' && s.x === ${t.x} && s.y === ${t.y}).length >= 2`, 5000));
    await shot('ix-edit-1-terminal-selection');
    await menu('Edit', 'Copy   Ctrl+Shift+C');
  }

  // ---- the IDE: Traditional, Blue, the paste ----
  await go('IDE');
  ok('Go > IDE: Untitled - IDE, typing as Text Editor does', await shownHas('Untitled - IDE') && await shownHas(/^Type to edit; Esc for a command/));
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
  // (End: the selection made above goes, which the first Esc would otherwise take)
  await hold('End');
  await hold('Escape');
  await tab.type('e! /usr/share/splash/c4ix.c4p.gz\n');
  ok('opening a binary file (the splash picture) asks first', await shownHas(/does not look like text/, 5000));
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
  // ---- the IDE on its own source: the wheel to both ends, Find, the splitter ----
  const src = readFileSync(new URL('../../src/c4ix/user/desktop/edit.c', import.meta.url), 'latin1').split('\n');
  const LINES = src.length - 1;
  const poll = async (f, t = 15000) => { const end = Date.now() + t; while (Date.now() < end) { if (await f()) return true; await sleep(250); } return !!await f(); };
  await go('IDE');
  await shownHas('Untitled - IDE');
  await hold('Escape');
  await tab.type('e /usr/src/c4ix/user/desktop/edit.c\n');
  ok('the IDE opens its own source, edit.c', await shownHas('edit.c - IDE', 30000));
  await sleep(500);
  // the window by its title, the highest of its strings (the taskbar's button is lower)
  const win = async () => { const t = (await find('edit.c - IDE')).sort((p, q) => p.y - q.y)[0]; return t ? { x: t.x - 27, y: t.y - 7 } : null; };
  const w = await win();
  // the line numbers in the gutter, right of a file pane FW wide
  const nums = async (fw = 180) => (await tab.eval(`${shown}.filter(t => /^ *\\d+$/.test(t.s) && t.x > ${w.x + fw + 4} && t.x < ${w.x + fw + 36} && t.y > ${w.y + 40} && t.y < ${w.y + 470}).map(t => +t.s.trim())`)) || [];
  // (the wheel as the display's own listener sends it, in its coordinates:
  // CDP's mouseWheel waits on the compositor, which this Chrome may not run
  // for a tab in the background)
  const wheel = async (dy, n = 1) => {
    for (let k = 0; k < n; k++) {
      await tab.eval(`window.c4m.display.send({ kind: 'wheel', x: ${w.x + 450}, y: ${w.y + 200}, dy: ${dy} })`);
      await sleep(80);
    }
  };
  // the wheel's last turns at each end (a few thousand turns over CDP would
  // take too long): the cursor put 80 lines from the end, or the top, first
  const goLine = async n => { await hold('Escape'); await tab.type(`${n}\n`); await sleep(500); };
  await goLine(LINES - 80);
  for (let i = 0; i < 20 && !(await nums()).includes(LINES); i++) await wheel(1, 3);
  ok(`the wheel down reaches the last line, ${LINES} (Word Wrap on)`, await poll(async () => (await nums()).includes(LINES)), (await nums()).join(' '));
  await wheel(1, 5);
  ok('and turned on, stays there', await poll(async () => (await nums()).includes(LINES)), (await nums()).join(' '));
  await shot('ix-edit-7-ide-wheel-to-the-end');
  await goLine(80);
  for (let i = 0; i < 20 && !(await nums()).includes(1); i++) await wheel(-1, 3);
  ok('and up, the first line', await poll(async () => (await nums()).includes(1)), (await nums()).join(' '));
  await goLine(1);
  // Find: e_findbar's places in the file; the status line is the cursor's
  // line and column, just past the match (Traditional)
  const word = 'e_findbar';
  const hits = []; src.forEach((l, i) => { let k = l.indexOf(word); while (k >= 0) { hits.push([i + 1, k]); k = l.indexOf(word, k + 1); } });
  const atHit = j => shownHas(`${hits[j][0]},${hits[j][1] + word.length + 1}`, 8000);
  await hold('f', 2);
  ok('Ctrl+F opens the find bar', await shownHas('Find:', 5000) && await shownHas('Match case', 5000));
  await tab.type(word);
  ok(`typing finds the first ${word}, line ${hits[0][0]}`, await atHit(0));
  await hold('F3');
  ok(`F3 the next, line ${hits[1][0]}`, await atHit(1));
  await shot('ix-edit-8-ide-find');
  await hold('F3', 8);
  ok('Shift+F3 the one before', await atHit(0));
  await hold('F3', 8);
  ok('and before the first, round to the last, saying so', await atHit(hits.length - 1) && await shownHas('Wrapped to the bottom', 5000));
  await shot('ix-edit-9-ide-find-wrapped');
  await hold('Escape');
  ok('Esc closes the bar', await poll(async () => !(await find('Find:')).length, 5000));
  // the file pane's splitter: dragged 80 pixels right, then back
  const sx = w.x + 4 + 182, sy = w.y + 250;
  await dragAt(sx, sy, sx + 80, sy);
  ok("dragging the file pane's splitter widens it, the text moved along", await poll(async () => (await nums(260)).length > 5 && !(await nums(180)).length), (await nums(260)).join(' '));
  await shot('ix-edit-10-ide-splitter');
  await dragAt(sx + 80, sy, sx, sy);
  ok('and dragged back', await poll(async () => (await nums(180)).length > 5));
  // ---- Text Editor's Find: the same bar ----
  await go('Text Editor');
  await shownHas('Untitled - Text Editor');
  await tab.type('alpha beta\ngamma beta\n');
  await hold('f', 2);
  ok("Text Editor's Ctrl+F opens the find bar", await shownHas('Find:', 5000) && await shownHas('Match case', 5000));
  await tab.type('beta');
  await hold('F3');
  // the match selected: drawn again, in white, over its row's text
  const selIn = async row => { const r = (await find(row)).pop(); return !!r && (await find('beta')).some(t => t.y === r.y); };
  ok('F3 finds the next beta, on the second line', await poll(async () => await selIn('gamma beta') && !await selIn('alpha beta'), 8000));
  await shot('ix-edit-11-text-editor-find');
  await hold('Escape');
  ok('no page errors', tab.errors.length === 0, tab.errors.join('\n  '));
  await tab.goto(`${base}/libjs/web/?persist=0&mmu=1`);
  ok('the page boots again at the end', await tab.waitFor('window.c4m && window.c4m.display', 90000));
} finally {
  await tab.close();
  b.disconnect();
}
console.log(fail ? 'web-ix-edit: FAILED' : 'web-ix-edit: OK');
process.exit(fail);
