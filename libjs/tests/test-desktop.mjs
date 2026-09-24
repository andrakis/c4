// test-desktop.mjs - the C4IX desktop and its tools, headless
// (docs/c4ix-desktop.md).
//
// Boots C4IX with a display fitted, starts `desktop` from the console,
// and drives it as a person would, with clicks and keys on the display.
// The screen is read back from the text commands of the last frame
// presented, and things are clicked by the text drawn on them, so the
// test follows the layout rather than pinning coordinates.

import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { createMachine, boot } from '../boot.js';
import { STOP } from '../c4m.js';
import { GuiDevice, CMD } from '../gui-device.js';
import { typeBytes } from '../keys.js';

const ROOT = new URL('../../src/c4bb/images/', import.meta.url).pathname;
const SW = 800, SH = 600, TASK_H = 30;
let fail = 0;
const check = (cond, what, extra = '') => {
  console.log(`test-desktop: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(0, 1500) : ''}`);
  if (!cond) fail = 1;
  return cond;
};

const names = JSON.parse(readFileSync(join(ROOT, 'disk/manifest.json'), 'utf8'));
const files = new Map(names.map(n => [n, new Uint8Array(readFileSync(join(ROOT, 'disk', n)))]));
const gui = new GuiDevice({ w: 640, h: 480 });
const out = [];
const vm = createMachine({ arenaMb: 64, gui, drives: [{ files, writable: false, sink: null }], onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const consoleText = () => Buffer.from(out).toString('latin1');

// The screen: every string of the last presented frame, with its position.
let frame = [], cur = [], presents = 0, rects = [], curRects = [];
const pump = () => {
  const words = gui.drain();
  for (let i = 0; words && i < words.length; i += words[i]) {
    const t = words[i + 1];
    if (t === CMD.CLEAR) { cur = []; curRects = []; }
    if (t === CMD.RECT) curRects.push({ x: words[i + 2], y: words[i + 3], w: words[i + 4], h: words[i + 5], rgb: words[i + 6] });
    if (t === CMD.TEXT2) {
      const n = words[i + 8];
      let s = '';
      for (let k = 0; k < n; k++) s += String.fromCharCode((words[i + 9 + (k >> 2)] >>> ((k & 3) * 8)) & 0xff);
      cur.push({ x: words[i + 2], y: words[i + 3], s });
    }
    if (t === CMD.PRESENT) { frame = cur; rects = curRects; presents++; }
  }
};
// run() returns early whenever the guest sleeps, so waiting is until()'s job
const run = (cycles = 2e6) => { const r = vm.run(cycles); pump(); return r; };
const until = (cond, budget = 400) => { for (let i = 0; i < budget; i++) { if (cond()) return true; if (run() === STOP.HALT) return cond(); } return cond(); };
const settle = () => { for (let i = 0; i < 20; i++) run(); return true; };
const screen = () => frame.map(t => t.s).join('\n');
const has = re => typeof re === 'string' ? frame.some(t => t.s.trimEnd() === re) : frame.some(t => re.test(t.s));
const find = s => frame.filter(t => (typeof s === 'string' ? t.s.trimEnd() === s : s.test(t.s)));
const ev = e => { gui.event(e); run(3e5); };
const click = (x, y, button = 0) => { ev({ kind: 'move', x, y, buttons: 0 }); ev({ kind: 'down', x, y, button }); ev({ kind: 'up', x, y, button }); };
// Click on a string on the screen. `pick` chooses among several (default:
// the last drawn, which is the topmost).
const clickText = (s, pick = 'last', dx = 5, dy = 5) => {
  const all = find(s);
  if (!all.length) return false;
  const t = pick === 'last' ? all[all.length - 1] : pick === 'first' ? all[0] : pick(all);
  if (!t) return false;
  click(t.x + dx, t.y + dy);
  return true;
};
const dbl = (s, pick = 'last') => {
  const all = find(s);
  const t = pick === 'last' ? all[all.length - 1] : pick(all);
  if (!t) return false;
  click(t.x + 5, t.y + 5); click(t.x + 5, t.y + 5);
  return true;
};
const key = (ch, mods = 0, code) => {
  const named = { '\n': 13, '\b': 8, ESC: 27, DEL: 46, F2: 113, F5: 116, TAB: 9, UP: 38, DOWN: 40 };
  const c = code ?? named[ch] ?? ch.toUpperCase().charCodeAt(0);
  ev({ kind: 'keydown', code: c, char: ch === '\n' ? 10 : ch.length === 1 && c !== 8 ? ch.charCodeAt(0) : 0, mods });
};
const type = s => { for (const ch of s) key(ch); };
const start = item => { click(20, SH - TASK_H + 12); until(() => has('Shut Down...')); const ok = clickText(item); settle(); return ok; };
// Explorer's file list: right of its folder tree, inside its window. The
// window is found by its title (drawn 27 px in from the window's left, 7 down).
const explorer = () => {
  const t = frame.find(t => /^Exploring - /.test(t.s) && t.y < SH - TASK_H);
  return t ? { x: t.x - 27, y: t.y - 7 } : null;
};
const inList = all => {
  const e = explorer();
  if (!e) return undefined;
  return all.filter(t => t.x > e.x + 200 && t.x < e.x + 600 && t.y > e.y + 60 && t.y < e.y + 420).pop();
};
const noDialog = () => !has('OK') && !has('Yes');

// ---- boot C4IX, start the desktop ----------------------------------------------
check(until(() => consoleText().includes('c4ix:/$'), 2000), 'C4IX boots to its shell');
typeBytes(vm, new TextEncoder().encode('desktop\n'));
check(until(() => consoleText().includes('desktop: running on the display')), 'desktop starts from the console');
check(until(() => gui.w === SW && gui.h === SH && presents > 0), `it asks for ${SW}x${SH} and draws`);
check(until(() => has(/c4ix:\/\$/), 800), 'a Command Prompt opens with a shell prompt', screen());
check(has('Start') && has(/[0-9]{1,2}:[0-9]{2} [AP]M/), 'the taskbar has Start and a clock');
check(['My Computer', 'Command Prompt', 'Notepad', 'Calculator', 'Task Manager'].every(has), 'the desktop icons are there');

// ---- Command Prompt ----------------------------------------------------------------
type('ps\n');
check(until(() => has(/c4ix-desktop/) && has(/tasks,/)), 'ps typed in the Command Prompt runs in its shell', screen());

// ---- top runs until Ctrl-C ----------------------------------------------------------------
type('top\n');
check(until(() => has(/Ctrl-C to stop/), 800), 'top starts in the Command Prompt', screen());
const t0 = vm.dev.simMs();
until(() => vm.dev.simMs() - t0 > 5000, 3000);
check(has(/Ctrl-C to stop/) && !frame.some(t => /^c4ix:\/\$/.test(t.s) && t.y > 300), 'and is still running five machine-seconds later', screen());
key('c', 2, 67);
check(until(() => has(/\^C/) && frame.filter(t => /^c4ix:\/\$/.test(t.s)).length >= 1), 'Ctrl-C stops it and the prompt comes back', screen());

// ---- raycast in the Command Prompt: 256 colours ---------------------------------------------
type('raycast.c4r 15x15 -s 3 -d -n 30 -g 80x25\n');
check(until(() => new Set(rects.map(r => r.rgb)).size > 8, 1500), `raycast draws in colour in the Command Prompt (${new Set(rects.map(r => r.rgb)).size} colours)`);
until(() => has(/^c4ix:\/\$/), 1500);
key('l', 2, 76);                                               // Ctrl-L: a clean screen for what follows

// ---- Explorer ------------------------------------------------------------------------
start('Explorer');
check(until(() => has('Exploring - /')), 'Start > Explorer opens at the root', screen());
check(['bin', 'ram', 'usr'].every(has) && has('File Folder') && has('C4IX (/)'), 'it lists the root folders and shows the tree', screen());
dbl('usr', inList);
check(until(() => has('Exploring - /usr') && has('src')), 'double-clicking a folder opens it', screen());
key('\b');
check(until(() => has('Exploring - /')), 'Backspace goes up a level');
dbl('ram', inList);
check(until(() => has('Exploring - /ram')), 'into /ram');
clickText('File'); settle(); clickText('New Folder');
check(until(() => has('Name of the new folder:')), 'File > New Folder asks for a name', screen());
key('\n');
check(until(() => noDialog() && !!inList(find('New Folder'))), 'and makes it', screen());
clickText('New Folder', inList);
key('F2');
check(until(() => has('New name:')), 'F2 asks for a new name');
for (let i = 0; i < 12; i++) key('\b');
type('docs'); key('\n');
check(until(() => noDialog() && !!inList(find('docs')) && !inList(find('New Folder'))), 'and renames it', screen());
clickText('docs', inList);
key('DEL');
check(until(() => has(/Are you sure you want to delete 'docs'/)), 'Delete asks first');
key('y');
check(until(() => noDialog() && !inList(find('docs'))), 'and Yes deletes it');

// ---- Notepad --------------------------------------------------------------------------
start('Notepad');
check(until(() => has('Untitled - Notepad')), 'Start > Notepad opens a new document');
type('hello from c4ix\n'); type('second line');
check(until(() => has('hello from c4ix') && has('second line') && has('Untitled * - Notepad')), 'typing shows, and the title marks it modified', screen());
key('s', 2, 83);
check(until(() => has('Save as:')), 'Ctrl+S on a new file asks where');
key('\n');
check(until(() => has('untitled.txt - Notepad')), 'and saves it to /ram/untitled.txt', screen());
clickText(/^Exploring/); key('F5');
check(until(() => !!inList(find('untitled.txt')) && has('Text Document')), 'Explorer lists the saved file after a refresh', screen());
const notes = find('untitled.txt - Notepad').length;
dbl('untitled.txt', inList);
check(until(() => find('untitled.txt - Notepad').length > notes), 'double-clicking it opens it in Notepad');
check(until(() => find('hello from c4ix').length >= 2), 'with what was saved in it');

// ---- Calculator ------------------------------------------------------------------------
start('Calculator');
check(until(() => has('Calculator') && has('Hex') && has('Dec')), 'Start > Calculator opens');
type('12+30=');
check(until(() => has('42')), '12 + 30 = 42', screen().slice(-300));
clickText('Hex', 'last', -8, 5);
check(until(() => has('2A')), 'switched to Hex it shows 2A');
type('*2=');
check(until(() => has('54')), '2A * 2 = 54 in hex');

// ---- Task Manager ------------------------------------------------------------------------
clickText(/^Command Prompt - task/);
type('spin 5000\n');
until(() => has(/spin: tick 1\//), 1500);
key('ESC', 3, 27);                                             // Ctrl+Shift+Esc
check(until(() => has('Task Manager') && has('Image Name')), 'Ctrl+Shift+Esc opens Task Manager on Processes', screen());
check(until(() => has(/^c4ix-desktop/) && has(/^c4ix-spin/) && has(/^CPU Usage: \d+%$/), 600), 'it lists the tasks, spin among them, with CPU usage', frame.filter(t => /c4ix|CPU|Image|boot|init|spin/.test(t.s)).map(t => t.s.trim()).join(' | '));
clickText(/^c4ix-spin/);
clickText('End Process');
check(until(() => has(/End process c4ix-spin/)), 'End Process asks first');
key('y');
check(until(() => !has(/^c4ix-spin/), 600), 'and ends it', screen());
clickText('Performance');
check(until(() => has('CPU Usage History') && has('Totals')), 'the Performance tab shows the gauge and the history');
clickText('Applications');
check(until(() => has('Status') && find('Running').length >= 3), 'the Applications tab lists the windows');

// ---- Run --------------------------------------------------------------------------------------
start('Run...');
check(until(() => has(/Type the name of a program/)), 'Start > Run asks for a program');
type('echo from-run'); key('\n');
check(until(() => has('from-run'), 800), 'and runs it in a new Command Prompt', screen());

// ---- Shut Down --------------------------------------------------------------------------------
start('Shut Down...');
check(until(() => consoleText().includes('desktop: shut down')), 'Shut Down ends the desktop');
typeBytes(vm, new TextEncoder().encode('ps\n'));
check(until(() => /tasks,/.test(consoleText().split('desktop: shut down').pop())), 'and the console shell carries on');
const psOut = consoleText().split('desktop: shut down').pop();
check(!/c4ix-sh.c4r[\s\S]*c4ix-sh.c4r/.test(psOut.replace(/^[\s\S]*?ID/, '')), 'no Command Prompt shell outlived the desktop', psOut);
process.exit(fail);
