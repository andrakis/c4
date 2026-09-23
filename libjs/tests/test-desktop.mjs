// test-desktop.mjs - the C4IX desktop, headless (docs/c4ix-desktop.md).
//
// Boots C4IX with a display fitted, starts `desktop` from the console,
// and drives it the way a person would: clicks and keys on the display.
// The screen is read back from the text commands of the last frame
// presented, each string tagged with the window it falls inside.
//
// Pinned: a terminal opens with a working shell; ps typed in it lists
// the desktop and that shell; a second terminal from the Start menu; spin
// in one and Ctrl-C cancels it without touching the other; windows drag;
// close ends the shell; Shut Down returns to the console.

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
  console.log(`test-desktop: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + extra : ''}`);
  if (!cond) fail = 1;
};

const names = JSON.parse(readFileSync(join(ROOT, 'disk/manifest.json'), 'utf8'));
const files = new Map(names.map(n => [n, new Uint8Array(readFileSync(join(ROOT, 'disk', n)))]));
const gui = new GuiDevice({ w: 640, h: 480 });
const out = [];
const vm = createMachine({ arenaMb: 64, gui, drives: [{ files, writable: false, sink: null }], onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const consoleText = () => Buffer.from(out).toString('latin1');

// The screen: every TEXT2 of the last presented frame, and the RECTs that
// were drawn as window frames, so text can be placed in a window.
let frame = [], cur = [], presents = 0;
const pump = () => {
  const words = gui.drain();
  for (let i = 0; words && i < words.length; i += words[i]) {
    const t = words[i + 1];
    if (t === CMD.CLEAR) cur = [];
    if (t === CMD.TEXT2) {
      const n = words[i + 8];
      let s = '';
      for (let k = 0; k < n; k++) s += String.fromCharCode((words[i + 9 + (k >> 2)] >>> ((k & 3) * 8)) & 0xff);
      cur.push({ x: words[i + 2], y: words[i + 3], s });
    }
    if (t === CMD.PRESENT) { frame = cur; presents++; }
  }
};
// run() returns early whenever the guest sleeps, so waiting is until()'s job
const run = (cycles = 2e6) => { const r = vm.run(cycles); pump(); return r; };
const until = (cond, budget = 400) => { for (let i = 0; i < budget; i++) { if (cond()) return true; if (run() === STOP.HALT) return cond(); } return cond(); };
// Text inside a rectangle, row by row
const textIn = (x0, y0, x1, y1) => frame.filter(t => t.x >= x0 && t.x < x1 && t.y >= y0 && t.y < y1)
  .sort((a, b) => a.y - b.y || a.x - b.x).map(t => t.s).join('\n');
const screen = () => textIn(0, 0, SW, SH);
const ev = e => { gui.event(e); run(3e5); };
const click = (x, y) => { ev({ kind: 'move', x, y, buttons: 0 }); ev({ kind: 'down', x, y, button: 0 }); ev({ kind: 'up', x, y, button: 0 }); };
const key = (ch, mods = 0) => {
  const code = ch === '\n' ? 13 : ch === '\b' ? 8 : ch.toUpperCase().charCodeAt(0);
  ev({ kind: 'keydown', code, char: ch === '\n' ? 10 : ch === '\b' ? 0 : ch.charCodeAt(0), mods });
};
const type = s => { for (const ch of s) key(ch); };

// The window rectangles, found from the title strings (the first window
// opens at 100,30 and each next one 28,24 further; a terminal is 652x421).
const TW = 80 * 8 + 12, THH = 24 * 16 + 4 + 4 * 2 + 18 + 1;
const winRect = i => ({ x: 100 + i * 28, y: 30 + i * 24 });

// ---- boot C4IX and start the desktop from its console --------------------
check(until(() => consoleText().includes('c4ix:/$'), 2000), 'C4IX boots to its shell');
typeBytes(vm, new TextEncoder().encode('desktop\n'));
check(until(() => consoleText().includes('desktop: running on the display')), 'desktop starts from the console');
check(until(() => gui.w === SW && gui.h === SH && presents > 0), `it asks for a ${SW}x${SH} display and draws`);
check(until(() => /c4ix:\/\$/.test(screen()), 800), 'a terminal window opens with a shell prompt in it', screen());
check(screen().includes('Start') && /[0-9]{1,2}:[0-9]{2} [AP]M/.test(screen()), 'the taskbar has Start and a clock');

// ---- a command in the first terminal ---------------------------------------
type('ps\n');
check(until(() => /c4ix-desktop/.test(screen()) && /tasks,/.test(screen())), 'ps typed in the window runs in its shell', screen().slice(-600));
const w0 = winRect(0);
check(/tasks,/.test(textIn(w0.x, w0.y, w0.x + TW, w0.y + THH)), 'and its output lands in that window');

// ---- a second terminal from the Start menu ----------------------------------
click(20, SH - TASK_H + 12);
check(until(() => screen().includes('Shut Down...')), 'Start opens the menu', screen());
const menuTop = SH - TASK_H - (3 * 26 + 12);
click(80, menuTop + 6 + 12);
const w1 = winRect(1);
check(until(() => (textIn(w1.x, w1.y + 30, w1.x + TW, w1.y + THH).match(/c4ix:\/\$/g) || []).length >= 1, 800) &&
      (screen().match(/Terminal \(task/g) || []).length >= 4, 'New Terminal opens a second window with its own shell', screen());

// ---- spin in the second, Ctrl-C there, the first unaffected -----------------
type('spin 50\n');
check(until(() => /spin: tick 2\//.test(screen()), 1500), 'spin runs in the second terminal');
key('c', 2);
check(until(() => /\^C/.test(screen()) && !/spin: tick 50\/50/.test(screen())), 'Ctrl-C there shows ^C');
const before = (screen().match(/spin: tick \d+/g) || []).pop();
for (let i = 0; i < 40; i++) run();
const after = (screen().match(/spin: tick \d+/g) || []).pop();
check(before === after, `and spin has stopped (${after})`);
// back to the first window: click its title bar, then type there
click(w0.x + 200, w0.y + 10);
type('echo still-here\n');
check(until(() => /still-here/.test(textIn(w0.x, w0.y, w0.x + 400, w0.y + THH))), 'the first terminal still answers after the second was interrupted');

// ---- drag a window ----------------------------------------------------------
ev({ kind: 'move', x: w0.x + 200, y: w0.y + 10, buttons: 0 });
ev({ kind: 'down', x: w0.x + 200, y: w0.y + 10, button: 0 });
ev({ kind: 'move', x: w0.x + 400, y: w0.y + 110, buttons: 1 });
ev({ kind: 'up', x: w0.x + 400, y: w0.y + 110, button: 0 });
check(until(() => frame.some(t => t.s.startsWith('Terminal (task') && t.x > w0.x + 200 && t.y > w0.y + 100)), 'dragging the title bar moves the window');

// ---- maximise, restore, minimise, restore from the taskbar -------------------
// window 0 now sits at (300,130); its caption buttons are 16 wide, from the
// right: close at x+630, maximise at x+612, minimise at x+596
const titleOf = pred => frame.filter(t => t.s.startsWith('Terminal (task') && t.y < SH - TASK_H && pred(t));
const mx = w0.x + 200, my = w0.y + 100;
click(mx + 612 + 6, my + 6 + 6);
check(until(() => titleOf(t => t.x < 40 && t.y < 12).length === 1), 'maximise fills the desktop');
click(4 + 612 + 6 + (SW - 652), 4 + 2 + 6);           // the button, now at the right edge of the screen
check(until(() => titleOf(t => t.x > mx && t.y > my).length === 1), 'and the same button restores it');
click(mx + 596 + 6, my + 6 + 6);
check(until(() => titleOf(t => t.x > mx && t.y > my).length === 0), 'minimise hides it');
click(64 + 20, SH - TASK_H + 12);                       // its taskbar button, the first
check(until(() => titleOf(t => t.x > mx && t.y > my).length === 1), 'and its taskbar button brings it back');

// ---- close one, then shut down -----------------------------------------------
click(w1.x + TW - 4 - 2 - 8, w1.y + 4 + 2 + 6);           // the second window's close button
until(() => (screen().match(/Terminal \(task/g) || []).length === 2, 400);
check((screen().match(/Terminal \(task/g) || []).length === 2, 'closing a window removes it and its taskbar button', screen().slice(0, 300));
click(20, SH - TASK_H + 12);
click(80, menuTop + 6 + 2 * 26 + 12);                      // Shut Down...
check(until(() => consoleText().includes('desktop: shut down')), 'Shut Down ends the desktop');
typeBytes(vm, new TextEncoder().encode('ps\n'));
check(until(() => /tasks,/.test(consoleText().split('desktop: shut down').pop())), 'and the console shell carries on');
const psOut = consoleText().split('desktop: shut down').pop();
check(!/c4ix-sh.c4r[\s\S]*c4ix-sh.c4r/.test(psOut.replace(/^[\s\S]*?ID/, '')), 'no terminal shell outlived the desktop', psOut);
process.exit(fail);
