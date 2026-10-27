// test-c4dos-ladder.mjs - the Build Ladder, headless: C4DOS builds C4KE,
// C4KE builds C4IX, and each one boots and plays a game, in one machine
// session with no reset (docs/c4dos-ladder.md).
//
// Boots c4dos32.c4r on the page's ladder disk (libjs/images/ladder, made
// by build-images.sh) as ?system=c4dos-ladder does: 128 MB, 400 MHz, a
// display fitted. Then, typing as a person would and waiting for each
// prompt:
//
//   A:\>    DOOM, quit (F10, Y); HEAP_USED back where it was
//           BUILD; RUN dosload.c4r c4ke.c4r
//   c4sh>   doom, quit, three times; HEAP_USED the same each time; rps, q
//           tar x c4ix-src.tar; b4ke -f c4ix.b4k; kboot c4ix.c4r
//   c4ix:/$ ls; doom, quit; exit
//   A:\>    MEM: C4IX returned to C4KE, C4KE to dosload, dosload to DOS
//
// and says what each step cost: wall time, cycles, and the heap.
//
//   node libjs/tests/test-c4dos-ladder.mjs [--disk DIR] [--quick]
//
// --quick stops at C4KE (no b4ke, the minute of the run).

import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { createMachine, boot } from '../boot.js';
import { STOP } from '../c4m.js';
import { GuiDevice, CMD } from '../gui-device.js';

const args = process.argv.slice(2);
const opt = n => (args.includes(n) ? args[args.indexOf(n) + 1] : null);
const disk = opt('--disk') || new URL('../images/ladder/', import.meta.url).pathname;
const quick = args.includes('--quick');
const T = 'test-c4dos-ladder';
if (!existsSync(join(disk, 'index.json'))) {
  console.log(`${T}: SKIPPED (no ${disk}: make c4dos-c4ix32 c4bb-images)`);
  process.exit(0);
}

let fail = 0;
const check = (cond, what, extra = '') => {
  console.log(`${T}: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-2000) : ''}`);
  if (!cond) fail = 1;
  return cond;
};

// the disk, read lazily as the page reads it
const index = JSON.parse(readFileSync(join(disk, 'index.json'), 'utf8'));
class LazyDisk extends Map {
  constructor(idx) { super(); this.sizes = new Map(idx); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) {
    if (super.has(n)) return super.get(n);
    if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(disk, n)));
    super.set(n, b);
    return b;
  }
}
const gui = new GuiDevice({ w: 640, h: 480 });
const out = [];
const vm = createMachine({ arenaMb: 128, mhz: 400, gui, drives: [{ files: new LazyDisk(index), writable: false, sink: null }],
                           onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(join(disk, 'c4dos32.c4r'))), ['c4dos32.c4r']);

let log = '';
const text = () => { if (out.length) log += Buffer.from(out.splice(0)).toString('latin1'); return log; };
let presents = 0, halted = false;
// The strings on the display, as test-grin.mjs keeps them: a frame's
// text, with a partial redraw replacing what its rectangles cover.
let cur = [], frame = [], clip = null, partial = false, gen = 0;
const within = (r, x, y) => x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
const pump = () => {
  const w = gui.drain();
  for (let i = 0; w && i < w.length; i += w[i]) {
    const t = w[i + 1];
    if (t === CMD.CLEAR) { cur = []; partial = false; }
    if (t === CMD.CLIP) clip = { x: w[i + 2], y: w[i + 3], w: w[i + 4], h: w[i + 5] };
    if (t === CMD.NOCLIP) clip = null;
    if (t === CMD.RECT && partial) {
      const r = { x: w[i + 2], y: w[i + 3], w: w[i + 4], h: w[i + 5] };
      cur = cur.filter(s => s.gen === gen || !within(r, s.x, s.y) || (clip && !within(clip, s.x, s.y)));
    }
    if (t === CMD.TEXT2) {
      const n = w[i + 8];
      let s = '';
      for (let k = 0; k < n; k++) s += String.fromCharCode((w[i + 9 + (k >> 2)] >>> ((k & 3) * 8)) & 0xff);
      cur.push({ x: w[i + 2], y: w[i + 3], s, gen });
    }
    if (t === CMD.PRESENT) { frame = cur.slice(); presents++; gen++; partial = true; }
  }
};
const shown = re => frame.filter(t => re.test(t.s.trimEnd()));
const run = () => { if (vm.run(4e6) === STOP.HALT) halted = true; pump(); };
// run until cond() or `secs` of wall time
const until = (cond, secs) => {
  const t0 = Date.now();
  while (!halted && Date.now() - t0 < secs * 1000) { if (cond()) return true; run(); }
  return cond();
};
const key = (code, ch = 0) => {
  gui.event({ kind: 'keydown', code, char: ch });
  for (let i = 0; i < 20; i++) run();
  gui.event({ kind: 'keyup', code, char: ch });
  for (let i = 0; i < 20; i++) run();
};
const MB = n => (n / 1048576).toFixed(1) + ' MB';
const costs = [];
let t0, c0;
const begin = () => { t0 = Date.now(); c0 = vm.cycle; vm.heap.peak = vm.heap.used; };
const cost = what => costs.push(`${what}: ${((Date.now() - t0) / 1000).toFixed(1)} s, ` +
  `${((vm.cycle - c0) / 1e6).toFixed(0)}M cycles, heap ${MB(vm.heap.used)} after, ${MB(vm.heap.peak)} at most`);

// DOS's prompt has its current directory in it: A:\>, A:\GAMES\DOOM>
const A = /A:\\[^>\n]*>\s*$/, KE = /c4sh>\s*$/, IX = /c4ix:\/\$\s*$/;
const idle = re => re.test(text().slice(-200));
// type a line at a prompt; what it printed up to the next prompt
const cmd = (line, prompt, secs = 120) => {
  const at = text().length;
  vm.dev.pushInput(line + '\n');
  until(() => prompt.test(text().slice(at)), secs);
  return text().slice(at);
};
// a game: start it, 40 frames, F10 then Y, and its shell's prompt again
const doom = (line, prompt) => {
  const f = presents;
  vm.dev.pushInput(line + '\n');
  const drew = until(() => presents >= f + 40, 120);
  key(121); key(89, 121);
  return drew && until(() => idle(prompt), 60);
};

// ---- C4DOS ----------------------------------------------------------------
begin();
check(until(() => idle(A), 60), 'C4DOS boots the ladder disk', text());
check(text().includes("installed: each program's memory is freed when it ends"), 'with HEAP.SYS', text());
// the disk is in directories, and APPEND finds what is in them: DIR at
// the root has them, and the games, the build kit and C4KE and C4IX's
// files are found from it (Doom, BUILD and everything above DOS below)
let o = cmd('DIR', A, 60);
check(/^GAMES  <DIR>$/m.test(o) && /^BUILD  <DIR>$/m.test(o) && /^C4IX  <DIR>$/m.test(o) && !/doom1\.wad/i.test(o),
  'DIR at A:\\: the directories, not the 250 files', o);
o = cmd('APPEND', A, 60);
check(/^APPEND=A:\\BUILD;A:\\C4KE;A:\\C4IX;A:\\GAMES\\DOOM/m.test(o), "APPEND, AUTOEXEC.BAT's", o);
o = cmd('CD GAMES\\DOOM', /A:\\GAMES\\DOOM>\s*$/, 60) + cmd('DIR', /A:\\GAMES\\DOOM>\s*$/, 60) + cmd('CD \\', A, 60);
check(/doom1\.wad/.test(o) && /doom\.c4r/.test(o), 'CD GAMES\\DOOM, DIR there, CD \\', o);
const used0 = vm.heap.used;
begin();
check(doom('DOOM', A), 'Doom at A>, played and quit', text().slice(-1500));
cost('Doom at A>');
check(vm.heap.used === used0, 'and it gave all its memory back', `${used0} -> ${vm.heap.used}`);

begin();
o = cmd('BUILD', A, 600);
const kbytes = /c4cc: wrote (\d+) bytes to ram:c4ke\.c4r/.exec(o);
check(kbytes && o.includes('ram:init.c4r'), 'BUILD compiles C4KE and its init on the RAM disk', o);
cost(`BUILD (c4ke.c4r ${kbytes ? kbytes[1] : '?'} bytes)`);

begin();
o = cmd('RUN dosload.c4r c4ke.c4r', KE, 300);
check(/seeded \d+\/\d+ file\(s\) from the C4DOS RAM disk/.test(o) && o.includes('C4SH - The C4 SHell'),
  'dosload boots the C4KE it built, seeded from the RAM disk', o);
cost('dosload c4ke.c4r');

// ---- C4KE -----------------------------------------------------------------
// Three rounds. C4KE gives each task a heap generation of HEAP.SYS's
// (c4ke.c's C4KE_HEAP_GEN) and releases it when the task is reaped, so
// HEAP_USED at c4sh> is the same after every round; without that each
// one kept about 14 MB.
check(o.includes('heap generations (C4DOS HEAP.SYS)'), 'C4KE takes heap generations from HEAP.SYS', o);
// at the prompt, once idle has reaped the task that ended
const settled = () => { for (let i = 0; i < 50; i++) run(); return vm.heap.used; };
const keUsed = [];
for (let round = 1; round <= 3; round++) {
  begin();
  check(doom('doom', KE), `Doom under C4KE (round ${round}), played and quit back to c4sh`, text().slice(-1500));
  keUsed.push(settled());
  cost(`Doom under C4KE, round ${round}`);
}
check(keUsed[1] === keUsed[0] && keUsed[2] === keUsed[0],
  `and HEAP_USED at c4sh> stays flat (${keUsed[0]} bytes)`, keUsed.join(' -> '));
o = cmd('rps', /move/, 30) + cmd('q', KE, 30);
check(/Your move/.test(o) && idle(KE), 'rps under C4KE', o);

// grin as a C4KE task, beside a task that spins in the background: its
// naps are C4KE's sleep (bare.c), so the other task keeps running, and its
// DOS Prompt is a C4KE Prompt, whose lines run as tasks
begin();
const ev = e => { gui.event(e); for (let i = 0; i < 5; i++) run(); };
const click = (x, y) => { ev({ kind: 'move', x, y, buttons: 0 }); ev({ kind: 'down', x, y, button: 0 }); ev({ kind: 'up', x, y, button: 0 }); };
const typeIn = s => {
  for (const c of s) {
    const code = c === ' ' ? 32 : c === '&' ? 55 : c === '-' ? 189 : c.toUpperCase().charCodeAt(0);
    ev({ kind: 'keydown', code, char: c.charCodeAt(0) }); ev({ kind: 'keyup', code, char: c.charCodeAt(0) });
  }
  ev({ kind: 'keydown', code: 13, char: 0 }); ev({ kind: 'keyup', code: 13, char: 0 });
};
const clickText = re => { const t = shown(re).pop(); if (t) click(t.x + 5, t.y + 5); return !!t; };
// spin's row in the last ps the window shows: [pid, TIME in ms]
const spinRow = () => {
  const r = shown(/\bspin x$/).pop();
  const m = r && /^\s*(\d+)\s+\d+\s+\S+\s+\S+\s+\d+\s+\S+\s+\S+\s+(\d+)/.exec(r.s);
  return m ? [+m[1], +m[2]] : [0, -1];
};
vm.dev.pushInput('grin\n');
check(until(() => shown(/^Program Manager$/).length && shown(/^DOS Prompt$/).length, 60), 'grin under C4KE: Program Manager', text().slice(-800));
let item = shown(/^DOS Prompt$/).pop();
if (item) { click(item.x + 5, item.y + 5); click(item.x + 5, item.y + 5); }
check(until(() => shown(/^C4KE Prompt 1$/).length && shown(/^c4ke>$/).length, 30), 'its DOS Prompt is a C4KE Prompt', JSON.stringify(frame.map(t => t.s)));
typeIn('spin x &');
check(until(() => shown(/^\[\d+\] in the background/).length, 30), 'spin x & starts a task in the background', JSON.stringify(frame.map(t => t.s)));
typeIn('ps');
check(until(() => spinRow()[1] >= 0, 30), 'ps runs in the window as a task, and lists it', JSON.stringify(frame.map(t => t.s)));
const [spinPid, ms0] = spinRow(), cy0 = vm.cycle;
until(() => vm.cycle - cy0 > 800e6, 120);     // two seconds of machine time, grin idle
typeIn('ps');
until(() => spinRow()[1] > ms0, 30);
const ms1 = spinRow()[1];
check(ms1 - ms0 > 1000, `the background task ran on while grin waited (${ms1 - ms0} ms of 2000)`);
typeIn(`kill ${spinPid} 9`);
check(until(() => text().includes(`SIGKILL on pid ${spinPid}!`) || text().includes(`SIGKILL on pid.${spinPid},`), 30), 'kill PID 9 in the window ends it (the task or the kernel says so on the console)', text().slice(-400));
typeIn('exit');
check(until(() => !shown(/^C4KE Prompt 1$/).length, 30), 'EXIT closes the window');
cost('grin under C4KE');
clickText(/^File$/); until(() => shown(/^Exit grin\.\.\.$/).length, 10);
clickText(/^Exit grin\.\.\.$/); until(() => shown(/^OK$/).length, 10);
clickText(/^OK$/);
check(until(() => idle(KE), 60), 'File > Exit grin: back at c4sh', text().slice(-500));

if (!quick) {
  begin();
  o = cmd('tar x c4ix-src.tar', KE, 300);
  check(/tar: extracted \d+ files/.test(o), "tar unpacks C4IX's sources", o);
  begin();
  o = cmd('b4ke -f c4ix.b4k', KE, 3000);
  const ixbytes = /c4rlink: wrote (\d+) bytes to ramfs:c4ix\.c4r/.exec(o);
  check(ixbytes && o.includes('b4ke: 13 ran, 0 skipped, 0 failed'), 'b4ke builds C4IX: twelve modules and a link', o.slice(-1500));
  cost(`b4ke -f c4ix.b4k (c4ix.c4r ${ixbytes ? ixbytes[1] : '?'} bytes)`);

  begin();
  o = cmd('kboot c4ix.c4r', IX, 300);
  check(o.includes('c4ke: clean shutdown') && o.includes('c4ke: booting c4ix.c4r') && o.includes('C4IX booting'),
    'kboot: C4KE stops cleanly and the C4IX it built boots', o);
  cost('kboot c4ix.c4r');

  // ---- C4IX ---------------------------------------------------------------
  o = cmd('ls', IX, 60);
  check(/\bdoom\b/.test(o) && /\bdesktop\b/.test(o), "C4IX's tree has the page's userland", o);
  begin();
  check(doom('doom', IX), 'Doom under C4IX, played and quit', text().slice(-1500));
  cost('Doom under C4IX');
  // its filesystem is the page's C4IX's (the shared disk's c4ix.vfs.txt):
  // the libraries the desktop and its programs are linked against, and
  // the desktop itself, which loads /lib/libgui.c4s
  o = cmd('ls /lib', IX, 60);
  check(['libc4ix.c4s', 'libc.c4s', 'libarc.c4s', 'libgui.c4s'].every(l => o.includes(l)),
    "C4IX's /lib has the shared libraries (libc4ix, libc, libarc, libgui)", o);
  begin();
  const f0 = presents, at0 = text().length;
  vm.dev.pushInput('desktop\n');
  const drew = until(() => presents >= f0 + 20, 120);
  check(drew && !/missing|cannot|not found|unable/i.test(text().slice(at0)), "the desktop starts under the ladder's C4IX",
    text().slice(at0).slice(-800));
  vm.pendingSignal = 2;                     // Ctrl+C, as the page's button sends it
  check(until(() => idle(IX), 60), 'Ctrl+C ends it, and C4IX is at its prompt again', text().slice(-500));
  cost('the desktop under C4IX');
  if (process.env.LADDER_DEBUG) console.log('--- desktop and Ctrl+C:\n' + text().slice(at0));
  begin();
  o = cmd('exit', A, 300);
  check(o.includes('c4ix: shutdown complete') && o.includes('c4ke: c4ix.c4r returned 0'),
    'C4IX shuts down, C4KE returns, and so does dosload, to A>', o);
  cost('exit (C4IX, then C4KE)');
  if (process.env.LADDER_DEBUG) console.log('--- exit:\n' + o);
} else {
  begin();
  o = cmd('exit', A, 120);
  check(o.includes('clean shutdown'), 'C4KE exits back to A>', o);
  cost('exit (C4KE)');
}

// ---- back at C4DOS --------------------------------------------------------
o = cmd('MEM', A, 60);
const m = /heap: \d+ bytes free of \d+, (\d+) in use/.exec(o);
check(m && +m[1] <= used0, 'every rung gave its memory back to DOS', `${used0} at the start, ${m && m[1]} now`);
check(!halted, 'the machine never stopped');
for (const c of costs) console.log(`${T}: ${c}`);
console.log(`${T}: ${fail ? 'FAILED' : 'OK'} (${(vm.cycle / 1e9).toFixed(1)}G cycles in all, heap at most ${MB(vm.heap.peak)} in the last step)`);
process.exit(fail);
