// test-grin.mjs - grin, C4DOS's graphical shell, headless
// (docs/grin-plan.md, G2 to G6).
//
// Boots C4DOS on the page's own disk (libjs/images/dos, build-images.sh)
// with a display fitted, as ?system=c4dos does. Its AUTOEXEC.BAT ends
// with GRIN, so grin comes up with nothing typed. The test then drives it
// as a person would, with clicks and keys on the display, finding things
// by the text drawn on them (as test-desktop.mjs does): the Task List,
// minimising to an icon and back, Control Panel's Display at 800x600 and
// back, and File > Exit grin, which returns to A>. Then grin's
// applications: two Clocks ticking and closed one at a time, with the
// heap back where it was; Calculator's 12*12=; a DOS program refused by
// File > Run; and EXITKEY, whose exit() from a key callback unloads it.
// Then G4's: the machine has a drive B: that the test keeps as the page
// keeps its store (C4DOS's HOME=B:), and Program Manager's groups from
// PROGMAN.INI, each item launching what it should, an item made with
// File > New, changed with Properties and deleted; Notepad saving
// NOTE.TXT through its file box onto B:, File Manager listing it there
// with its size and copying it to the RAM disk; and a second boot with
// the files B: kept: grin at the resolution Control Panel chose, NOTE.TXT
// still there and opening in Notepad, and File Manager's Delete taking
// it off B: and out of the store.
// Then G5's: at A>, redirection (REDIR.SYS TAP on this disk): DIR into a
// file, a c4cc program's printf through the console tap, a bare.c
// program's line from < IN.TXT, and pipes; in grin, File > Run of a DOS
// program, two DOS Prompt windows each with its own output, a bare.c
// program reading a line in one while a Clock ticks and the other says
// busy, a c4cc program and a pipe in one, EXIT, File Manager running a
// .bat in a DOS Prompt, and Doom full screen from Program Manager, twice,
// quit with keys, grin drawn again and the heap where it was.
// Then G6's: the games in a window of grin's. Doom from Program Manager
// beside a Clock whose seconds go on and a Notepad that takes typing,
// frames arriving in the window's rectangle; closing its window ends it,
// with the heap where it was; Wolfenstein 3D (from File Manager), Tyrian
// and Rise of the Triad each drawing in a window and closed; and a dialog
// opened while Doom plays a timedemo, which still works after Doom has
// ended by itself.
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
if (!existsSync(join(DISK, 'index.json')) || !existsSync(join(DISK, 'GRIN/grin.c4r'))) {
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
  for (let i = 0; i < 2000 && !/grin: this machine has no display[^]*A:\\>/.test(text()); i++) if (vm.run(4e6) === STOP.HALT) break;
  check(/grin: this machine has no display[^]*A:\\>/.test(text()), 'with no display, grin says so and DOS comes back to A:\\>', text());
}

// ---- with one ----------------------------------------------------------------------
// Drive B: as the page keeps it (worker.js's persist drive, page.js's
// store): what the machine closes there lands in `store`, what it deletes
// leaves it, and the next boot is given what the store holds.
const store = new Map();
const homeDrive = () => ({
  files: new Map([...store].map(([n, b]) => [n, b.slice()])), writable: true,
  sink: (n, b) => store.set(n, b.slice()), remove: n => store.delete(n),
});
const files = new LazyDisk(index);
let gui = new GuiDevice({ w: 640, h: 480 });
let out = [];
let vm = createMachine({ arenaMb: 64, mhz: 400, gui, drives: [{ files, writable: false, sink: null }, homeDrive()], onByte: b => out.push(b) });
boot(vm, image, ['c4dos32.c4r']);
const text = () => Buffer.from(out).toString('latin1');

// The screen: every string and rectangle of the last frame presented. A
// frame that does not begin with CLEAR paints over part of the one
// before, as the display does (test-desktop.mjs has the same reader).
let frame = [], cur = [], presents = 0, rects = [], curRects = [];
// every 8-bit image the display was sent (GUI_BLIT8, which the host turns
// into 32-bit pixels): a game's frames
let blits = [], blitCount = 0;
const timesSeen = new Set();
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
      if (/^\d\d:\d\d:\d\d$/.test(s.trimEnd())) timesSeen.add(s.trimEnd());
    }
    if (t === CMD.RGB) {
      blitCount++;
      blits.push({ x: words[i + 2], y: words[i + 3], w: words[i + 4], h: words[i + 5], dw: words[i + 6], dh: words[i + 7] });
      if (blits.length > 64) blits.shift();
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
// DOS's prompt, A:\> (its current directory in it: A:\GAMES>)
const prompts = () => (text().match(/A:\\[^>\n]*>/g) || []).length;

// ---- boot to grin, with nothing typed ---------------------------------------------
check(until(() => has('Program Manager') && has('Main'), 3000), 'C4DOS boots into grin: Program Manager is drawn', text());
check(text().includes("installed: each program's memory is freed when it ends"), 'the disk\'s CONFIG.SYS has HEAP.SYS', text());
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
// (a row of the Task List: drawn after its title and before its buttons,
// in its column -- a window behind it may have texts in the same place)
const inList = t => {
  const tl = at('Switch To'), i = frame.indexOf(t);
  return tl && i > lastIndex('Task List') && i < lastIndex('Switch To') && t.y < tl.y && t.y > tl.y - 180 && t.x > tl.x - 40 && t.x < tl.x;
};
const list = find('Control Panel').filter(inList);
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
  const row = find('Program Manager').filter(inList)[0];
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
check(/width=640/.test(iniText) && /height=480/.test(iniText) && /scheme=1/.test(iniText) && /pattern=2/.test(iniText), 'GRIN.INI keeps the choices', iniText);
check(store.has('GRIN.INI') && /scheme=1/.test(Buffer.from(store.get('GRIN.INI')).toString('latin1')), 'GRIN.INI is on B:, the home drive, which the page keeps');

// ---- G5: redirection at A> -----------------------------------------------------------
// The disk's CONFIG.SYS has DEVICE=REDIR.SYS TAP: > >> < and | for DOS's
// builtins, for bare.c programs through DOS's console slots, and for
// c4cc programs through the machine's console tap.
check(text().includes('installed: > >> < and |, with the console tap'), "the disk's CONFIG.SYS has REDIR.SYS TAP", text().slice(0, 800));
check(text().includes('Starting grin') && !store.has('.'), "the AUTOEXEC's ECHO lines have no > for REDIR.SYS to take (no file '.' on B:)", [...store.keys()].join(' '));
let o = typed('DIR > LIST.TXT');
check(!/c4dos32\.c4r/i.test(o), 'DIR > LIST.TXT prints nothing at A>', o);
o = typed('TYPE LIST.TXT');
check(/c4dos32\.c4r/i.test(o) && /^GRIN  <DIR>$/m.test(o), '... and TYPE LIST.TXT shows the listing: the root, its directories too', o);
check(store.has('LIST.TXT') && /c4dos32\.c4r/i.test(Buffer.from(store.get('LIST.TXT')).toString('latin1')), '... which is on B:, the home drive');
o = typed('HELLO > OUT.TXT');
check(!/yello/.test(o), "HELLO > OUT.TXT: a c4cc program's printf goes into the file, through the console tap", o);
check(/yello/.test(typed('TYPE OUT.TXT')), '... TYPE OUT.TXT: yello');
typed('ECHO Ada > IN.TXT');
o = typed('ASKLINE < IN.TXT');
check(/askline: hello, Ada \(3 letters\)/.test(o), 'ASKLINE < IN.TXT: a bare.c program reads its line from the file', o);
o = typed('DIR | DOSFILT');
check(/^> C4DOS32\.C4R$/mi.test(o) && /dosfilt: \d+ lines/.test(o), 'DIR | DOSFILT: a pipe, through a RAM-disk file', o);
o = typed('TYPE IN.TXT | DOSFILT -r');
check(/^ADA$/m.test(o), "TYPE IN.TXT | DOSFILT -r: a c4cc program's READ fed from the pipe (the tap)", o);
check(!/PIPE\d+\.\$\$\$/.test(typed('DIR')), '... and no pipe file is left on the RAM disk');
for (const n of ['LIST.TXT', 'OUT.TXT', 'IN.TXT']) typed('DEL ' + n);
check(!store.has('LIST.TXT') && !store.has('IN.TXT'), 'DEL takes them off B: again');

// ---- GRIN again --------------------------------------------------------------------------
const f0 = presents;
vm.dev.pushInput('GRIN\n');
check(until(() => presents > f0 && has('Program Manager'), 3000) && gui.w === 640, 'GRIN at A> starts it again');
check(rects.some(r => r.rgb === 0x384c60), 'with the scheme GRIN.INI keeps');

// ---- applications (G3): Clock, Calculator, and one that exits ------------------------
// grin's applications are programs of their own (clock.c4r, calc.c4r),
// loaded by DOS, kept while they have a window, and unloaded with their
// memory when the last one goes. The windows are found by their first
// menu ("Settings" for a Clock) and closed with the Task List's End Task.
// A window by its title (Windows 3.1's chrome draws it 7 px down and
// centred), as the rectangle under it whose middle is nearest the title's
const winByTitle = title => {
  const t = find(title).pop();
  if (!t) return null;
  const mid = t.x + t.s.trimEnd().length * 7 / 2;
  const all = rects.filter(r => r.y === t.y - 7 && r.x < t.x && r.x + r.w > t.x && r.w > 100);
  all.sort((a, b) => Math.abs(a.x + a.w / 2 - mid) - Math.abs(b.x + b.w / 2 - mid));
  return all.length ? { x: all[0].x, y: all[0].y, w: all[0].w, h: all[0].h } : null;
};
// Program Manager's window, by its title (other windows have a File menu too)
const pmWin = () => winByTitle('Program Manager') || win('File');
const inPm = all => { const p = pmWin(); return p ? all.filter(t => within(p, t.x, t.y) && t.y > p.y + 40).pop() : undefined; };
// Program Manager to the front first (a click on its title bar), then a
// double click on the item
const groupOf = { Clock: 'Accessories', Calculator: 'Accessories', 'Control Panel': 'Main', 'File Manager': 'Main',
                  'DOS Prompt': 'Main', Notepad: 'Main', Doom: 'Games', 'Wolfenstein 3D': 'Games', Tyrian: 'Games',
                  'Rise of the Triad': 'Games' };
// a group shown: its name in the list down Program Manager's left
const showGroup = g => {
  const p = pmWin();
  const t = p && find(g).filter(t => within(p, t.x, t.y) && t.x < p.x + 140)[0];
  if (t) { click(t.x + 5, t.y + 5); settle(); }
  return !!t;
};
const startItem = (s, group = groupOf[s] || 'Accessories') => {
  const p = pmWin();
  if (p) { click(p.x + 40, p.y + BORDER + 9); settle(); }
  showGroup(group);
  return dbl(s, inPm);
};
const figures = () => find(/^\d\d:\d\d:\d\d$/);
const taskList = () => { key(27, 0, 2); return until(() => has('Switch To') && has('End Task')); };
// a row of the Task List's list: in its column, which starts a little left
// of its Switch To button's label (a window behind it, redrawn as a Clock
// ticks, may put a title of the same text into the frame after the rows)
const inTasks = inList;
const switchTo = title => {
  if (!taskList()) return false;
  const row = find(title).filter(inTasks)[0];
  if (row) { click(row.x + 5, row.y + 5); settle(); }
  clickText('Switch To');
  return until(() => !has('Switch To'));
};
const endTask = title => {
  if (!taskList()) return false;
  const row = find(title).filter(inTasks)[0];
  if (!row) { key(27); return false; }
  click(row.x + 5, row.y + 5); settle();
  clickText('End Task');
  return until(() => !has('Switch To'));
};
const running = () => {
  clickText('Help'); until(() => has('About grin...'));
  clickText('About grin...');
  until(() => find(/running: \d+/).length > 0);
  const m = (at(/running: \d+/) || { s: '' }).s.match(/running: (\d+)/);
  clickText('OK'); until(() => !find(/running: \d+/).length);
  return m ? +m[1] : -1;
};
// every clock showing figures shows new ones (the seconds moved on)
const tick = n => {
  const a = figures(), t0 = Date.now();
  if (a.length !== n) return false;
  const moved = () => {
    const b = figures();
    return b.length === n && a.every(o => b.some(t => t.x === o.x && t.y === o.y && t.s !== o.s));
  };
  // the Clocks read the host's time of day, so this waits in the host's time
  while (!moved() && !halted && Date.now() - t0 < 5000) run();
  return moved();
};
const digital = () => { clickText('Settings'); until(() => has('Digital')); clickText('Digital'); settle(); };

settle();
const heap0 = vm.heap.used;
showGroup('Accessories');
check(has('Accessories') && !!inPm(find('Clock')) && !!inPm(find('Calculator')), 'Program Manager has the Accessories group: Clock and Calculator');
startItem('Clock');
check(until(() => has('Settings')), 'Accessories > Clock opens a Clock (clock.c4r)');
digital();
check(until(() => figures().length === 1), 'Settings > Digital shows the time in figures', JSON.stringify(frame.map(t => t.s)));
startItem('Clock');
check(until(() => find('Settings').length === 2), 'a second launch opens a second Clock');
digital();
// side by side: the Task List's Tile
taskList(); clickText('Tile'); settle(); until(() => !has('Switch To'));
check(until(() => figures().length === 2), 'tiled, both Clocks show their figures', JSON.stringify(frame.map(t => [t.s, t.x, t.y])));
check(running() === 1, 'Help > About: one application running, the second Clock on the same image');
check(tick(2), 'both Clocks tick', JSON.stringify(figures()));
endTask('Clock');
check(until(() => figures().length === 1), 'End Task closes one Clock');
check(tick(1), '... and the other ticks on', JSON.stringify(figures()));
check(running() === 1, '... on the image still loaded');
endTask('Clock');
check(until(() => !figures().length && !has('Settings')), 'End Task closes the other');
settle();
check(running() === 0, 'with its last window gone, Clock is unloaded');
check(Math.abs(vm.heap.used - heap0) < 512, `HEAP_USED is back where it started (${heap0} -> ${vm.heap.used})`);

// Calculator: the C4IX desktop's, as calc.c4r
startItem('Calculator');
check(until(() => has('Hex') && has('Dec') && has('Bin')), 'Accessories > Calculator opens the desktop\'s Calculator (calc.c4r)');
for (const [code, ch, mods] of [[49, '1'], [50, '2'], [56, '*', 1], [49, '1'], [50, '2'], [187, '=']]) key(code, ch.charCodeAt(0), mods || 0);
check(until(() => has('144')), 'typing 12*12= into it shows 144', JSON.stringify(frame.map(t => t.s)));
endTask('Calculator');
check(until(() => !has('Hex')), 'End Task closes it');

// File > Run...: a program that is not an application is a DOS program,
// and runs full screen (G5): HELLO prints at the console and returns, and
// grin takes the display back
const runCmd = cmd => {
  clickText('File'); until(() => has('Run...'));
  clickText('Run...');
  if (!until(() => has('Command Line:'))) return false;
  for (const c of cmd) key(c.charCodeAt(0), c.charCodeAt(0));
  key(13);
  return true;
};
const heap1 = vm.heap.used;
let a0 = text().length;
runCmd('HELLO');
check(until(() => text().slice(a0).includes('yello') && gui.stats().attached && has('Program Manager'), 3000),
  'File > Run... HELLO: not an application, so a DOS program, full screen; grin comes back after it', text().slice(a0));
check(running() === 0, '... and it is not kept loaded');
// an application that calls exit() from its key callback
runCmd('EXITKEY');
check(until(() => has('A key here calls exit(3).')), 'File > Run... EXITKEY starts the test application');
check(running() === 1, '... which is loaded');
// (About raised Program Manager over it: the Task List's Switch To brings it back)
switchTo('Exit Test');
check(lastIndex('A key here calls exit(3).') > lastIndex('File'), 'Switch To brings it to the front');
key(65, 97);
check(until(() => !has('A key here calls exit(3).') && !has('Exit Test')), 'a key in it calls exit(): its window is closed');
settle();
check(running() === 0, '... and it is unloaded');
check(Math.abs(vm.heap.used - heap1) < 512, `... with its memory (HEAP_USED ${heap1} -> ${vm.heap.used})`);
startItem('Clock');
check(until(() => has('Settings')), 'grin carries on: Clock starts again');
endTask('Clock');
check(until(() => !has('Settings')) && has('Program Manager'), '... and closes');

// a Clock left open when the session ends goes with it
startItem('Clock');
check(until(() => has('Settings')), 'a Clock is open as the session ends');
p0 = prompts();
// Program Manager's control menu: Close asks the same
pm = win('File');
if (pm) click(pm.x + BORDER + 9, pm.y + BORDER + 9);
settle();
clickText('Close');
until(() => has('This will end your grin session.'));
clickText('OK');
check(until(() => prompts() > p0 && !gui.stats().attached, 3000), 'Close on Program Manager\'s control menu ends the session too');
const mem = typed('MEM');
check(/programs loaded: 0/.test(mem), 'at A>, MEM: no program is left loaded, the Clock unloaded with grin', mem);
check(/Clock is a grin application/.test(typed('CLOCK')), 'and CLOCK at A> says it needs grin: grin took its table back');

// ---- G4: Program Manager's groups and items, Notepad, File Manager, B: --------------------------
// a window's menu bar's texts, 27 px down from its corner
const menuOf = (w, label) => w && find(label).filter(t => t.y === w.y + 27 && t.x > w.x && t.x < w.x + w.w)[0];
const menu = (title, label, item) => {
  const w = winByTitle(title);
  const m = menuOf(w, label);
  if (!m) return false;
  click(m.x + 5, m.y + 5); settle();
  if (!until(() => has(item))) return false;
  return clickText(item);
};
// (keyCodes as a browser's: '.' is 190, not 46, which is Delete)
const typeText = s => { for (const c of s) key(c === ' ' ? 32 : c === '.' ? 190 : c.toUpperCase().charCodeAt(0), c.charCodeAt(0), /[A-Z]/.test(c) ? 1 : 0); };
// Help > About's count, from Program Manager's own menu
const aboutCount = () => {
  switchTo('Program Manager');
  if (!menu('Program Manager', 'Help', 'About grin...')) return -1;
  until(() => find(/running: \d+/).length > 0);
  const m = (at(/running: \d+/) || { s: '' }).s.match(/running: (\d+)/);
  okBox(); until(() => !find(/running: \d+/).length);
  return m ? +m[1] : -1;
};
const endSession = () => { switchTo('Program Manager'); return menu('Program Manager', 'File', 'Exit grin...') && until(() => has('This will end your grin session.')); };
const okBox = () => { clickText('OK'); settle(); };
const storeText = n => store.has(n) ? Buffer.from(store.get(n)).toString('latin1') : null;

p0 = prompts();
vm.dev.pushInput('GRIN\n');
check(until(() => has('Program Manager'), 3000), 'GRIN again, for the shell applications (G4)');
check(text().includes('files are written to B:'), "the disk's CONFIG.SYS has DRIVES.SYS HOME=B:, and B: can be written", text().slice(0, 600));
const pmIni = storeText('PROGMAN.INI');
check(pmIni && /^\[Main\]$/m.test(pmIni) && /^\[Accessories\]$/m.test(pmIni) && /^\[Games\]$/m.test(pmIni) &&
      /^Doom=\\GAMES\\DOOM\\DOOM,game,window$/m.test(pmIni) && /^Notepad=NOTEPAD,notepad,app$/m.test(pmIni),
  'PROGMAN.INI, written with the default groups, is on B:', pmIni);
showGroup('Main');
check(['File Manager', 'DOS Prompt', 'Control Panel', 'Notepad'].every(s => inPm(find(s))), 'Main: File Manager, DOS Prompt, Control Panel, Notepad');
showGroup('Games');
check(['Wolfenstein 3D', 'Doom', 'Tyrian', 'Rise of the Triad'].every(s => inPm(find(s))), 'Games: the four games');

// each item launches what it should (DOS Prompt and Doom: G5's, below)
startItem('Control Panel');
check(until(() => has('Settings') && has('Display')), 'Main > Control Panel opens Control Panel');
startItem('File Manager');
check(until(() => has('File Manager') && has('RAM:') && has('A:') && has('B:')), 'Main > File Manager opens File Manager: A:, B: and RAM:');
check(has('(files are saved here)'), '... on B:, the home drive');
startItem('Notepad');
check(until(() => has('Untitled - Notepad')), 'Main > Notepad opens Notepad (notepad.c4r)');
check(aboutCount() === 1, '... a grin application: one running');

// File > New, Properties, Delete
startItem('Clock');
endTask('Clock'); settle();
showGroup('Accessories');
check(menu('Program Manager', 'File', 'New...') && until(() => has('New Program Item') && has('Command Line:')), 'File > New opens New Program Item');
typeText('Second Clock'); key(9); typeText('CLOCK'); key(13);
check(until(() => !has('New Program Item') && !!inPm(find('Second Clock'))), '... OK: Accessories has Second Clock');
check(/^Second Clock=CLOCK,app,app$/m.test(storeText('PROGMAN.INI') || ''), '... and PROGMAN.INI on B: has it', storeText('PROGMAN.INI'));
dbl('Second Clock', inPm);
check(until(() => has('Settings')), '... which starts a Clock');
endTask('Clock'); settle();
showGroup('Accessories');
clickText('Second Clock', inPm);
check(menu('Program Manager', 'File', 'Properties...') && until(() => has('Program Item Properties')), 'File > Properties opens the item');
for (let i = 0; i < 5; i++) key(8);
typeText('Ticker'); key(13);
check(until(() => !has('Program Item Properties') && !!inPm(find('Second Ticker'))), '... changed: Second Ticker');
check(/^Second Ticker=CLOCK,app,app$/m.test(storeText('PROGMAN.INI') || ''), '... in PROGMAN.INI too');
check(menu('Program Manager', 'File', 'Delete') && until(() => find(/delete the item 'Second Ticker'/).length > 0), 'File > Delete asks');
clickText('Yes');
check(until(() => !inPm(find('Second Ticker'))) && !/Second Ticker/.test(storeText('PROGMAN.INI') || 'x'), '... Yes: the item is gone, from PROGMAN.INI too');

// Notepad: some text, saved as NOTE.TXT through the file box
switchTo('Untitled - Notepad');
typeText('hello from grin');
check(until(() => has('hello from grin')), 'Notepad takes typing');
check(menu(/ - Notepad$/, 'File', 'Save As...') && until(() => has('Save as:') && has('/B:')), 'Save As is the file box, in /B:, the home drive', JSON.stringify(frame.map(t => t.s)));
for (let i = 0; i < 16; i++) key(8);
typeText('NOTE.TXT'); key(13);
check(until(() => has('NOTE.TXT - Notepad')), 'it saves as NOTE.TXT', JSON.stringify(frame.map(t => t.s)));
check(storeText('NOTE.TXT') === 'hello from grin', 'NOTE.TXT is on B:, as the page would keep it', storeText('NOTE.TXT'));
check(/^NOTE\.TXT$/m.test(storeText('c4dos.dir') || ''), "... and in B:'s c4dos.dir", storeText('c4dos.dir'));

// File Manager lists it on B:, with its size
switchTo('File Manager');
check(until(() => has('NOTE.TXT')), 'File Manager lists NOTE.TXT on B:');
clickText('NOTE.TXT');
check(until(() => has('B:\\NOTE.TXT   15 bytes')), '... its size when selected', JSON.stringify(frame.map(t => t.s)));
check(menu('File Manager', 'File', 'Copy...') && until(() => has('Copy B:\\NOTE.TXT to:')), 'File > Copy... asks where, offering RAM:');
key(13);
clickText('RAM:');
check(until(() => has('NOTE.TXT') && has('RAM:\\   1 file')), '... COPY through DOS: the RAM disk has NOTE.TXT');
clickText('B:');
until(() => has('(files are saved here)'));

// 800x600, for the next boot to find
startItem('Control Panel');
dbl('Display');
until(() => has('800 x 600'));
clickText('800 x 600'); clickText('OK');
check(until(() => gui.w === 800), 'Display: 800 x 600, kept in GRIN.INI on B:');
check(/width=800/.test(storeText('GRIN.INI') || ''), '... width=800 there', storeText('GRIN.INI'));
endSession(); clickText('OK');
check(until(() => prompts() > p0 && !gui.stats().attached, 3000), 'File > Exit grin, Notepad and File Manager open: back at A>');
check(/hello from grin/.test(typed('TYPE B:NOTE.TXT')), 'TYPE B:NOTE.TXT at A>');
check(/NOTE\.TXT/.test(typed('DIR B:')), 'DIR B: lists it');

// ---- a second boot, with what B: kept --------------------------------------------------
gui = new GuiDevice({ w: 640, h: 480 });
out = []; frame = []; cur = []; rects = []; curRects = []; partial = false; halted = false;
vm = createMachine({ arenaMb: 64, mhz: 400, gui, drives: [{ files: new LazyDisk(index), writable: false, sink: null }, homeDrive()], onByte: b => out.push(b) });
boot(vm, image, ['c4dos32.c4r']);
check(until(() => has('Program Manager'), 3000), 'boot again, with the files B: kept');
check(until(() => gui.w === 800 && gui.h === 600), `grin comes up at 800x600, as Control Panel left it (${gui.w}x${gui.h})`);
startItem('File Manager');
check(until(() => has('File Manager') && has('NOTE.TXT')), 'File Manager: NOTE.TXT is still on B:');
dbl('NOTE.TXT');
check(until(() => has('NOTE.TXT - Notepad') && has('hello from grin')), 'a double click opens it in Notepad, text and all');
endTask('NOTE.TXT - Notepad');
switchTo('File Manager');
clickText('NOTE.TXT');
check(menu('File Manager', 'File', 'Delete...') && until(() => has('Delete B:\\NOTE.TXT?')), 'File > Delete... asks');
clickText('Yes');
check(until(() => !has('NOTE.TXT')), '... Yes: NOTE.TXT is gone from the list');
check(!store.has('NOTE.TXT') && !/NOTE\.TXT/.test(storeText('c4dos.dir') || ''), "... from B:, its c4dos.dir, and the page's store");

// ---- G5: DOS programs from grin --------------------------------------------------------
// A window's texts: drawn inside it (another window drawn under it in the
// same frame can put texts there too, so a check that something is NOT in
// a window is made with no other window over the place)
const inWin = (w, re) => !!w && frame.some(t => within(w, t.x, t.y) && re.test(t.s.trimEnd()));
// (by its title, as winByTitle, but the whole window: the other DOS
// Prompt's frame has lines at the same height)
const dosWin = n => {
  const t = find('DOS Prompt ' + n).pop();
  if (!t) return null;
  const r = rects.filter(r => r.y === t.y - 7 && r.x < t.x && r.x + r.w > t.x && r.h > 200).sort((a, b) => b.h - a.h)[0];
  return r ? { x: r.x, y: r.y, w: r.w, h: r.h } : null;
};
const typeLine = s => { typeText(s); key(13); };
// (typeText knows letters, digits, spaces and '.'; these need the rest)
const typeRaw = s => { for (const c of s) key(c.charCodeAt(0), c.charCodeAt(0)); key(13); };

startItem('Clock');
digital();
check(until(() => figures().length === 1), 'a Clock, in figures, for the DOS Prompt to run beside');
startItem('DOS Prompt');
check(until(() => !!dosWin(1) && has(/^EXIT closes it\./)), 'Main > DOS Prompt opens DOS Prompt 1: C4DOS in a window', JSON.stringify(frame.map(t => t.s)));
check(until(() => inWin(dosWin(1), /^A:\\>$/)), '... at A:\\>');
typeLine('ECHO in the first window');
check(until(() => inWin(dosWin(1), /^in the first window$/)), 'ECHO in it answers in it');
startItem('DOS Prompt');
check(until(() => !!dosWin(2)), 'a second DOS Prompt: DOS Prompt 2');
typeLine('DIR');
check(until(() => inWin(dosWin(2), /^c4dos32\.c4r$/i) && inWin(dosWin(2), /^GAMES  <DIR>$/)), 'DIR in DOS Prompt 2 lists A: there');
// directories (C4DOS's API v7): each window keeps its own current one
typeRaw('CD \\GAMES\\DOOM');
check(until(() => inWin(dosWin(2), /^A:\\GAMES\\DOOM>$/)), 'CD \\GAMES\\DOOM in DOS Prompt 2: its prompt says where it is');
typeLine('DIR');
check(until(() => inWin(dosWin(2), /^doom1\.wad$/i)), '... and DIR lists Doom there');
switchTo('DOS Prompt 1');
typeLine('CD');
check(until(() => inWin(dosWin(1), /^A:\\$/)), 'CD in DOS Prompt 1 says A:\\: the other window\'s directory is its own');
switchTo('DOS Prompt 2');
typeRaw('CD \\');
check(until(() => inWin(dosWin(2), /^A:\\>$/)), 'CD \\ takes DOS Prompt 2 back to the root');

// a bare.c program reading a line in its window: its read pumps grin's
// loop, so the Clock ticks while it waits, and the other DOS Prompt is busy
typeLine('ASKLINE');
check(until(() => inWin(dosWin(2), /^askline: what is your name\?$/)), 'ASKLINE in DOS Prompt 2 asks, in that window');
const seen0 = timesSeen.size, t0 = Date.now();
while (timesSeen.size < seen0 + 2 && Date.now() - t0 < 6000 && !halted) run();
check(timesSeen.size >= seen0 + 2, `... and the Clock keeps ticking while it waits (${timesSeen.size - seen0} new times drawn)`);
startItem('Doom');
check(until(() => find(/A DOS program is running \(ASKLINE\)/).length > 0), 'Games > Doom meanwhile: one DOS program at a time, says grin');
okBox();
switchTo('DOS Prompt 1');
typeLine('DIR');
check(until(() => inWin(dosWin(1), /^grin: busy -- ASKLINE is running/)), 'DIR in DOS Prompt 1 meanwhile: busy');
switchTo('DOS Prompt 2');
typeLine('Ada');
check(until(() => inWin(dosWin(2), /^askline: hello, Ada \(3 letters\)$/)), 'Ada typed in DOS Prompt 2 is the line it reads');

// a c4cc program (its printf through the console tap), redirection and a pipe
typeLine('HELLO');
check(until(() => inWin(dosWin(2), /^yello$/)), "HELLO in DOS Prompt 2: a c4cc program's output, from the tap");
typeRaw('DIR | DOSFILT');
check(until(() => inWin(dosWin(2), /^> C4DOS32\.C4R$/i) && inWin(dosWin(2), /^dosfilt: \d+ lines$/)), 'DIR | DOSFILT in it: a pipe');
typeRaw('ECHO ECHO from a batch file > B:RUNME.BAT');
check(until(() => /from a batch file/.test(storeText('RUNME.BAT') || '')), 'ECHO ... > B:RUNME.BAT: redirected onto B:', storeText('RUNME.BAT'));
typeLine('EXIT');
check(until(() => !dosWin(2)), 'EXIT closes DOS Prompt 2');
check(until(() => inWin(dosWin(1), /^in the first window$/)) && !has(/^c4dos32\.c4r$/i) && !has(/^yello$/),
  'DOS Prompt 1 has its own lines and none of the other window\'s');

// File Manager runs a batch file in a DOS Prompt
switchTo('File Manager');
menu('File Manager', 'File', 'Refresh');
check(until(() => has('RUNME.BAT')), 'File Manager lists RUNME.BAT on B:');
dbl('RUNME.BAT');
check(until(() => !!dosWin(2) && inWin(dosWin(2), /^from a batch file$/)), '... and a double click runs it in a DOS Prompt', JSON.stringify(frame.map(t => t.s)));
typeLine('EXIT');
switchTo('DOS Prompt 1');
typeLine('EXIT');
check(until(() => !dosWin(1) && !dosWin(2)), 'EXIT closes the other one too');

// File Manager in directories (C4DOS's API v7): A:'s root has folders; a
// double click opens one, .. comes back up
switchTo('File Manager');
clickText('A:');
check(until(() => has('GAMES') && has('c4dos32.c4r')), 'File Manager on A:: the root, its folders in it', JSON.stringify(frame.map(t => t.s)));
dbl('GAMES');
check(until(() => has('DOOM') && has('..') && has(/^A:\\GAMES +\d+ files$/)), '... a double click opens GAMES', JSON.stringify(frame.map(t => t.s)));
dbl('DOOM');
check(until(() => has('doom1.wad')), '... and DOOM in it');
dbl('..'); dbl('..');
check(until(() => has('c4dos32.c4r') && !has('..')), '... and .. twice comes back up to the root');
clickText('B:');
endTask('Clock');
until(() => !figures().length);

// ---- G6: games in a window ------------------------------------------------------------
// A game's window: found by its Size menu, the first on its bar (win), or
// by its title. Its control menu's Close closes it.
// (each game in its directory, A:\GAMES\NAME: doom.c4r in GAMES/DOOM)
const gameDir = c4r => 'GAMES/' + c4r.replace(/\.c4r$/i, '').toUpperCase();
const haveGame = (c4r, data) => existsSync(join(DISK, gameDir(c4r), c4r)) && existsSync(join(DISK, gameDir(c4r), data));
const gameWin = () => win('Size');
const closeBox = w => {
  click(w.x + BORDER + 9, w.y + BORDER + 9); settle();
  if (until(() => has('Close'), 200)) clickText('Close');
};
const framesIn = (w, n) => !!w && blits.length >= n && blits.slice(-n).every(b =>
  within(w, b.x, b.y) && within(w, b.x + b.dw - 1, b.y + b.dh - 1));
// The Task List's rows, by where they are under its buttons (a partial
// frame may draw them after the buttons, where inTasks does not look);
// and End Task on one of them
const taskRows = () => { const tl = at('Switch To'); return frame.filter(t => tl && t.y < tl.y && t.y > tl.y - 180 && t.x > tl.x - 40 && t.x < tl.x); };
// (title a string, or a pattern; a "Save changes?" it raises is answered No)
const endTask2 = title => {
  if (!taskList()) return false;
  const row = taskRows().filter(t => typeof title === 'string' ? t.s.trimEnd() === title : title.test(t.s.trimEnd()))[0];
  if (!row) { key(27); until(() => !has('Switch To')); return false; }
  click(row.x + 5, row.y + 5); settle();
  clickText('End Task');
  const r = until(() => !has('Switch To'));
  if (until(() => find(/Save changes/).length > 0, 30)) clickText('No');
  return r;
};
const NOTEPAD = /^Untitled( \*)? - Notepad$/;
const haveDoom = haveGame('doom.c4r', 'doom1.wad');
if (!haveDoom) console.log('test-grin: Doom in a window SKIPPED (no doom on the disk)');
if (haveDoom) {
  startItem('Clock');
  digital();
  until(() => figures().length === 1);
  startItem('Notepad');
  until(() => has('Untitled - Notepad'));
  settle();
  const heapW0 = vm.heap.used;
  const b0 = blitCount;
  startItem('Doom');
  check(until(() => !!gameWin() && blitCount > b0 + 40, 4000), "Games > Doom plays in a window of grin's: its frames arrive", text().slice(-400));
  let dw = gameWin();
  check(framesIn(dw, 20) && blits.slice(-20).every(b => b.w === 320 && b.h === 200 && b.dw === 640 && b.dh === 400),
    `... 320x200 frames, at 2x (640x400) in the window's rectangle at 800x600 (${JSON.stringify(dw)}, ${JSON.stringify(blits[blits.length - 1])})`);
  check(gui.stats().attached && has('Program Manager'), '... and grin keeps the display: Program Manager is drawn beside it');
  // the cooperation: Doom runs grin's loop in its polls, so the Clock ticks
  const seen0 = timesSeen.size, f0 = blitCount, t0 = Date.now();
  while (timesSeen.size < seen0 + 2 && Date.now() - t0 < 8000 && !halted) run();
  check(timesSeen.size >= seen0 + 2 && blitCount > f0 + 20,
    `... while Doom plays, the Clock's seconds advance (${timesSeen.size - seen0} new times, ${blitCount - f0} frames meanwhile)`);
  // keys go to the window with the focus: Notepad, clicked, takes typing
  // while Doom plays on; then Doom's window, clicked, has them again
  switchTo('Untitled - Notepad');
  typeText('typed beside Doom');
  const f1 = blitCount;
  check(until(() => has('typed beside Doom')) && until(() => blitCount > f1 + 10),
    'Notepad, switched to, takes typing while Doom plays on in its window');
  switchTo('Doom');
  // closing the window ends Doom; grin carries on, with the heap where it was
  dw = gameWin();
  if (dw) closeBox(dw);
  check(until(() => !gameWin(), 3000), 'its control menu\'s Close closes Doom\'s window, and Doom ends', text().slice(-400));
  const f2 = blitCount;
  settle();
  check(blitCount === f2 && gui.stats().attached && has('Program Manager') && has('typed beside Doom'),
    'grin carries on: no more frames, Program Manager and Notepad still there');
  endTask2(NOTEPAD);
  until(() => !has('typed beside Doom'));
  settle();
  // (Notepad's text and window gone, the Clock still open: as before Doom)
  startItem('Notepad'); until(() => has('Untitled - Notepad')); settle();
  const heapW1 = vm.heap.used;
  check(Math.abs(heapW1 - heapW0) / heapW0 <= 0.01, `HEAP_USED after Doom in a window is where it was before (${heapW0} -> ${heapW1}; Notepad was closed and opened again)`);
  // and Doom alone, again, beside the same windows: to the byte
  settle();
  const heapW2 = vm.heap.used, b2 = blitCount;
  startItem('Doom');
  until(() => !!gameWin() && blitCount > b2 + 20, 4000);
  if (gameWin()) closeBox(gameWin());
  until(() => !gameWin(), 3000);
  settle();
  check(vm.heap.used === heapW2 && blitCount > b2 + 20, `Doom again, closed: HEAP_USED the same to the byte (${heapW2} -> ${vm.heap.used})`);
  endTask2(NOTEPAD);
  endTask2('Clock');
  check(until(() => !find(NOTEPAD).length && !figures().length), "Notepad and the Clock closed by the Task List's End Task");
}

// Wolfenstein 3D from File Manager (a .C4R that Program Manager runs in a
// window runs in one from there too), Tyrian and Rise of the Triad from
// Program Manager: each draws in a window, and closes
const oneFrame = (name, start, data, c4r) => {
  if (!haveGame(c4r, data)) { console.log(`test-grin: ${name} in a window SKIPPED (not on the disk)`); return; }
  const b0 = blitCount;
  start();
  if (!check(until(() => !!gameWin() && blitCount > b0 && framesIn(gameWin(), 1), 4000), `${name} draws a frame in a window of grin's`, text().slice(-300))) return;
  closeBox(gameWin());
  check(until(() => !gameWin(), 3000) && has('Program Manager'), `... and its window closes, and ${name} with it`, text().slice(-300));
  settle();
};
oneFrame('Wolfenstein 3D', () => {
  startItem('File Manager');
  until(() => has('File Manager'));
  clickText('A:');
  // A:\GAMES\WOLF3D, and a double click on wolf3d.c4r there
  until(() => has('GAMES'));
  dbl('GAMES');
  until(() => has('WOLF3D'));
  dbl('WOLF3D');
  until(() => has('wolf3d.c4r'));
  dbl('wolf3d.c4r');
}, 'vswap.wl1', 'wolf3d.c4r');
endTask2('File Manager');
oneFrame('Tyrian', () => startItem('Tyrian'), 'tyrian', 'tyrian.c4r');
oneFrame('Rise of the Triad', () => startItem('Rise of the Triad'), 'HUNTBGIN.WAD', 'rott.c4r');

// A dialog opened while a game plays is grin's, in grin's heap generation,
// and still works once the game has ended (docs/grin-plan.md 3.3): File >
// Run... DOOM -timedemo demo3 -show (in a window, as its item says), which
// ends by itself; meanwhile Program Manager's File > Run... box, left
// open; after Doom, CLOCK typed into it starts a Clock.
if (haveDoom) {
  const b0 = blitCount;
  switchTo('Program Manager');
  runCmd('\\GAMES\\DOOM\\DOOM -timedemo demo3 -show -nosound');
  check(until(() => !!gameWin() && blitCount > b0 + 20, 4000), 'File > Run... \\GAMES\\DOOM\\DOOM -timedemo demo3 -show: a timedemo, in a window', text().slice(-300));
  switchTo('Program Manager');
  menu('Program Manager', 'File', 'Run...');
  const b1 = blitCount;
  check(until(() => has('Command Line:')) && until(() => blitCount > b1 + 10),
    '... and meanwhile File > Run... opens its box, Doom drawing on under it', `${blitCount - b1} frames; ${JSON.stringify(frame.map(t => t.s.trimEnd()))}`);
  // (Program Manager, maximised, covers Doom's window: the end is when its
  // frames stop for good -- 400 runs, two seconds of the machine's, as a
  // timedemo pauses for less -- and the Task List has no Doom any more)
  let last = blitCount, still = 0;
  const tq = Date.now();
  while (still < 400 && Date.now() - tq < 1800000 && !halted) { run(); if (blitCount === last) ++still; else { still = 0; last = blitCount; } }
  check(still >= 400, `... the timedemo ends by itself (${blitCount - b0} frames in all)`, text().slice(-300));
  settle();
  for (const c of 'CLOCK') key(c.charCodeAt(0), c.charCodeAt(0));
  key(13);
  check(until(() => !has('Command Line:') && has('Settings')), 'the box opened during the game still works after it: CLOCK typed there starts a Clock',
    JSON.stringify(frame.map(t => t.s.trimEnd())));
  taskList();
  // (its rows by where they are: a partial frame may draw them after its buttons)
  const rows = taskRows().map(t => t.s.trimEnd());
  check(rows.includes('Clock') && !rows.includes('Doom'), "... and the Task List has the Clock, and Doom's window no more", JSON.stringify(rows));
  key(27); until(() => !has('Switch To'));
  endTask2('Clock');
  check(until(() => !has('Settings')), "... and End Task closes the Clock");
}

// Doom full screen from Program Manager: grin gives the display up, Doom
// takes it; F10 then Y quits; grin takes the display back, at its size,
// and draws everything. Twice, with HEAP_USED where it was each time.
const slowKey = (code, ch = 0) => {
  gui.event({ kind: 'keydown', code, char: ch, mods: 0 }); for (let i = 0; i < 10; i++) run(2e6);
  gui.event({ kind: 'keyup', code, char: ch, mods: 0 }); for (let i = 0; i < 10; i++) run(2e6);
};
// (its item runs it in a window since G6: Properties sets it to full screen)
if (haveDoom) {
  switchTo('Program Manager');
  showGroup('Games');
  clickText('Doom', inPm);
  check(menu('Program Manager', 'File', 'Properties...') && until(() => has('Program Item Properties')), "Doom's Properties, to run it full screen");
  clickText('DOS program, full screen');
  key(13);
  check(until(() => !has('Program Item Properties')) && /^Doom=\\GAMES\\DOOM\\DOOM,game,full$/m.test(storeText('PROGMAN.INI') || ''),
    '... full screen, in PROGMAN.INI too', storeText('PROGMAN.INI'));
}
const doomHeap = [];
settle();
const doomHeap0 = vm.heap.used;
for (let r = 1; haveDoom && r <= 2; r++) {
  const f0 = presents;
  startItem('Doom');
  if (!check(until(() => presents > f0 + 40, 4000), `Doom ${r}: Games > Doom runs it full screen (frames arrive)`, text().slice(-400))) break;
  frame = []; cur = []; rects = []; curRects = [];
  slowKey(121);                       // F10: quit
  slowKey(89, 121);                   // Y
  if (!check(until(() => gui.stats().attached && has('Program Manager') && gui.w === 800 && gui.h === 600, 4000),
    `Doom ${r}: F10 and Y quit it, and grin takes the display back at 800x600 and draws everything`, text().slice(-400))) break;
  settle();
  doomHeap.push(vm.heap.used);
}
if (doomHeap.length === 2) {
  const worst = Math.max(...doomHeap.map(u => Math.abs(u - doomHeap0) / doomHeap0));
  check(worst <= 0.01, `HEAP_USED after each Doom is where it was before (${doomHeap0} -> ${doomHeap.join(', ')})`);
}
const loadedAfter = aboutCount();
check(loadedAfter === 0, `grin carries on after Doom: Help > About, no application loaded (${loadedAfter})`);
startItem('Clock');
check(until(() => has('Settings')), '... a Clock starts');
endTask('Clock');

console.log(fail ? 'test-grin: FAILED' : 'test-grin: OK');
process.exit(fail);
