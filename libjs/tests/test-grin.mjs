// test-grin.mjs - grin, C4DOS's graphical shell, headless
// (docs/grin-plan.md, G2).
//
// Boots C4DOS on the page's own disk (libjs/images/dos, build-images.sh)
// with a display fitted, as ?system=c4dos does. Its AUTOEXEC.BAT ends
// with GRIN, so grin comes up with nothing typed. The test then drives it
// as a person would, with clicks and keys on the display, finding things
// by the text drawn on them (as test-desktop.mjs does): the Task List,
// minimising to an icon and back, Control Panel's Display at 800x600 and
// back, and File > Exit grin, which returns to A>.
//
//   node libjs/tests/test-grin.mjs

import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { createMachine, boot } from '../boot.js';
import { STOP } from '../c4m.js';
import { GuiDevice, CMD } from '../gui-device.js';

const DISK = new URL('../images/dos/', import.meta.url).pathname;
let fail = 0;
const check = (cond, what, extra = '') => {
  console.log(`test-grin: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-1500) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
if (!existsSync(join(DISK, 'index.json')) || !existsSync(join(DISK, 'grin.c4r'))) {
  console.log('test-grin: SKIPPED, no libjs/images/dos (make c4bb-images builds it)');
  process.exit(0);
}

// the disk as the page serves it: a file read the first time the machine opens it
const index = JSON.parse(readFileSync(join(DISK, 'index.json'), 'utf8'));
class LazyDisk extends Map {
  constructor(idx) { super(); this.sizes = new Map(idx); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) {
    if (super.has(n)) return super.get(n);
    if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(DISK, n)));
    super.set(n, b);
    return b;
  }
}
const image = new Uint8Array(readFileSync(join(DISK, 'c4dos32.c4r')));

// ---- without a display: grin says so, and DOS carries on ------------------------
{
  const out = [];
  const vm = createMachine({ arenaMb: 64, mhz: 400, drives: [{ files: new LazyDisk(index), writable: false, sink: null }], onByte: b => out.push(b) });
  boot(vm, image, ['c4dos32.c4r']);
  const text = () => Buffer.from(out).toString('latin1');
  for (let i = 0; i < 2000 && !/grin: this machine has no display[^]*A>/.test(text()); i++) if (vm.run(4e6) === STOP.HALT) break;
  check(/grin: this machine has no display[^]*A>/.test(text()), 'with no display, grin says so and DOS comes back to A>', text());
}

// ---- with one ----------------------------------------------------------------------
const files = new LazyDisk(index);
const gui = new GuiDevice({ w: 640, h: 480 });
const out = [];
const vm = createMachine({ arenaMb: 64, mhz: 400, gui, drives: [{ files, writable: false, sink: null }], onByte: b => out.push(b) });
boot(vm, image, ['c4dos32.c4r']);
const text = () => Buffer.from(out).toString('latin1');

// The screen: every string and rectangle of the last frame presented. A
// frame that does not begin with CLEAR paints over part of the one
// before, as the display does (test-desktop.mjs has the same reader).
let frame = [], cur = [], presents = 0, rects = [], curRects = [];
let gen = 0, clip = null, partial = false;
const imgSize = new Map();
const resized = [];
const within = (r, x, y) => x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
const cut = (a, b) => {
  const x = Math.max(a.x, b.x), y = Math.max(a.y, b.y);
  return { x, y, w: Math.min(a.x + a.w, b.x + b.w) - x, h: Math.min(a.y + a.h, b.y + b.h) - y };
};
const pump = () => {
  const words = gui.drain();
  for (let i = 0; words && i < words.length; i += words[i]) {
    const t = words[i + 1];
    if (t === CMD.CLEAR) { cur = []; curRects = []; partial = false; }
    if (t === CMD.IMGDEF) imgSize.set(words[i + 2], { w: words[i + 3], h: words[i + 4] });
    if (t === CMD.CLIP) clip = { x: words[i + 2], y: words[i + 3], w: words[i + 4], h: words[i + 5] };
    if (t === CMD.NOCLIP) clip = null;
    const z = t === CMD.IMG ? imgSize.get(words[i + 2]) : null;
    if (t === CMD.RECT || z) {
      const r = z ? { x: words[i + 3], y: words[i + 4], w: z.w, h: z.h, rgb: -1, gen }
                  : { x: words[i + 2], y: words[i + 3], w: words[i + 4], h: words[i + 5], rgb: words[i + 6], gen };
      const v = clip ? cut(r, clip) : r;
      if (partial && v.w > 0 && v.h > 0) {
        cur = cur.filter(s => s.gen === gen || !within(v, s.x, s.y));
        curRects = curRects.filter(q => q.gen === gen || !(within(v, q.x, q.y) && within(v, q.x + q.w - 1, q.y + q.h - 1)));
      }
      curRects.push(r);
    }
    if (t === CMD.TEXT2) {
      const n = words[i + 8];
      let s = '';
      for (let k = 0; k < n; k++) s += String.fromCharCode((words[i + 9 + (k >> 2)] >>> ((k & 3) * 8)) & 0xff);
      if (!partial || !clip || within(clip, words[i + 2], words[i + 3]))
        cur.push({ x: words[i + 2], y: words[i + 3], s, rgb: words[i + 4], gen });
    }
    if (t === CMD.PRESENT) { frame = cur.slice(); rects = curRects.slice(); presents++; gen++; partial = true; }
    // a new size asked for (gui_size): the page's display takes it and
    // says so with a resize event (display.js), which is what sets GUI_W
    if (t === CMD.SIZE) resized.push({ kind: 'resize', w: words[i + 2], h: words[i + 3] });
  }
  while (resized.length) gui.event(resized.shift());
};
let halted = false;
const run = (cycles = 2e6) => { const r = vm.run(cycles); pump(); if (r === STOP.HALT) halted = true; return r; };
const until = (cond, budget = 1500) => { for (let i = 0; i < budget && !halted; i++) { if (cond()) return true; run(); } return cond(); };
const settle = () => { for (let i = 0; i < 20; i++) run(); };
const has = s => frame.some(t => (typeof s === 'string' ? t.s.trimEnd() === s : s.test(t.s)));
const find = s => frame.filter(t => (typeof s === 'string' ? t.s.trimEnd() === s : s.test(t.s)));
const lastIndex = s => { let k = -1; frame.forEach((t, i) => { if (t.s.trimEnd() === s) k = i; }); return k; };
const ev = e => { gui.event(e); run(3e5); };
const click = (x, y, button = 0) => { ev({ kind: 'move', x, y, buttons: 0 }); ev({ kind: 'down', x, y, button }); ev({ kind: 'up', x, y, button }); };
const at = (s, pick = 'last') => { const all = find(s); return pick === 'last' ? all[all.length - 1] : pick(all); };
const clickText = (s, pick) => { const t = at(s, pick); if (!t) return false; click(t.x + 5, t.y + 5); settle(); return true; };
const dbl = (s, pick) => { const t = at(s, pick); if (!t) return false; click(t.x + 5, t.y + 5); click(t.x + 5, t.y + 5); settle(); return true; };
const key = (code, ch = 0, mods = 0) => {
  ev({ kind: 'keydown', code, char: ch, mods }); ev({ kind: 'keyup', code, char: ch, mods }); settle();
};
// A window, found by the first menu on its bar ("File" for Program
// Manager, "Settings" for Control Panel): Windows 3.1's chrome puts it 14
// px in from the window's left and 27 down, and the frame's first
// rectangle is the whole window.
const BORDER = 4, BOX = 18;
const win = menu => {
  const t = at(menu);
  if (!t) return null;
  const x = t.x - 14, y = t.y - 27;
  const r = rects.find(r => r.x === x && r.y === y && r.w > 100);
  return r ? { x, y, w: r.w, h: r.h } : null;
};
const minButton = w => [w.x + w.w - BORDER - 2 * BOX + 9, w.y + BORDER + 9];
const maxButton = w => [w.x + w.w - BORDER - BOX + 9, w.y + BORDER + 9];
const prompts = () => (text().match(/A>/g) || []).length;

// ---- boot to grin, with nothing typed ---------------------------------------------
check(until(() => has('Program Manager') && has('Main'), 3000), 'C4DOS boots into grin: Program Manager is drawn', text());
check(text().includes('heap device installed'), 'the disk\'s CONFIG.SYS has HEAP.SYS', text());
check(gui.w === 640 && gui.h === 480, `grin asks for 640x480 (${gui.w}x${gui.h})`);
check(has('Control Panel') && has('File') && has('Help'), 'Program Manager has its File menu and the Main group\'s Control Panel');
const pm0 = win('File');
check(pm0 && find('Program Manager').some(t => Math.abs(t.x + 7 * 15 / 2 - (pm0.x + pm0.w / 2)) < 30),
  'the Windows 3.1 look: the title is centred', JSON.stringify(pm0));

// a double click on the desktop opens the Task List; Esc closes it
click(600, 400); click(600, 400); settle();
check(until(() => has('Task List') && has('Switch To') && has('Arrange Icons')), 'a double click on the desktop opens the Task List');
key(27); until(() => !has('Switch To'));
check(!has('Switch To'), 'Esc closes it');

// ---- Control Panel, minimised to an icon and restored ------------------------------
dbl('Control Panel');
check(until(() => has('Settings') && has('Display') && has('Colors') && has('Desktop')),
  'a double click on Control Panel opens it: Colors, Desktop, Display');
let cp = win('Settings');
check(!!cp, 'Control Panel\'s window is found', JSON.stringify(cp));
if (cp) click(...minButton(cp));
settle();
const label = () => find('Control').filter(t => t.y > 480 - 64);
check(until(() => !has('Settings') && label().length > 0), 'its minimise button makes it an icon along the bottom, labelled',
  JSON.stringify(find(/Control|Panel/)));
const ic = label()[0];
if (ic) { click(ic.x + 5, ic.y + 5); click(ic.x + 5, ic.y + 5); settle(); }
check(until(() => has('Settings') && !label().length), 'a double click on the icon restores the window');
// a single click opens the icon's control menu, and Restore there brings it back too
cp = win('Settings');
if (cp) click(...minButton(cp));
settle();
until(() => label().length > 0);
const ic2 = label()[0];
if (ic2) click(ic2.x + 5, ic2.y + 5);
settle();
check(until(() => has('Restore') && has('Minimize')), 'a click on the icon opens its control menu');
clickText('Restore');
check(until(() => has('Settings') && !label().length), '... and its Restore brings the window back');

// ---- Program Manager maximised; the Task List's Switch To --------------------------
let pm = win('File');
if (pm) click(...maxButton(pm));
settle();
pm = win('File');
check(pm && pm.x === 0 && pm.y === 0 && pm.w === 640 && pm.h === 480, 'Program Manager maximised fills 640x480', JSON.stringify(pm));
check(lastIndex('File') > lastIndex('Settings'), 'and it is in front of Control Panel');
// minimised, and back by its icon's Restore: maximised again, as it was
click(...minButton(pm));
settle();
const pmIcon = () => find('Program').filter(t => t.y > 480 - 64);
check(until(() => !has('File') && pmIcon().length > 0), 'Program Manager minimised: its icon\'s label is on two lines',
  JSON.stringify(find(/Program|Manager/)));
if (pmIcon()[0]) click(pmIcon()[0].x + 5, pmIcon()[0].y + 5);
settle();
clickText('Restore');
pm = win('File');
check(until(() => (pm = win('File')) && pm.w === 640 && pm.h === 480), '... and Restore brings it back maximised', JSON.stringify(pm));
key(27, 0, 2);                          // Ctrl+Esc
check(until(() => has('Switch To')), 'Ctrl+Esc opens the Task List');
// (a row of the Task List's list: above its buttons, inside it)
const inList = t => { const tl = at('Switch To'); return tl && t.y < tl.y && t.y > tl.y - 180 && t.x > tl.x - 40 && t.x < tl.x + 280; };
const list = find('Control Panel').filter(t => { const tl = at('Switch To'); return tl && t.y < tl.y && t.y > tl.y - 180 && t.x > tl.x - 40 && t.x < tl.x + 280; });
check(list.length === 1 && find('Program Manager').length >= 2, 'it lists Program Manager and Control Panel',
  JSON.stringify(frame.map(t => [t.s, t.x, t.y])));
if (list[0]) click(list[0].x + 5, list[0].y + 5);
settle();
clickText('Switch To');
check(until(() => !has('Switch To') && lastIndex('Settings') > lastIndex('File')), 'Switch To brings Control Panel to the front');

// ---- Control Panel > Display: 800x600 and back -------------------------------------
const display = (choice, w, h) => {
  dbl('Display');
  if (!check(until(() => has('800 x 600') && has('640 x 480') && has('OK')), `Display offers 640 x 480 and 800 x 600 (for ${choice})`)) return;
  clickText(choice);
  clickText('OK');
  check(until(() => gui.w === w && gui.h === h && !has('640 x 480')), `${choice}: GUI_W and GUI_H read ${gui.w}x${gui.h}`);
  settle();
  const p = win('File');
  check(p && p.x === 0 && p.y === 0 && p.w === w && p.h === h, `... and the maximised Program Manager fills ${w}x${h}`, JSON.stringify(p));
  check(has('Settings'), '... and Control Panel is still there');
};
display('800 x 600', 800, 600);
display('640 x 480', 640, 480);

// ---- Colors: another scheme, drawn at once -------------------------------------------
dbl('Colors');
if (check(until(() => has('Granite') && has('OK')), 'Colors lists the schemes')) {
  clickText('Granite'); clickText('OK');
  check(until(() => !has('Granite') && rects.some(r => r.rgb === 0x384c60)), 'Granite\'s title colour is drawn after OK');
}

// ---- Desktop: a pattern under the windows ---------------------------------------------
dbl('Desktop');
if (check(until(() => has('Boxes') && has('Screen Saver') && has('OK')), 'Desktop offers the patterns and the screen savers')) {
  clickText('Boxes'); clickText('OK');
  check(until(() => !has('Screen Saver') && rects.filter(r => r.rgb === -1 && r.w === 64 && r.h === 64).length >= 50),
    'Boxes is tiled over the desktop after OK');
}

// ---- File > Exit grin, back to A> ---------------------------------------------------------
const exitGrin = () => {
  // Program Manager's File menu: in front first (the Task List again)
  key(27, 0, 2);
  until(() => has('Switch To'));
  const row = find('Program Manager').filter(t => { const tl = at('Switch To'); return tl && t.y < tl.y && t.y > tl.y - 180 && t.x > tl.x - 40 && t.x < tl.x + 280; })[0];
  if (row) click(row.x + 5, row.y + 5);
  clickText('Switch To');
  clickText('File');
  until(() => has('Exit grin...'));
  clickText('Exit grin...');
  return until(() => has('This will end your grin session.') && has('Cancel'));
};
check(exitGrin(), 'File > Exit grin asks: This will end your grin session.');
clickText('Cancel');
check(until(() => !has('This will end your grin session.')) && has('Program Manager'), 'Cancel keeps the session');
let p0 = prompts();
exitGrin();
clickText('OK');
check(until(() => prompts() > p0 && !gui.stats().attached, 3000), 'OK ends it: the display is let go and DOS is at A>', text().slice(-300));
const typed = (line, budget = 1500) => { const p = prompts(), a = text().length; vm.dev.pushInput(line + '\n'); until(() => prompts() > p, budget); return text().slice(a); };
check(/C4DOS version \d+\.\d+/.test(typed('VER')), 'VER answers at A>');
const iniText = typed('TYPE GRIN.INI');
check(/width=640/.test(iniText) && /height=480/.test(iniText) && /scheme=1/.test(iniText) && /pattern=2/.test(iniText), 'GRIN.INI on the RAM disk keeps the choices', iniText);

// ---- GRIN again --------------------------------------------------------------------------
const f0 = presents;
vm.dev.pushInput('GRIN\n');
check(until(() => presents > f0 && has('Program Manager'), 3000) && gui.w === 640, 'GRIN at A> starts it again');
check(rects.some(r => r.rgb === 0x384c60), 'with the scheme GRIN.INI keeps');
p0 = prompts();
// Program Manager's control menu: Close asks the same
pm = win('File');
if (pm) click(pm.x + BORDER + 9, pm.y + BORDER + 9);
settle();
clickText('Close');
until(() => has('This will end your grin session.'));
clickText('OK');
check(until(() => prompts() > p0 && !gui.stats().attached, 3000), 'Close on Program Manager\'s control menu ends the session too');

console.log(fail ? 'test-grin: FAILED' : 'test-grin: OK');
process.exit(fail);
