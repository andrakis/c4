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
//   c4sh>   doom, quit; rps, q
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
const pump = () => {
  const w = gui.drain();
  for (let i = 0; w && i < w.length; i += w[i]) if (w[i + 1] === CMD.PRESENT) presents++;
};
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
check(text().includes('heap device installed'), 'with HEAP.SYS', text());
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
begin();
check(doom('doom', KE), 'Doom under C4KE, played and quit back to c4sh', text().slice(-1500));
cost('Doom under C4KE');
o = cmd('rps', /move/, 30) + cmd('q', KE, 30);
check(/Your move/.test(o) && idle(KE), 'rps under C4KE', o);

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
  begin();
  o = cmd('exit', A, 300);
  check(o.includes('c4ix: shutdown complete') && o.includes('c4ke: c4ix.c4r returned 0'),
    'C4IX shuts down, C4KE returns, and so does dosload, to A>', o);
  cost('exit (C4IX, then C4KE)');
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
