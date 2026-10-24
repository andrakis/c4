// test-c4dos-heap.mjs - C4DOS's heap generations (DEVICE=HEAP.SYS) with
// a real program: Doom, run again and again at the A> prompt, quit with
// keys each time, as a person would (docs/grin-plan.md G0).
//
// Boots c4dos32.c4r on the games disk (src/c4bb/images/games) as the web
// page does, with a display fitted, and a CONFIG.SYS of its own that
// adds DEVICE=HEAP.SYS. Each round types DOOM, waits for frames, presses
// F10 and then Y, and waits for the prompt. HEAP_USED is read at the
// prompt after every round: from the second round on it must stay
// within 1% of the first's.
//
//   node libjs/tests/test-c4dos-heap.mjs [--rounds N] [--noheap]
//
// --noheap boots without the line and only reports, which is how the
// leak it fixes was measured: 14,853,107 bytes a round on 2026-10-24.
//
// --extra DIR puts DIR's files on the disk too, and if dosexec.c4r is
// among them (make test-c4dos-heap builds it, and dosres.c4r, with c4lc
// -O) runs API v4's scenarios first, with HEAP.SYS on: a RAM-disk file
// a child writes outlives the child, and what a child leaves behind does
// not outlive it.

import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { createMachine, boot } from '../boot.js';
import { STOP } from '../c4m.js';
import { GuiDevice, CMD } from '../gui-device.js';

const ROOT = new URL('../../src/c4bb/images/', import.meta.url).pathname;
const args = process.argv.slice(2);
const rounds = args.includes('--rounds') ? +args[args.indexOf('--rounds') + 1] : 5;
const noheap = args.includes('--noheap');
const image = args.includes('--image') ? args[args.indexOf('--image') + 1] : join(ROOT, 'c4dos32.c4r');
const extra = args.includes('--extra') ? args[args.indexOf('--extra') + 1] : null;
let fail = 0;
const check = (cond, what, extra = '') => {
  console.log(`test-c4dos-heap: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-1500) : ''}`);
  if (!cond) fail = 1;
  return cond;
};

// the games disk, read lazily as the page reads it; CONFIG.SYS is ours.
// No games disk (make c4bb-images builds it): C4DOS's own floppy files,
// and no Doom.
const disk = join(ROOT, 'games');
const haveGames = existsSync(join(disk, 'index.json'));
const index = haveGames ? JSON.parse(readFileSync(join(disk, 'index.json'), 'utf8')) : [];
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
const files = new LazyDisk(index);
const FS = new URL('../../src/c4dos/fs/', import.meta.url).pathname;
const config = readFileSync(haveGames ? join(disk, 'config.sys') : join(FS, 'CONFIG.SYS'), 'latin1').replace(/\r?\n?$/, '\n') +
  (haveGames ? '' : 'DEVICE=CONSOLE.SYS FLUSH\n') + (noheap ? '' : 'DEVICE=HEAP.SYS\n');
files.set('config.sys', new TextEncoder().encode(config));
if (!haveGames) files.set('autoexec.bat', new TextEncoder().encode('@ECHO OFF\nECHO no games disk\n'));
if (extra) for (const n of readdirSync(extra)) files.set(n, new Uint8Array(readFileSync(join(extra, n))));

const gui = new GuiDevice({ w: 640, h: 480 });
const out = [];
const vm = createMachine({ arenaMb: 64, mhz: 400, gui, drives: [{ files, writable: false, sink: null }], onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(image)), ['c4dos32.c4r']);
const text = () => Buffer.from(out).toString('latin1');
let presents = 0;
const pump = () => {
  const words = gui.drain();
  for (let i = 0; words && i < words.length; i += words[i]) if (words[i + 1] === CMD.PRESENT) presents++;
};
let halted = false;
const run = (cycles = 4e6) => { const r = vm.run(cycles); pump(); if (r === STOP.HALT) halted = true; return r; };
const until = (cond, budget = 4000) => {
  for (let i = 0; i < budget && !halted; i++) { if (cond()) return true; run(); }
  return cond();
};
const type = s => vm.dev.pushInput(s);
const key = (code, ch = 0) => {
  gui.event({ kind: 'keydown', code, char: ch });
  for (let i = 0; i < 20; i++) run(1e6);
  gui.event({ kind: 'keyup', code, char: ch });
  for (let i = 0; i < 20; i++) run(1e6);
};
const prompts = () => (text().match(/A>/g) || []).length;

check(until(() => prompts() > 0), haveGames ? 'C4DOS boots the games disk' : 'C4DOS boots', text());
if (!noheap) check(text().includes('heap device installed'), 'HEAP.SYS is installed', text());
// One command at the prompt; what it printed.
const cmd = (line, budget) => {
  const p = prompts(), at = text().length;
  type(line + '\n');
  until(() => prompts() > p, budget);
  return text().slice(at);
};
const memUsed = () => { const m = /heap: \d+ bytes free of \d+, (\d+) in use/.exec(cmd('MEM')); return m ? +m[1] : -1; };

if (files.has('dosexec.c4r') && !noheap) {
  const used0 = memUsed();
  let o = cmd('dosexec deep 3');
  check(['level 1: child returned 3\n', 'level 2: child returned 13\n', 'level 3: child returned 23\n'].every(l => o.includes(l)),
    'exec three deep, exit(3) from three calls down at the bottom', o);
  o = cmd('dosexec resident');
  check(o.includes('first start returned 1') && o.includes('second start (it exits) returned 52') &&
        o.includes('dosres: destructor, after 2 call(s)') && o.includes('unload again returned -1'),
    'load, start twice, unload a resident image', o);
  o = cmd('dosexec catch');
  check(o.includes('catch of an exit gave 42, exited 1') && o.includes('catch of a return gave 6, exited 0'),
    'catch around a function that exits', o);
  check(memUsed() === used0, 'and none of it leaves memory behind', `${used0} before`);
  o = cmd('dosexec file');
  check(o.includes('kid.txt after the child: 16 bytes: made by a child'), 'a child writes kid.txt and its parent reads it', o);
  const used1 = memUsed();
  check(used1 > used0, 'kid.txt is still held after the child has gone', `${used0} -> ${used1}`);
  o = cmd('dosexec leak 4096');
  check(o.includes('left 4096 KB behind') && memUsed() === used1, 'a child that leaves 4 MB behind gives it all back', o);
  o = cmd('TYPE kid.txt');
  check(o.includes('made by a child'), 'kid.txt survives its child, and the next one', o);
}

const used = [];
const haveDoom = files.has('doom.c4r') && files.has('doom1.wad');
if (!haveDoom) console.log('test-c4dos-heap: Doom SKIPPED (no games disk: make c4bb-images)');
for (let r = 1; haveDoom && r <= rounds && !fail; r++) {
  const p = prompts(), f = presents;
  type('DOOM\n');
  if (!check(until(() => presents > f + 40), `round ${r}: Doom draws`, text())) break;
  key(121);                       // F10: quit
  key(89, 121);                   // Y
  if (!check(until(() => prompts() > p), `round ${r}: Doom quits to the prompt`, text())) break;
  used.push(vm.heap.used);
  console.log(`test-c4dos-heap: round ${r}: HEAP_USED ${vm.heap.used} at the prompt, ${vm.heap.count} blocks`);
}
if (used.length > 1) {
  const growth = used[used.length - 1] - used[0];
  console.log(`test-c4dos-heap: ${noheap ? 'without' : 'with'} HEAP.SYS, HEAP_USED grew ${growth} bytes over ${used.length - 1} more round(s), ${Math.round(growth / (used.length - 1))} a round`);
  if (!noheap) {
    const worst = Math.max(...used.map(u => Math.abs(u - used[0]) / used[0]));
    check(worst <= 0.01, `HEAP_USED stays within 1% of the first round's (worst ${(worst * 100).toFixed(3)}%)`);
  }
}
console.log(cmd('MEM').split('\n').filter(l => /heap:|programs loaded/.test(l)).join('\n'));
console.log(fail ? 'test-c4dos-heap: FAILED' : 'test-c4dos-heap: OK');
process.exit(fail);
