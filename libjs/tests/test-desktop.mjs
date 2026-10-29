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
import { layUploads } from '../uploads.js';

const ROOT = new URL('../../src/c4bb/images/', import.meta.url).pathname;
// the screen's size: the desktop's own, until the test tells it another
let SW = 800, SH = 600;
const TASK_H = 30;
let fail = 0;
const check = (cond, what, extra = '') => {
  console.log(`test-desktop: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(0, 1500) : ''}`);
  if (!cond) fail = 1;
  return cond;
};

// The disk the way the web page now serves it: lazily, a file read from
// the host the first time the machine opens it (worker.js LazyFiles).
const index = JSON.parse(readFileSync(join(ROOT, 'disk/index.json'), 'utf8'));
class LazyDisk extends Map {
  constructor(idx) { super(); this.sizes = new Map(idx); this.fetched = new Set(); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) {
    if (super.has(n)) return super.get(n);
    if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(ROOT, 'disk', n)));
    this.fetched.add(n); super.set(n, b);
    return b;
  }
}
const files = new LazyDisk(index);
// The page's Uploads, laid over drive 0 as worker.js does (uploads.js):
// C4IX lists them in /home. In the order given here they are made, the
// VFS's dates, which with their names, sizes and extensions sorts each
// of Files' orders differently.
const UPLOADS = [['bravo.dat', 500], ['charlie.c', 9000], ['alpha.txt', 3000]];
layUploads(files, UPLOADS.map(([n, z]) => [n, new TextEncoder().encode(`uploaded ${n}\n`.repeat(z)).slice(0, z)]));
const gui = new GuiDevice({ w: 640, h: 480 });
const out = [];
const vm = createMachine({ arenaMb: 64, gui, drives: [{ files, writable: false, sink: null }], onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const consoleText = () => Buffer.from(out).toString('latin1');

// The screen: every string of the last presented frame, with its position.
//
// A frame that begins with CLEAR is the whole scene. One that does not --
// the desktop's damage redraw (docs/c4ix-cpu.md, step 11) -- paints over
// part of the frame before, as the display does: a rectangle it fills,
// inside its clip, covers what earlier frames put there, and a string it
// draws outside its clip is not seen. Within one frame nothing covers
// anything, as before: the tests look for text by what was drawn.
let frame = [], cur = [], presents = 0, rects = [], curRects = [], pointer = 0;
let gen = 0, clip = null, partial = false;
let imgdefs = 0, imgs = 0, curImgs = 0;
const imgSize = new Map();
const within = (r, x, y) => x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
const cut = (a, b) => {
  const x = Math.max(a.x, b.x), y = Math.max(a.y, b.y);
  return { x, y, w: Math.min(a.x + a.w, b.x + b.w) - x, h: Math.min(a.y + a.h, b.y + b.h) - y };
};
let rgbFrames = 0, rgbLast = null;
const pump = () => {
  const words = gui.drain();
  for (let i = 0; words && i < words.length; i += words[i]) {
    const t = words[i + 1];
    if (t === CMD.CLEAR) { cur = []; curRects = []; partial = false; curImgs = 0; }
    if (t === CMD.IMGDEF) { imgSize.set(words[i + 2], { w: words[i + 3], h: words[i + 4] }); imgdefs++; }
    if (t === CMD.CLIP) clip = { x: words[i + 2], y: words[i + 3], w: words[i + 4], h: words[i + 5] };
    if (t === CMD.NOCLIP) clip = null;
    // an image (the desktop's picture) covers what is under it, as a rectangle does
    const z = t === CMD.IMG ? imgSize.get(words[i + 2]) : null;
    if (t === CMD.RECT || z) {
      const r = z ? { x: words[i + 3], y: words[i + 4], w: z.w, h: z.h, rgb: -1, gen }
                  : { x: words[i + 2], y: words[i + 3], w: words[i + 4], h: words[i + 5], rgb: words[i + 6], gen };
      if (z) curImgs++;
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
        cur.push({ x: words[i + 2], y: words[i + 3], s, rgb: words[i + 4], adv: words[i + 7], gen });
    }
    if (t === CMD.PRESENT) { frame = cur.slice(); rects = curRects.slice(); imgs = curImgs; presents++; gen++; partial = true; }
    if (t === CMD.CURSOR) pointer = words[i + 2];
    // an 8-bit frame, made 32-bit by the device (GUI_BLIT8): count them,
    // and keep the last one's size and a sum of its pixels
    if (t === CMD.RGB) {
      rgbFrames++;
      const w = words[i + 4], h = words[i + 5];
      let sum = 0;
      for (let k = 0; k < w * h; k += 97) sum = (sum * 31 + words[i + 8 + k]) | 0;
      rgbLast = { w, h, dw: words[i + 6], dh: words[i + 7], sum };
    }
  }
};
// run() returns early whenever the guest sleeps, so waiting is until()'s job
const run = (cycles = 2e6) => { const r = vm.run(cycles); pump(); return r; };
const until = (cond, budget = 400) => { for (let i = 0; i < budget; i++) { if (cond()) return true; if (run() === STOP.HALT) return cond(); } return cond(); };
const settle = () => { for (let i = 0; i < 20; i++) run(); return true; };
const screen = () => frame.map(t => t.s).join('\n');
const has = re => typeof re === 'string' ? frame.some(t => t.s.trimEnd() === re) : frame.some(t => re.test(t.s));
const find = s => frame.filter(t => (typeof s === 'string' ? t.s.trimEnd() === s : s.test(t.s)));
// Monospaced text as lines: the Editor colours a line in runs, one string
// each, so a run that starts where the last ended joins it, and the cursor
// drawn over a character is left out.
const rows = () => {
  const mono = frame.filter(t => t.adv > 0).sort((a, b) => a.y - b.y || a.x - b.x);
  const out = [];
  for (const t of mono) {
    const p = out[out.length - 1];
    if (p && p.y === t.y && p.adv === t.adv && t.x === p.x + p.s.length * p.adv) p.s += t.s;
    else if (p && p.y === t.y && t.x >= p.x && t.x < p.x + p.s.length * p.adv) continue;
    else out.push({ ...t });
  }
  return out;
};
// Drag a scroll bar's thumb, which sits at the top of its track: the
// track is the 16-pixel-wide 0xe0e0e0 rectangle PICK chooses, the last
// drawn of them (the topmost window's).
const dragThumb = (pick, dy) => {
  const bar = rects.filter(r => r.w === 16 && r.rgb === 0xe0e0e0).filter(pick).pop();
  if (!bar) return false;
  const x = bar.x + 8, y = bar.y + 20;
  ev({ kind: 'move', x, y, buttons: 0 }); ev({ kind: 'down', x, y, button: 0 });
  for (let k = 1; k <= 4; k++) ev({ kind: 'move', x, y: y + (dy * k) / 4, buttons: 1 });
  ev({ kind: 'up', x, y: y + dy, button: 0 });
  return true;
};
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
const start = item => { click(20, SH - TASK_H + 12); until(() => has('Power Off...')); const ok = clickText(item); settle(); return ok; };
// Files' file list: right of its folder tree, inside its window. The
// window is found by its title (drawn 27 px in from the window's left, 7 down).
const explorer = () => {
  const t = frame.find(t => /^Files - /.test(t.s) && t.y < SH - TASK_H);
  return t ? { x: t.x - 27, y: t.y - 7 } : null;
};
const inList = all => {
  const e = explorer();
  if (!e) return undefined;
  return all.filter(t => t.x > e.x + 200 && t.x < e.x + 600 && t.y > e.y + 60 && t.y < e.y + 420).pop();
};
// anywhere in Files' window: its menu bar, the list's column headers
const inFiles = all => {
  const e = explorer();
  if (!e) return undefined;
  return all.filter(t => t.x > e.x && t.x < e.x + 600 && t.y > e.y && t.y < e.y + 420).pop();
};
const noDialog = () => !has('OK') && !has('Yes');

// ---- boot C4IX, start the desktop ----------------------------------------------
check(until(() => consoleText().includes('c4ix:/$'), 2000), 'C4IX boots to its shell');
check(/entries left on the host until first use/.test(consoleText()) && files.fetched.size < 12,
  `booting reads ${files.fetched.size} of ${index.length} disk files; the rest stay lazy`, [...files.fetched].join(' '));
// The Editor's :make links with /usr/lib/libc4ix.c4l, which the root
// archive brings: let its background unpack (task 4) finish first
typeBytes(vm, new TextEncoder().encode('wait 4\n'));
check(until(() => consoleText().includes('[4] done (0)'), 3000), 'the root archive unpacks in the background');
// No screen saver: headless, an idle machine covers its five minutes
// within a long build (the savers are tried below, from Settings)
{
  const c0 = consoleText().length;
  typeBytes(vm, new TextEncoder().encode('mkdir /etc\n'));
  until(() => /c4ix:\/\$ ?$/.test(consoleText().slice(c0).trimEnd() + ' '));
  const c1 = consoleText().length;
  typeBytes(vm, new TextEncoder().encode('echo scheme 0 bg -1 wall 1 saver 0 wait 5 > /etc/desktop.cfg\n'));
  until(() => /c4ix:\/\$ ?$/.test(consoleText().slice(c1).trimEnd() + ' '));
}
// The uploads are in /home, where ls lists them, and open there and by
// their bare names, as they did before they were listed anywhere
{
  const sh = cmd => {
    const c0 = consoleText().length;
    typeBytes(vm, new TextEncoder().encode(cmd + '\n'));
    until(() => /c4ix:\/\$ ?$/.test(consoleText().slice(c0).trimEnd() + ' '));
    return consoleText().slice(c0);
  };
  const ls = sh('ls /home');
  check(UPLOADS.every(([n]) => ls.includes(n)), 'ls lists the uploads in /home', ls);
  check(sh('cat /home/alpha.txt').includes('uploaded alpha.txt'), 'cat reads one there', consoleText().slice(-300));
  check(sh('cat bravo.dat').includes('uploaded bravo.dat'), 'and by its bare name, from the host', consoleText().slice(-300));
  sh('mkdir /home/zeta');
}
typeBytes(vm, new TextEncoder().encode('desktop\n'));
check(until(() => consoleText().includes('desktop: running on the display')), 'desktop starts from the console');
check(until(() => gui.w === SW && gui.h === SH && presents > 0), `it asks for ${SW}x${SH} and draws`);
check(until(() => has(/c4ix:\/\$/), 800), 'a Terminal opens with a shell prompt', screen());
check(has('Go') && has(/[0-9]{1,2}:[0-9]{2} [AP]M/), 'the taskbar has Go and a clock');
check(until(() => has(/^CPU \d{1,3}%$/)), 'and a CPU meter beside the clock', screen());
check(['Filesystem', 'Terminal', 'Text Editor', 'IDE', 'Calculator', 'System Monitor'].every(has), 'the desktop icons are there, the IDE among them');

// ---- Terminal ----------------------------------------------------------------
type('ps\n');
// the shell finds the desktop as /bin/desktop now (it tries /bin first)
check(until(() => has(/ (c4ix-)?desktop/) && has(/tasks,/)), 'ps typed in the Terminal runs in its shell', screen());

// ---- the Terminal's line editing, history and scrollback --------------------------
// What is typed after the last prompt (the Terminal runs c4ix-sh -p: the prompt is on the input line).
const inputLine = () => {
  const ps = frame.filter(t => /^c4ix:\S*\$/.test(t.s));
  if (!ps.length) return null;
  const y = Math.max(...ps.map(t => t.y)), x = ps[0].x;
  const row = frame.filter(t => t.y === y && t.x >= x && t.x < x + 640).sort((a, b) => a.x - b.x).map(t => t.s).join('');
  return row.replace(/^c4ix:\S*\$ ?/, '').trimEnd();
};
check(until(() => inputLine() === ''), 'the prompt sits on the input line', screen());
type('echo one\n');
until(() => has('one'));
type('echo two\n');
until(() => has('two') && inputLine() === '');
key('', 0, 38); key('', 0, 38);                                // Up, Up
check(until(() => inputLine() === 'echo one'), 'Up brings back the line before last', inputLine());
key('', 0, 40);                                                // Down
check(until(() => inputLine() === 'echo two'), 'and Down the last', inputLine());
key('', 0, 40);                                                // Down: the empty line again
check(until(() => inputLine() === ''), 'and Down past the newest gives an empty line', inputLine());
type('echo xz');
key('', 0, 37);                                                // Left
type('y');
check(until(() => inputLine() === 'echo xyz'), 'Left moves into the line, and a key goes in there', inputLine());
key('', 0, 36); key('', 0, 46);                                // Home, Delete: the "e"
check(until(() => inputLine() === 'cho xyz'), 'Home and Delete edit at the start of the line', inputLine());
type('e'); key('', 0, 35); type('!\n');                        // "e", End, "!", Enter
check(until(() => has('xyz!')), 'End goes back to the end, and Enter sends the whole line', screen());
type('cat /usr/src/c4ix/user/top.c\n');
until(() => has(/^\s*return 0;/) && inputLine() === '', 600);
check(!has(/^\/\/ C4IX userland: top\./), 'a long file scrolls its first line off the Terminal', screen());
for (let i = 0; i < 8; i++) key('', 0, 33);                    // PgUp, to the start
check(until(() => has(/^\/\/ C4IX userland: top\./)), 'and PgUp scrolls back to it', screen());
key('', 0, 34);                                                // PgDn
type('echo back\n');
check(until(() => has('back') && !has(/^\/\/ C4IX userland: top\./)), 'typing comes back to the bottom', screen());

// ---- top runs until Ctrl-C ----------------------------------------------------------------
type('top\n');
check(until(() => has(/Ctrl-C to stop/), 800), 'top starts in the Terminal', screen());
const t0 = vm.dev.simMs();
until(() => vm.dev.simMs() - t0 > 5000, 3000);
check(has(/Ctrl-C to stop/) && !frame.some(t => /^c4ix:\/\$/.test(t.s) && t.y > 300), 'and is still running five machine-seconds later', screen());
key('c', 2, 67);
check(until(() => has(/\^C/) && frame.filter(t => /^c4ix:\/\$/.test(t.s)).length >= 1), 'Ctrl-C stops it and the prompt comes back', screen());

// ---- Ctrl-C drops what was typed at the job and not read --------------------------------------
// as a tty flushes its input: before, the shell read the line next and ran it
key('l', 2, 76);
type('spin 1000\n');
settle();
type('ls /usr/lib\n');
settle();
key('c', 2, 67);
until(() => has(/\^C/) && inputLine() === '', 800);
const tc = vm.dev.simMs();
until(() => vm.dev.simMs() - tc > 1000, 2000);                // time enough for the shell to have run it
check(has(/\^C/) && inputLine() === '' && !has(/libarc\.c4l/), 'Ctrl-C drops a line typed at the job, and the shell does not run it', screen());

// ---- c4m boots C4KE in the Terminal, and C4KE reads what is typed there ------------------
// C4KE's shell polls /dev/stdin, which C4IX gave it as the machine's console;
// in a Terminal that is nobody's keyboard, and every command waited for ever
key('l', 2, 76);
type('cd /usr\n');
until(() => has(/^c4ix:\/usr\$/));
type('c4m load-c4r.c -- c4ke.c4r\n');
check(until(() => has(/^c4sh>/), 20000), 'c4m boots C4KE to its shell in the Terminal, from /usr/c4ke.c4r', screen());
type('echo from-c4ke\n');
check(until(() => has('from-c4ke') && find(/^c4sh>/).length >= 2, 4000), 'a command typed there runs in C4KE and its prompt comes back', screen());
// Ctrl-C goes to the job C4KE is running, through c4m, as on a host
// terminal; it used to end c4m itself
key('l', 2, 76);
type('spin\n');
check(until(() => has(/^spin: running forever/), 4000), 'C4KE runs spin there', screen());
key('c', 2, 67);
check(until(() => has(/^spin: SIGINT/) && has(/^c4sh>/), 4000) && !has(/^c4ix:\/usr\$/),
  'Ctrl-C reaches spin inside C4KE, and C4KE\'s shell carries on', screen());
type('\\q\n');
check(until(() => has(/^c4ix:\/usr\$/) && inputLine() === '', 4000), '\\q leaves C4KE for C4IX\'s shell', screen());
type('cd /\n');
until(() => has(/^c4ix:\/\$/));

// ---- raycast in the Terminal: 256 colours ---------------------------------------------
type('raycast.c4r 15x15 -s 3 -d -n 30 -g 80x25\n');
check(until(() => new Set(rects.map(r => r.rgb)).size > 8, 1500), `raycast draws in colour in the Terminal (${new Set(rects.map(r => r.rgb)).size} colours)`);
until(() => has(/^c4ix:\/\$/), 1500);
key('l', 2, 76);                                               // Ctrl-L: a clean screen for what follows

// ---- compile a windowed program inside the machine, and run it ------------------------------
type('c4cc -o /tmp/bounce.c4r /usr/include/window.h /usr/include/window.c /usr/src/c4ix/examples/bounce.c\n');
check(until(() => has(/c4cc: wrote \d+ bytes to ramfs:\/tmp\/bounce.c4r/), 3000), 'c4cc compiles bounce.c inside C4IX', screen());
type('/tmp/bounce.c4r\n');
check(until(() => has(/Bounce - compiled inside C4IX/) && has('Click me') && has('Button clicks:'), 1500),
  'the program makes its Terminal its window, titled and drawn', screen());
const sameRow = (label, value) => { const l = find(label)[0]; return !!l && frame.some(t => t.s === value && Math.abs(t.y - l.y) < 3 && t.x > l.x); };
clickText('Click me');
check(until(() => sameRow('Button clicks:', '1'), 400), 'clicking its button counts', screen());
key('x');
check(until(() => sameRow('Last key:', 'x'), 400), 'a key typed in the window reaches it');
key('q');
check(until(() => has(/bounce: \d+ frames, 1 clicks/) && !has('Click me'), 800), 'q ends it and the window is a terminal again', screen());
check(has(/^Terminal - task \d+/), 'with its own title back');

// ---- Files ---------------------------------------------------------------------------
start('Files');
check(until(() => has('Files - /')), 'Go > Files opens at the root', screen());
check(['bin', 'tmp', 'usr', 'build'].every(has) && has('Folder') && has('C4IX (/)'), 'it lists the root folders and shows the tree', screen());
// The folder tree: the root archive's folders are more than its pane
// shows. It scrolls (the wheel here), and a folder collapses and expands
// (the keyboard, then its +/- box).
const inTree = all => {
  const e = explorer();
  if (!e) return undefined;
  return all.filter(t => t.x > e.x && t.x < e.x + 190 && t.y > e.y + 50 && t.y < e.y + 400).pop();
};
{
  const e = explorer();
  for (let i = 0; i < 40; i++) ev({ kind: 'wheel', x: e.x + 60, y: e.y + 200, dy: 1 });
  check(until(() => !inTree(find('C4IX (/)'))), 'the wheel scrolls the folder tree past its top', screen());
  for (let i = 0; i < 40; i++) ev({ kind: 'wheel', x: e.x + 60, y: e.y + 200, dy: -1 });
  check(until(() => !!inTree(find('C4IX (/)'))), 'and back', screen());
  check(dragThumb(r => r.x < e.x + 200, 120) && until(() => !inTree(find('C4IX (/)'))), "dragging the tree's scroll thumb scrolls it", screen());
  for (let i = 0; i < 40; i++) ev({ kind: 'wheel', x: e.x + 60, y: e.y + 200, dy: -1 });
  until(() => !!inTree(find('C4IX (/)')));
  // the deepest folders are wider than the tree: a bar across, whose right
  // arrow moves every name left
  const hbar = rects.filter(r => r.h === 16 && r.w > 60 && r.rgb === 0xe0e0e0 && r.x < e.x + 200 && r.y > e.y + 60).pop();
  const rootX = inTree(find('C4IX (/)')).x;
  if (hbar) for (let i = 0; i < 3; i++) click(hbar.x + hbar.w - 8, hbar.y + 8);
  check(!!hbar && until(() => !!inTree(find('C4IX (/)')) && inTree(find('C4IX (/)')).x < rootX), 'the tree scrolls across too', screen());
  if (hbar) for (let i = 0; i < 3; i++) click(hbar.x + 8, hbar.y + 8);
}
key('TAB'); key('', 0, 36); key('', 0, 37);               // the tree; Home; Left
check(until(() => !!inTree(find('C4IX (/)')) && !inTree(find('usr')) && !!inList(find('usr'))),
  'Left collapses the root in the tree, and the list still shows it', screen());
key('F5');
check(until(() => !inTree(find('usr')) && has('Files - /')), 'it stays collapsed through a refresh', screen());
key('', 0, 39);                                           // Right
check(until(() => !!inTree(find('usr')) && !!inTree(find('src'))), 'Right expands it again', screen());
{
  const u = inTree(find('usr'));
  click(u.x - 29, u.y + 6);                               // its +/- box, left of the icon
  check(until(() => !!inTree(find('usr')) && !inTree(find('src'))), "clicking /usr's box collapses it", screen());
  click(u.x - 29, u.y + 6);
  check(until(() => !!inTree(find('src'))), 'and again expands it', screen());
}
key('TAB');                                               // the list has the keyboard again
dbl('usr', inList);
check(until(() => has('Files - /usr') && has('src')), 'double-clicking a folder opens it', screen());
key('\b');
check(until(() => has('Files - /')), 'Backspace goes up a level');
dbl('bin', inList);
// b4ke, because it sorts first: /bin holds more programs than the list
// shows at once, and one off the bottom cannot be double-clicked
check(until(() => has('Files - /bin') && !!inList(find('b4ke')) && find('C4IX Program').length >= 10),
  '/bin lists its programs as C4IX Programs, with no .c4r in their names', screen());
dbl('b4ke', inList);
check(until(() => has(/b4ke: cannot read BUILD\.B4K/), 800), 'double-clicking one runs it in a Terminal', screen());
clickText(/^Files - /); key('\b');
check(until(() => has('Files - /')), 'and back up to the root');
// a library is C4R inside, but not a program: it says so rather than run
dbl('lib', inList);
check(until(() => has('Files - /lib') && !!inList(find('libc.c4s')) && has('Shared Library')),
  '/lib lists its libraries as Shared Libraries', screen());
dbl('libc.c4s', inList);
check(until(() => has('Object files and libraries are linked into programs, not run.')),
  'double-clicking one says it does not run, rather than running it', screen());
key('\n');
check(until(() => noDialog()) && !has(/missing|unresolved|cannot resolve/i), 'OK dismisses that, and nothing ran', screen());
key('\b');
check(until(() => has('Files - /')), 'and up to the root again');
dbl('tmp', inList);
check(until(() => has('Files - /tmp')), 'into /tmp');
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
check(until(() => has(/Delete 'docs'/)), 'Delete asks first');
key('y');
check(until(() => noDialog() && !inList(find('docs'))), 'and Yes deletes it');
// Several at once, and a folder with something in it: Ctrl-click takes a
// second item (Ctrl is a key the page sends, wm_mods), and Delete takes
// both, and what is inside
const newdir = name => {
  clickText('File'); settle(); clickText('New Folder');
  until(() => has('Name of the new folder:'));
  for (let i = 0; i < 12; i++) key('\b');
  type(name); key('\n');
  return until(() => noDialog() && !!inList(find(name)));
};
check(newdir('aa') && newdir('bb'), 'two new folders', screen());
dbl('aa', inList);
check(until(() => has('Files - /tmp/aa')) && newdir('inner'), 'and one inside the first', screen());
key('\b');
check(until(() => has('Files - /tmp')), 'back in /tmp');
clickText('aa', inList);
ev({ kind: 'keydown', code: 17, char: 0, mods: 2 });
clickText('bb', inList);
ev({ kind: 'keyup', code: 17, char: 0, mods: 0 });
check(until(() => has(/2 selected/)), 'Ctrl-click selects a second item', screen());
key('DEL');
check(until(() => has(/Delete these 2 items/)), 'Delete asks about both', screen());
key('y');
check(until(() => noDialog() && !inList(find('aa')) && !inList(find('bb'))), 'and Yes deletes both, the folder inside too', screen());

// ---- Files: the uploads in /home, and the list's order ------------------------------
// /home has the three uploads and the folder made above. Their order in
// the list, top to bottom:
const homeNames = ['zeta', ...UPLOADS.map(([n]) => n)];
const order = () => homeNames.every(n => inList(find(n)))
  ? homeNames.slice().sort((a, b) => inList(find(a)).y - inList(find(b)).y).join(' ') : screen();
const viewMenu = item => { clickText('View'); settle(); return clickText(item); };
key('\b');
until(() => has('Files - /'));
dbl('home', inList);
check(until(() => has('Files - /home') && order() === 'zeta alpha.txt bravo.dat charlie.c'),
  '/home lists the uploads by name, after the folder', order());
check(!!inList(find('1 KB')) && !!inList(find('3 KB')) && !!inList(find('9 KB')), 'with their sizes', screen());
viewMenu('Sort by Size');
check(until(() => order() === 'zeta bravo.dat alpha.txt charlie.c'), 'View > Sort by Size: the smallest file first, the folder still first', order());
viewMenu('Descending');
check(until(() => order() === 'zeta charlie.c alpha.txt bravo.dat'), 'View > Descending: the largest first, the folder still first', order());
viewMenu('Sort by Type');
check(until(() => order() === 'zeta alpha.txt bravo.dat charlie.c'), 'Sort by Type keeps descending: by extension, txt dat c', order());
viewMenu('Ascending');
check(until(() => order() === 'zeta charlie.c bravo.dat alpha.txt'), 'and Ascending turns it round', order());
viewMenu('Sort by Date');
check(until(() => order() === 'zeta bravo.dat charlie.c alpha.txt'), 'Sort by Date: in the order they were made', order());
clickText('Name', inFiles);
check(until(() => order() === 'zeta alpha.txt bravo.dat charlie.c'), "clicking the Name column's header sorts by name", order());
clickText('Name', inFiles);
check(until(() => order() === 'zeta charlie.c bravo.dat alpha.txt'), 'and clicking it again reverses it', order());
clickText('Size', inFiles);
check(until(() => order() === 'zeta bravo.dat alpha.txt charlie.c'), 'clicking Size sorts by size, smallest first', order());
// the order is kept with Settings' choices: a new Files window has it
clickText('File', all => all.filter(t => inFiles([t])).sort((p, q) => p.y - q.y)[0]); settle(); clickText('Close');
check(until(() => !explorer()), 'File > Close closes Files', screen());
start('Files');
until(() => has('Files - /'));
dbl('home', inList);
check(until(() => has('Files - /home') && order() === 'zeta bravo.dat alpha.txt charlie.c'), 'and Files opened again keeps the order', order());
clickText('Name', inFiles);
check(until(() => order() === 'zeta alpha.txt bravo.dat charlie.c'), 'by name again', order());
key('\b');
until(() => has('Files - /'));
dbl('tmp', inList);
check(until(() => has('Files - /tmp')), 'and back in /tmp');

// ---- Text Editor --------------------------------------------------------------------------
start('Text Editor');
check(until(() => has('Untitled - Text Editor')), 'Go > Text Editor opens a new document');
type('hello from c4ix\n'); type('second line');
check(until(() => has('hello from c4ix') && has('second line') && has('Untitled * - Text Editor')), 'typing shows, and the title marks it modified', screen());
key('s', 2, 83);
check(until(() => has('Save as:')), 'Ctrl+S on a new file asks where');
key('\n');
check(until(() => has('untitled.txt - Text Editor')), 'and saves it to /tmp/untitled.txt', screen());
clickText(/^Files - /); key('F5');
check(until(() => !!inList(find('untitled.txt')) && has('Text')), 'Files lists the saved file after a refresh', screen());
const notes = find('untitled.txt - Text Editor').length;
dbl('untitled.txt', inList);
check(until(() => find('untitled.txt - Text Editor').length > notes), 'double-clicking it opens it in Text Editor');
check(until(() => find('hello from c4ix').length >= 2), 'with what was saved in it');

// ---- Calculator ------------------------------------------------------------------------
start('Calculator');
check(until(() => has('Calculator') && has('Hex') && has('Dec')), 'Go > Calculator opens');
type('12+30=');
check(until(() => has('42')), '12 + 30 = 42', screen().slice(-300));
clickText('Hex', 'last', -8, 5);
check(until(() => has('2A')), 'switched to Hex it shows 2A');
type('*2=');
check(until(() => has('54')), '2A * 2 = 54 in hex');
// Single and Double: IEEE arithmetic in that precision, the shortest
// decimal that reads back, and the bits. Printable keys go in with
// keyCode 0 here, or '.' would be Delete.
const ftype = s => { for (const ch of s) key(ch, 0, ch === '=' ? 13 : 0); };
clickText('Double', 'last', -8, 5);
check(until(() => has('IEEE double, bits:')), 'Double mode shows the bits in place of the radix', screen().slice(-300));
key('ESC'); ftype('0.1+0.2=');
check(until(() => has('0.30000000000000004') && has('3FD33333 33333334')), '0.1 + 0.2 = 0.30000000000000004 in Double', screen().slice(-300));
clickText('Single', 'last', -8, 5);
check(until(() => has('IEEE single, bits:') && has('3E99999A')), 'switched to Single it is the float nearest, 0.3', screen().slice(-300));
key('ESC'); ftype('16777216+1=');
check(until(() => has('16777216') && has('4B800000')), '2^24 + 1 is 2^24 in Single', screen().slice(-300));
clickText('Double', 'last', -8, 5);
key('ESC'); ftype('16777216+1=');
check(until(() => has('16777217')), 'and 16777217 in Double', screen().slice(-300));
clickText('Single', 'last', -8, 5);
key('ESC'); clickText('Max'); ftype('*2=');
check(until(() => has('inf') && has('7F800000')), 'Max * 2 overflows to inf in Single', screen().slice(-300));
clickText('Integer', 'last', -8, 5);
check(until(() => has('Hex') && has('Dec') && !has('IEEE single, bits:')), 'and back to Integer', screen().slice(-300));
clickText('Dec', 'last', -8, 5);
key('ESC'); type('2+3==');
check(until(() => has('8')), '= again repeats the last operation: 2 + 3 = = is 8', screen().slice(-300));
type('=');
check(until(() => has('11')), 'and again, 11', screen().slice(-300));
type('10=');
check(until(() => has('13')), 'a new number then = adds 3 to it', screen().slice(-300));
clickText('Double', 'last', -8, 5);
key('ESC'); ftype('10*0.5==');
check(until(() => has('2.5')), 'in Double too: 10 * 0.5 = = is 2.5', screen().slice(-300));
clickText('Integer', 'last', -8, 5);

// ---- Editor -------------------------------------------------------------------------------
// Printable keys go in with keyCode 0: this harness would otherwise make
// '(' a Down arrow and '.' Delete (a browser sends their own keyCodes).
const txt = s => { for (const ch of s) key(ch, 0, ch === '\n' ? 13 : 0); };
const edLine = s => rows().some(t => t.s.trimEnd().endsWith(s));
start('Editor');
check(until(() => has('Untitled - Editor')), 'Go > Editor opens a new file in VIM mode', screen());
txt('i');
check(until(() => has('-- INSERT --')), 'i enters Insert mode');
txt('int main() {\n    return 4;\n}');
key('ESC');
check(until(() => edLine('int main() {') && edLine('return 4;') && !has('-- INSERT --')), 'typing shows, keeping the indent, and Esc leaves Insert', screen());
txt(':w /tmp/t.c\n');
check(until(() => has(/"\/tmp\/t\.c" written, 3 lines/) && has('t.c - Editor')), ':w writes the file', screen());
txt('2Gf4r7');
check(until(() => edLine('    return 7;')), '2G f4 r7 changes the number in place', screen());
txt('ggyyp');
const mains = () => rows().filter(t => /int main\(\) \{\s*$/.test(t.s)).length;
check(until(() => mains() === 2), 'yyp copies a line', screen());
txt('dd');
check(until(() => mains() === 1), 'dd deletes it');
txt('u');
check(until(() => mains() === 2), 'u undoes that', screen());
txt('dd');
key('UP'); key('DOWN');
txt('A');
key('LEFT', 0, 37); txt('0');
key('ESC');
check(until(() => edLine('    return 70;')), 'arrow keys move in Insert mode', screen());
txt(':make\n');
check(until(() => has(/^Build: finished/), 4000), ':make builds it with c4sc and c4rlink', screen());
txt('ggOint x = ;');
key('ESC');
txt(':make\n');
check(until(() => has(/^Build: failed - 1 error/), 4000), 'a compile error fails the build and is listed', screen());
check(until(() => has(/\(1 of 1\)/)), 'and the editor goes to it', screen());
txt('dd');
txt('jwciwexit');
key('ESC');
check(until(() => edLine('    exit 70;')), 'ciw changes a word', screen());
txt('w.');
check(until(() => edLine('    exit exit;')), '. repeats the change', screen());
txt('/main\n');
txt('Vjd');
check(until(() => !edLine('int main() {') && !edLine('exit exit;') && edLine('}')), '/ finds, and V j d deletes the lines', screen());
txt(':set novim\n');
check(until(() => has(/^VIM mode off|^-- EDIT --/)), ':set novim makes a plain editor', screen());
txt('abc');
check(until(() => edLine('abc}')), 'that types at once', screen());
key('ESC'); txt('set vim\n');
check(until(() => !has(/^-- EDIT --/)), 'and Esc then set vim brings VIM back', screen());
txt(':q!\n');
check(until(() => !has(/t\.c \* - Editor|t\.c - Editor/)), ':q! closes the window');

// ---- the IDE: its desktop icon, its file pane, colours, its scroll bar -----------------------
dbl('IDE');
check(until(() => has('Untitled - Editor') && has('Files')), 'the IDE icon opens the Editor, with a file pane', screen());
// The Editor window, by its title; what is drawn after the title in the
// frame is that window's (it is on top), so other windows' text at the
// same place does not count.
const edTitle = () => frame.findIndex(t => / - Editor$/.test(t.s));
const ed = () => { const i = edTitle(); return i >= 0 ? { x: frame[i].x - 27, y: frame[i].y - 7, i } : null; };
const inPane = all => { const e = ed(); return e ? all.filter(t => frame.indexOf(t) > e.i && t.x > e.x && t.x < e.x + 184 && t.y > e.y + 60).pop() : undefined; };
check(!!inPane(find('usr')) && !!inPane(find('bin')) && !inPane(find('src')), 'the pane lists the top of the disk, its folders closed', screen());
clickText('usr', inPane);
check(until(() => !!inPane(find('src'))), 'a click opens a folder', screen());
clickText('src', inPane);
check(until(() => !!inPane(find('c4ix'))), 'and one inside it');
clickText('c4ix', inPane);
until(() => !!inPane(find('examples')));
clickText('examples', inPane);
// The tree is taller than the pane by now (examples' files come last,
// below the bottom row), so a turn of the wheel brings them up.
{ const e = ed(); ev({ kind: 'wheel', x: e.x + 60, y: e.y + 200, dy: 1 }); }
check(until(() => !!inPane(find('hello.c'))), 'which lists its files', screen());
clickText('hello.c', inPane);
check(until(() => has('hello.c - Editor') && edLine('int main(int argc, char **argv) {')), 'a click on a file opens it in the editor', screen());
check(frame.some(t => t.s === 'int' && t.rgb === 0x007878) && frame.some(t => t.s === 'return' && t.rgb === 0x0000b0) &&
  frame.some(t => /^\/\/ hello\.c/.test(t.s) && t.rgb === 0x2a8a2a) && frame.some(t => /^"hello from/.test(t.s) && t.rgb === 0xa01818),
  'a .c file is coloured: types, keywords, comments and strings', frame.filter(t => t.adv === 7).map(t => `${t.s}:${t.rgb.toString(16)}`).slice(0, 40).join(' '));
txt(':syntax off\n');
check(until(() => edLine('int main(int argc, char **argv) {') && !frame.some(t => t.s === 'int' && t.rgb === 0x007878)), ':syntax off draws it plain', screen());
txt(':syntax on\n');
key('b', 2, 66);
check(until(() => !has('Files') && has('hello.c - Editor')), 'Ctrl+B hides the file pane', screen());
key('b', 2, 66);
check(until(() => has('Files') && !!inPane(find('hello.c'))), 'and shows it again, still open at the file', screen());
txt(':e /usr/src/c4ix/user/desktop/edit.c\n');
check(until(() => has('edit.c - Editor') && !!inPane(find('edit.c'))), ':e opens a file, and the pane opens the folders to it', screen());
{
  const e = ed();
  const firstLine = () => Math.min(...frame.filter(t => t.adv === 7 && /^\d+$/.test(t.s.trim()) && t.x > e.x + 184 && t.x < e.x + 250).map(t => +t.s.trim()));
  check(firstLine() === 1 && dragThumb(r => r.x > e.x + 400, 150) && until(() => firstLine() > 100),
    "dragging the editor's scroll thumb scrolls the text", `first line ${firstLine()}`);
}
// a long line: off to the right, then the bar across, then folded by wrap
txt(':e /tmp/long.txt\n');
until(() => has('long.txt - Editor'));
txt('i' + 'W'.repeat(110) + 'END');
key('ESC'); txt('0');
check(until(() => rows().some(t => /^W{40,}$/.test(t.s.trim())) && !edLine('WEND')), 'a long line shows its start, the rest off to the right', screen());
{
  const e = ed();
  const hb = rects.filter(r => r.h === 16 && r.rgb === 0xe0e0e0 && r.x > e.x + 150 && r.w > 200).pop();
  if (hb) for (let i = 0; i < 4; i++) click(hb.x + hb.w - 30, hb.y + 8);   // the track, right of the thumb: a page each
  check(!!hb && until(() => edLine('WEND')), "the editor's bar across scrolls to the end of it", screen());
}
txt(':set wrap\n');
check(until(() => edLine('WEND') && rows().filter(t => /^W{20,}/.test(t.s.trim())).length >= 2), ':set wrap folds it onto the next row', screen());
txt(':set nowrap\n');
txt(':e! /usr/src/c4ix/user/desktop/edit.c\n');
until(() => has('edit.c - Editor'));
// a build that takes a while, cancelled from the pane's button. /build
// arrives built, so it is an edit that makes one: edit.c's object, again
txt('Go// a change, so there is something to build'); key('ESC');
txt(':make\n');
check(until(() => has('Build: running') && has('Cancel'), 400), ':make on a file under a BUILD.B4K runs b4ke there', screen());
clickText('Cancel');
check(until(() => has('Build: cancelled') && !has('Build: running'), 400), 'Cancel stops the build', screen());
// src/tests has no BUILD.B4K, and /usr's builds the tree, not
// mandel.c: so it is built alone, by cc, with the repo's headers
txt(':e! /usr/src/tests/mandel.c\n');
until(() => has('mandel.c - Editor'));
txt(':make\n');
check(until(() => has(/^Build: finished/) && has(/^c4rlink: wrote \d+ bytes to ramfs:\/tmp\/mandel\.c4r/), 4000),
  ':make on a file no BUILD.B4K names builds it alone, with cc', frame.filter(t => /Build|mandel|cc|b4ke|c4lc|cannot|rror/.test(t.s)).map(t => t.s.trim()).join('\n'));
{
  // the pointer: a text beam over the text, an arrow on the taskbar
  const e = ed();
  ev({ kind: 'move', x: e.x + 400, y: e.y + 150, buttons: 0 });
  check(until(() => pointer === 1), 'the pointer is a text beam over the text', `pointer ${pointer}`);
  ev({ kind: 'move', x: 400, y: SH - 10, buttons: 0 });
  check(until(() => pointer === 0), 'and an arrow over the taskbar', `pointer ${pointer}`);
}
txt(':q\n');
check(until(() => !has('edit.c - Editor')), 'and :q closes it');

// ---- System Monitor ------------------------------------------------------------------------
clickText(/^Terminal - task/);
type('spin 5000\n');
until(() => has(/spin: tick 1\//), 1500);
key('ESC', 3, 27);                                             // Ctrl+Shift+Esc
check(until(() => has('System Monitor') && has('Name')), 'Ctrl+Shift+Esc opens System Monitor on Processes', screen());
check(until(() => has(/^(c4ix-)?desktop/) && has(/^(c4ix-spin\.c4r|spin) *$/) && has(/^CPU Usage: \d+%$/), 600), 'it lists the tasks, spin among them, with CPU usage', frame.filter(t => /c4ix|CPU|Image|boot|init|spin/.test(t.s)).map(t => t.s.trim()).join(' | '));
check(until(() => has('Memory') && has(/^[\d,]+ K$/)), 'each task shows the memory it holds');
check(has('idle') && has(/^System *$/), 'and, as NT does, idle and System', frame.filter(t => /System/.test(t.s)).map(t => t.s.trim()).join(' | '));
check(until(() => has('Interval') && frame.some(t => /^[0-9,]+\.[0-9]M$/.test(t.s.trim())), 600), 'its Interval column shows what a task ran in the last second, spin in millions', frame.filter(t => /[0-9]M$|[0-9]k$|Interval/.test(t.s)).map(t => t.s.trim()).join(' | '));
check(!has(/^(c4sc|b4ke)(\.c4r)? *$/), 'the cancelled build left nothing behind', frame.filter(t => /c4sc|b4ke/.test(t.s)).map(t => t.s).join(' | '));
clickText(/^(c4ix-spin\.c4r|spin) *$/);
clickText('Kill');
check(until(() => has(/Kill task (c4ix-)?spin/)), 'Kill asks first');
key('y');
check(until(() => !has(/^(c4ix-spin\.c4r|spin) *$/), 600), 'and ends it', screen());
clickText('Load');
check(until(() => has('CPU Usage History') && has('Totals')), 'the Performance tab shows the gauge and the history');
// and the memory: the heap's use, free and size, and its history; the
// status bar says it on every page
check(until(() => has('Memory Usage History') && has('In use (MB)') && has('Free (MB)') && has(/^Memory: [\d.]+ MB of [\d.]+ MB$/)),
      'it shows the memory in use and free', frame.filter(t => /Memory|MB/.test(t.s)).map(t => t.s.trim()).join(' | '));
clickText('Programs');
check(until(() => has('Status') && find('Running').length >= 3), 'the Applications tab lists the windows');

// ---- Settings ---------------------------------------------------------------------------------
start('Settings');
check(until(() => has('Color scheme') && has('Classic Teal') && has('Granite') && has('Defaults')), 'Go > Settings lists the colour schemes', screen());
clickText('Granite');
check(until(() => rects.some(r => r.rgb === 0xb8c4cc) && !rects.some(r => r.rgb === 0xc0c0c0 && r.w > 300)), 'choosing Granite repaints the desktop in it', screen());
key('ESC');
check(until(() => !has('Color scheme')), 'Esc closes Settings');
clickText(/^Terminal - task/);
type('cat /etc/desktop.cfg\n');
check(until(() => has(/^scheme 1 bg -1/)), 'the choice is kept in /etc/desktop.cfg', screen());
start('Settings');
clickText('Defaults');
check(until(() => rects.some(r => r.rgb === 0xeceff4 && r.w > 300) && !rects.some(r => r.rgb === 0xb8c4cc)), 'Defaults brings the Modern scheme back', screen());
key('ESC');
until(() => !has('Color scheme'));

// ---- the look: Flat (the default) or Classic ----------------------------------------------------
// Flat outlines the taskbar along its top in the shadow; Classic lights a line one down
const taskLine = dy => rects.some(r => r.w === SW && r.h === 1 && r.y === SH - TASK_H + dy);
check(taskLine(0) && !taskLine(1), 'the desktop starts in the Flat look', screen());
start('Settings');
until(() => has('Look'));
clickText('Classic', 'last', -8, 5);
check(until(() => taskLine(1)), 'Settings > Look > Classic bevels it', screen());
clickText('Flat', 'last', -8, 5);
check(until(() => taskLine(0) && !taskLine(1)), 'and Flat flattens it again', screen());
key('ESC');
until(() => !has('Look'));

// ---- the picture, and the screen savers ------------------------------------------------------
check(imgdefs === 64 && imgs >= 1, 'the desktop\'s picture went to the display once, as 64 tiles, and is drawn', `imgdefs ${imgdefs} imgs ${imgs}`);
start('Settings');
check(until(() => has('Picture: C4IX') && has('Screen saver') && has('Starfield') && has('Digital Rain') && has('5 min')),
  'Settings has the picture and the screen savers', screen());
clickText('The scheme\'s colour', 'last', -8, 5);
check(until(() => imgs === 0), 'the scheme\'s colour turns the picture off', `imgs ${imgs}`);
clickText('Picture: C4IX', 'last', -8, 5);
check(until(() => imgs > 0), 'and Picture turns it back on', `imgs ${imgs}`);
clickText('Polygons', 'last', -8, 5);
clickText('-');
check(until(() => has('4 min')), 'the wait goes down a minute', screen());
clickText('Preview');
{
  const p = presents;
  check(until(() => !has('Go') && presents > p + 15), 'Preview runs the screen saver over everything', screen());
}
key('a');
check(until(() => has('Go') && has('Screen saver')), 'a key ends it and the desktop comes back as it was', screen());
key('ESC');
until(() => !has('Screen saver'));
// a Terminal's title, not its taskbar button (drawn after the windows)
clickText(/^Terminal - task/, 'first');
type('cat /etc/desktop.cfg\n');
check(until(() => has(/wall 1 saver 2 wait 4 style 0/), 800), 'the picture and the saver are kept in /etc/desktop.cfg',
  frame.filter(t => /scheme|saver|cat /.test(t.s)).map(t => t.s.trimEnd()).join(' | '));
// and no saver again, or the long waits below would start one over the clicks they make
start('Settings');
until(() => has('Screen saver'));
clickText('(None)', 'last', -8, 5);
check(until(() => frame.some(t => t.s.trimEnd() === '(None)')), 'and (None) turns the screen saver off');
key('ESC');
until(() => !has('Screen saver'));

// ---- Wolfenstein 3D: a game in a Terminal, its frames 8-bit (GUI_BLIT8) -----------------------
// (--window: by itself it takes the whole display, as gui does below)
// Only where the game is on the disk: it is an optional submodule
// (games/README.md), and C4IX's manifest lists it when it is there.
if (/^\s*wolf3d: ilink/m.test(readFileSync(join(ROOT, 'disk/c4ix.vfs.txt'), 'utf8'))) {
  clickText(/^Terminal - task/, 'first');
  type('wolf3d --window\n');
  check(until(() => has(/Unpacking the shareware episode/), 1500), 'wolf3d unpacks the shareware episode the first time', screen());
  check(until(() => rgbFrames > 0 && has('Wolfenstein 3D'), 3000), 'and shows its frames in the window, which it titles',
    `frames ${rgbFrames} ` + screen().slice(-300));
  check(rgbLast && rgbLast.w === 320 && rgbLast.h === 200 && rgbLast.dw === 640 && rgbLast.dh === 400,
    '320x200 in 8 bits, scaled to the canvas', JSON.stringify(rgbLast));
  {
    // the sign-on screen waits for a key; then the title, a new picture
    const before = rgbLast.sum, n = rgbFrames;
    key(' ', 0, 32);
    check(until(() => rgbFrames > n + 2 && rgbLast.sum !== before, 1500), 'a key goes to the game, which moves on', `frames ${rgbFrames}`);
  }
  key('c', 2, 67);
  check(until(() => has(/^Terminal - task/), 800), 'Ctrl-C stops it, and the window is a Terminal again', screen().slice(-300));
} else console.log('test-desktop: Wolfenstein 3D is not on the disk (games/README.md): its checks skipped');

// ---- the whole display: a program asks, the desktop gives way, and has it back ----------------
// (include/display.h) gui, the display's demo, for 40 frames: the display is
// its own, at its size, and once it ends the desktop draws everything again,
// its Terminal showing what the demo printed meanwhile
clickText(/^Terminal - task/, 'first');
type('gui 40\n');
{
  // the demo's text is GUI_TEXT, which the screen here does not read: its frames are counted
  const ok = until(() => gui.w === 640 && gui.h === 480, 1500), p = presents;
  check(ok && until(() => presents >= p + 10 || gui.w !== 640, 1500) && presents >= p + 10,
    'a program on the whole display: the desktop gives way', `${gui.w}x${gui.h}, ${presents - p} frames`);
}
check(until(() => gui.w === SW && has(/^Terminal - task/) && has(/gui: done after 40 frames/), 1500),
  'and has it back when the program ends, and draws it all again', `${gui.w}x${gui.h} ` + screen().slice(-300));

// ---- the games in the Go menu: each where it is on the disk ------------------------------------
// (games/README.md: optional submodules; C4IX's manifest lists the ones there)
{
  const vfs = readFileSync(join(ROOT, 'disk/c4ix.vfs.txt'), 'utf8');
  const games = [['wolf3d', 'Wolfenstein 3D'], ['doom', 'Doom'], ['tyrian', 'Tyrian'], ['rott', 'Rise of the Triad']];
  click(20, SH - TASK_H + 12);
  until(() => has('Power Off...'));
  const wrong = games.filter(([g, label]) => new RegExp(`^\\s*${g}: ilink`, 'm').test(vfs) !== frame.some(t => t.s.trimEnd() === label));
  check(wrong.length === 0, 'the Go menu lists the games that are on the disk, and only those',
    wrong.map(([g]) => g).join(' ') || 'all as the manifest says');
  key('ESC');
  until(() => !has('Power Off...'));
}

// ---- Run --------------------------------------------------------------------------------------
start('Run...');
check(until(() => has(/Program to run/)), 'Go > Run asks for a program');
type('echo from-run'); key('\n');
check(until(() => has('from-run'), 800), 'and runs it in a new Terminal', screen());

// ---- as many windows as memory allows (there were 8 slots) --------------------------------------
// System Monitor's Performance tab (the CPU meter opens it) counts the windows open
const opened = () => { const l = frame.find(t => t.s.trim() === 'Open windows'); const v = l && frame.find(t => t.y === l.y && /^\d+$/.test(t.s.trim())); return v ? +v.s.trim() : 0; };
click(SW - 100, SH - TASK_H + 12);
until(() => opened() > 0);
const nopen = opened();
for (let i = 0; i < 10; i++) { start('Calculator'); until(() => has('Hex')); }
click(SW - 100, SH - TASK_H + 12);
check(until(() => opened() === nopen + 10) && opened() > 8, 'ten more windows open, more than the old eight', `${nopen} then ${opened()}`);

// ---- the screen's size, at run time: 640x480 and back (the window kit's relayout) ---------------
// The page resizes the display, as it does when a size a program asked
// for comes back (GUI_EV_RESIZE): a maximised window fills the new screen,
// the rest are pulled back onto it, the taskbar runs along its new bottom
// and the picture is stretched to it again.
const filling = (w, h) => rects.some(r => r.x === 0 && r.y === 0 && r.w === w && r.h === h);
start('Files');
until(() => has('Files - /'));
dbl('Files - /', all => all.filter(t => t.y < SH - TASK_H).pop());
check(until(() => filling(SW, SH - TASK_H)), 'a double click on its title maximises Files', screen());
{
  const defs = imgdefs;
  gui.event({ kind: 'resize', w: 640, h: 480 }); SW = 640; SH = 480;
  run(3e5);
  check(until(() => taskLine(0) && filling(640, 450)), 'told 640x480, the taskbar moves to its bottom and the maximised window fills what is above it', screen().slice(-400));
  check(frame.every(t => t.x >= 0 && t.x < SW && t.y >= 0 && t.y < SH), 'every window is pulled back onto the screen',
    frame.filter(t => t.x >= SW || t.y >= SH || t.x < 0 || t.y < 0).map(t => `${t.s}@${t.x},${t.y}`).join(' | '));
  check(imgdefs === defs + 64, 'and the picture is stretched to it again', `imgdefs ${defs} then ${imgdefs}`);
}
start('Calculator');
check(until(() => has('Hex')) && find('Power Off...').length === 0, 'the Go menu opens from the new taskbar, and Calculator from it');
type('123*456=');
check(until(() => has('56088')), 'which still works there: 123 * 456 = 56088', screen().slice(-300));
gui.event({ kind: 'resize', w: 800, h: 600 }); SW = 800; SH = 600;
run(3e5);
check(until(() => taskLine(0) && filling(800, 570) && !filling(640, 450)), 'back at 800x600, the taskbar and the maximised window follow', screen().slice(-400));

// ---- the Windows 3.1 look (look31.c), from Settings, kept across a restart ---------------------
// The control-menu box is a white bar 11 by 3 on the title bar's left;
// the title is centred between it and the minimise and maximise buttons.
const sysbox = () => rects.some(r => r.w === 11 && r.h === 3 && r.rgb === 0xffffff);
check(!sysbox(), 'the Flat look has no control-menu box');
start('Settings');
until(() => has('Look'));
clickText('Windows 3.1', 'last', -8, 5);
check(until(() => sysbox()), 'Settings > Look > Windows 3.1 draws the control-menu box', screen());
{
  const r = rects.filter(q => q.w === 440 && q.h === 430).pop();
  const t = r && frame.filter(s => s.s.trimEnd() === 'Settings' && s.y > r.y && s.y < r.y + 22).pop();
  const off = r && t ? Math.round(t.x + 'Settings'.length * 7 / 2 - (r.x + r.w / 2)) : 999;
  check(Math.abs(off) <= 20, `and a title centred in its bar (${off} px from the middle; the Flat look's is ${27 + 28 - 220})`, JSON.stringify({ r, t }));
}
key('ESC');
until(() => !has('Look'));

// ---- Power Off --------------------------------------------------------------------------------
start('Power Off...');
check(until(() => consoleText().includes('desktop: shut down'), 2000), 'Power Off ends the desktop', consoleText().slice(-500) + ' / screen: ' + screen().slice(-400));
typeBytes(vm, new TextEncoder().encode('ps\n'));
check(until(() => /tasks,/.test(consoleText().split('desktop: shut down').pop())), 'and the console shell carries on');
const psOut = consoleText().split('desktop: shut down').pop();
check(!/c4ix-sh.c4r[\s\S]*c4ix-sh.c4r/.test(psOut.replace(/^[\s\S]*?ID/, '')), 'no Terminal shell outlived the desktop', psOut);

// ---- the desktop again: in the look it was left in --------------------------------------------
{
  const c0 = consoleText().length;
  typeBytes(vm, new TextEncoder().encode('cat /etc/desktop.cfg\n'));
  check(until(() => / style 2\b/.test(consoleText().slice(c0))), 'the look is kept in /etc/desktop.cfg', consoleText().slice(c0));
}
typeBytes(vm, new TextEncoder().encode('desktop\n'));
check(until(() => /desktop: running on the display/.test(consoleText().split('desktop: shut down').pop())), 'the desktop starts again');
check(until(() => has(/^Terminal - task/) && sysbox(), 800), 'in the Windows 3.1 look, as it was left', screen());
start('Power Off...');
check(until(() => consoleText().split('desktop: shut down').length > 2, 2000), 'and Power Off ends it again');
process.exit(fail);
