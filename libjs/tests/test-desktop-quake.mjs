// test-desktop-quake.mjs - Quake in a window of C4IX's desktop, with the MMU
// on, in a small machine (docs/c4ix-mmu.md 11), headless.
//
// The desktop draws a windowed game's frames by borrowing the game's
// memory (SYS_MAPFROM) and handing the display the address (GUI_BLIT8),
// which reads the whole 320x200 picture as one run of memory. A block a
// program asks for has its pages in frames from anywhere, so the run has
// to be made one before the display reads it: the desktop was killed for
// "a device's read of" Doom's canvas at 32 MB with four CPUs, where the
// frames had not happened to come out in order.
//
// Boots C4IX with a display, the MMU and a drive 1 (as the page has, where
// the root archive's files are kept), starts the desktop from the console,
// types `doom --window` into its Terminal, and waits for the game's frames
// to come through the display; nothing may be killed or fault.
//
// MB (default 32), CPUS (default 4), PARALLEL=1: the threaded engine.
//
//   node libjs/tests/test-desktop-quake.mjs

import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { Worker } from 'node:worker_threads';
import { createMachine, boot } from '../boot.js';
import { STOP } from '../c4m.js';
import { GuiDevice, CMD } from '../gui-device.js';
import { typeBytes } from '../keys.js';
import { fileURLToPath } from 'node:url';

const ROOT = process.env.IMAGES || fileURLToPath(new URL('../../src/c4bb/images/', import.meta.url));
const MB = Number(process.env.MB || 64), CPUS = Number(process.env.CPUS || 4), PARALLEL = !!process.env.PARALLEL;
const FRAMES = Number(process.env.FRAMES || 30);
let fail = 0;
const check = (cond, what, extra = '') => {
  console.log(`test-desktop-quake: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-1500) : ''}`);
  if (!cond) fail = 1;
  return cond;
};

const vfs = readFileSync(join(ROOT, 'disk/c4ix.vfs.txt'), 'utf8');
if (!/^\s*quake: ilink/m.test(vfs)) {
  console.log('test-desktop-quake: Quake is not on the disk (games/README.md): skipped');
  process.exit(0);
}
const index = JSON.parse(readFileSync(join(ROOT, 'disk/index.json'), 'utf8'));
class LazyDisk extends Map {
  constructor(idx) { super(); this.sizes = new Map(idx.map(e => [e[0], e[1]])); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) {
    if (super.has(n)) return super.get(n);
    if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(ROOT, 'disk', n)));
    super.set(n, b);
    return b;
  }
}
const store = new Map();
const drives = [{ files: new LazyDisk(index), writable: false, sink: null },
  { files: store, writable: true, sink: (n, b) => store.set(n, b), remove: n => store.delete(n) }];
const gui = new GuiDevice({ w: 640, h: 480 });
const out = [];
const vm = createMachine({ arenaMb: MB, mmu: true, cpus: CPUS, parallel: PARALLEL, spawn: url => new Worker(url),
  gui, drives, onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const consoleText = () => Buffer.from(out).toString('latin1');

// The screen: the strings of the last frame presented, and the game's
// 8-bit frames (GUI_BLIT8, made 32-bit by the device) counted
let frame = [], cur = [], rgbFrames = 0, rgbLast = null;
const pump = () => {
  const words = gui.drain();
  let sized = null;
  for (let i = 0; words && i < words.length; i += words[i]) {
    const t = words[i + 1];
    if (t === CMD.CLEAR) cur = [];
    if (t === CMD.TEXT2) {
      const n = words[i + 8];
      let s = '';
      for (let k = 0; k < n; k++) s += String.fromCharCode((words[i + 9 + (k >> 2)] >>> ((k & 3) * 8)) & 0xff);
      cur.push(s);
    }
    if (t === CMD.PRESENT) { frame = cur.slice(); cur = []; }
    if (t === CMD.SIZE) sized = { w: words[i + 2], h: words[i + 3] };
    if (t === CMD.RGB) { rgbFrames++; rgbLast = { w: words[i + 4], h: words[i + 5] }; }
  }
  if (sized && (sized.w !== gui.w || sized.h !== gui.h)) gui.event({ kind: 'resize', ...sized });
};
// On threads CPU 0 sleeps and parks for real, as test-c4ix-par.mjs has it
let halted = false;
const par = vm.smp && vm.smp.parallel ? vm.smp : null;
const run = () => {
  const r = vm.run(2e6);
  if (r === STOP.HALT || vm.dev.halted) halted = true;
  if (par && !halted) {
    par.service();
    if (r === STOP.SLEEP && par.othersRunning()) par.sleepSync(vm.dev.napMs);
    else if (r === STOP.PARK || r === STOP.CPUHALT || vm.cpuState !== 'run') par.parkSync(10);
  }
  pump();
};
const until = (cond, budget = 400) => { for (let i = 0; i < budget && !halted; i++) { if (cond()) return true; run(); } return cond(); };
const sh = cmd => {
  const c0 = consoleText().length;
  typeBytes(vm, new TextEncoder().encode(cmd + '\n'));
  until(() => /c4ix:\/\$ ?$/.test(consoleText().slice(c0).trimEnd() + ' '), 3000);
};
const key = ch => { gui.event({ kind: 'keydown', code: ch === '\n' ? 13 : ch.toUpperCase().charCodeAt(0), char: ch === '\n' ? 10 : ch.charCodeAt(0), mods: 0 }); run(); };
const type = s => { for (const ch of s) key(ch); };
const bad = () => /page fault|killed|panic|nothing is at/.test(consoleText());

check(until(() => consoleText().includes('c4ix:/$'), 3000), `C4IX boots in ${MB} MB, MMU on, ${CPUS} CPU${CPUS > 1 ? 's' : ''}${PARALLEL ? ' on threads' : ''}`);
const unpack = (/unpacking c4ix-root\.tgz in the background as task (\d+)/.exec(consoleText()) || [])[1];
if (unpack) sh(`wait ${unpack}`);
// no screen saver: headless, an idle machine would reach it
sh('mkdir /etc');
sh('echo scheme 0 bg -1 wall 1 saver 0 wait 5 > /etc/desktop.cfg');
typeBytes(vm, new TextEncoder().encode('desktop\n'));
check(until(() => consoleText().includes('desktop: running on the display'), 3000), 'the desktop starts', consoleText().slice(-600));
check(until(() => frame.some(s => /c4ix:\/\$/.test(s)), 1500), 'its Terminal has a prompt', frame.join(' | '));
type('quake --window -nosound\n');
check(until(() => rgbFrames >= FRAMES || bad(), 20000) && !bad(),
  `Quake's frames come through the display in its window (${rgbFrames}, the last ${rgbLast ? rgbLast.w + 'x' + rgbLast.h : 'none'})`,
  consoleText().slice(-800));
check(rgbLast && rgbLast.w === 320 && rgbLast.h === 200, '320x200 in 8 bits', JSON.stringify(rgbLast));
check(!bad(), 'nothing was killed and nothing faulted', consoleText().split('\n').filter(l => /page fault|killed|panic|nothing is at/.test(l)).join('\n'));
console.log(`test-desktop-quake: ${fail ? 'FAILED' : 'OK'}`);
process.exit(fail);
