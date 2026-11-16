// try-quake3.mjs -- three windowed Quakes at once, as the page has C4IX
// (?mhz=400&cpus=4&parallel&mmu=1&mem=32): `pin N quake --window &` typed
// N = 1, 2, 3 into the desktop's Terminal, with a sound card. For looking,
// not passing (the try-* harnesses). The console's top shows the tasks, and
// each status line says how many Quakes are alive; a kernel line about a
// kill or a fault is echoed as KERNEL.
//
// Found 2026-11-16: one Quake killed for "a device's read of" another's
// memory, the sound card's PCM latches set by two players at once
// (snd-device.js PCM_START). NOSTART=1 hides CAPS bit 6, so the games play
// through the latches as they did, and the race comes back.
//
//   node libjs/tests/try-quake3.mjs
//   MB=32 CPUS=4 PARALLEL=0|1 SECS=400 (machine seconds) NQ=3 SND=0
//   CMD='pin %d quake --window &' ARGS=-v (kernel arguments) QUIET=1 ECHO=1
//   LIBJS=dir IMAGES=dir OUT=file (the console at the end)
import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { Worker } from 'node:worker_threads';
import { fileURLToPath } from 'node:url';
const LIBJS = process.env.LIBJS || fileURLToPath(new URL('..', import.meta.url));
const { createMachine, boot } = await import(LIBJS + '/boot.js');
const { STOP } = await import(LIBJS + '/c4m.js');
const { GuiDevice, CMD } = await import(LIBJS + '/gui-device.js');
const { SoundDevice } = await import(LIBJS + '/snd-device.js');
const { typeBytes } = await import(LIBJS + '/keys.js');
const ROOT = process.env.IMAGES || join(LIBJS, '../src/c4bb/images/');
const MB = Number(process.env.MB || 32), CPUS = Number(process.env.CPUS || 4), PARALLEL = process.env.PARALLEL !== '0';
const SECS = Number(process.env.SECS || 400), N = Number(process.env.NQ || 3);
const SND = process.env.SND !== '0';
const index = JSON.parse(readFileSync(join(ROOT, 'disk/index.json'), 'utf8'));
class LazyDisk extends Map {
  constructor(idx) { super(); this.sizes = new Map(idx.map(e => [e[0], e[1]])); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) {
    if (super.has(n)) return super.get(n);
    if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(ROOT, 'disk', n)));
    super.set(n, b); return b;
  }
}
const store = new Map();
const drives = [{ files: new LazyDisk(index), writable: false, sink: null },
  { files: store, writable: true, sink: (n, b) => store.set(n, b), remove: n => store.delete(n) }];
const gui = new GuiDevice({ w: 1024, h: 768 });
const snd = SND ? new SoundDevice() : null;
// NOSTART=1: the card as it was, without PCM_START (CAPS bit 6): the five-write snd_play
if (snd && process.env.NOSTART) { const r = snd.read32.bind(snd); snd.read32 = a => a === 0x480 ? r(a) & ~64 : r(a); }
const out = [];
const vm = createMachine({ arenaMb: MB, mmu: true, cpus: CPUS, parallel: PARALLEL, mhz: 400, spawn: url => new Worker(url),
  gui, snd, drives, onByte: b => { out.push(b); if (process.env.ECHO) process.stdout.write(String.fromCharCode(b)); } });
const argv = ['c4ix32.c4r', ...(process.env.ARGS ? process.env.ARGS.split(' ') : []), '--desktop'];
boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), argv);
const consoleText = () => Buffer.from(out).toString('latin1');
let frame = [], cur = [], rgb = 0;
const pump = () => {
  if (snd) snd.drain();
  const words = gui.drain();
  let sized = null;
  for (let i = 0; words && i < words.length; i += words[i]) {
    const t = words[i + 1];
    if (t === CMD.CLEAR) cur = [];
    if (t === CMD.TEXT2) {
      const n = words[i + 8]; let s = '';
      for (let k = 0; k < n; k++) s += String.fromCharCode((words[i + 9 + (k >> 2)] >>> ((k & 3) * 8)) & 0xff);
      cur.push(s);
    }
    if (t === CMD.PRESENT) { frame = cur.slice(); cur = []; }
    if (t === CMD.SIZE) sized = { w: words[i + 2], h: words[i + 3] };
    if (t === CMD.RGB) rgb++;
  }
  if (sized && (sized.w !== gui.w || sized.h !== gui.h)) gui.event({ kind: 'resize', ...sized });
};
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
const key = ch => { gui.event({ kind: 'keydown', code: ch === '\n' ? 13 : ch.toUpperCase().charCodeAt(0), char: ch === '\n' ? 10 : ch.charCodeAt(0), mods: 0 }); run(); };
const type = s => { for (const ch of s) key(ch); };
const t0 = Date.now();
const log = s => console.log(`[${((Date.now() - t0) / 1000).toFixed(0)}s sim ${(vm.dev.simMs() / 1000).toFixed(1)}] ${s}`);
log('boot ' + argv.join(' '));
if (!until(() => frame.some(s => /c4ix:.*\$/.test(s)), 20000)) { log('no prompt: ' + frame.join(' | ') + '\n' + consoleText().slice(-1500)); process.exit(2); }
log('terminal prompt');
const cmd = process.env.CMD || 'pin %d quake --window &';
for (let k = 1; k <= N; k++) { type(cmd.replace('%d', k) + '\n'); until(() => false, 50); log('started ' + k); }
let lastRgb = 0, lastCon = consoleText().length, lastQ = '', maxq = 0, minq = 99;
const wins = () => frame.filter(s => /quake/i.test(s)).join(',');
while (!halted && vm.dev.simMs() / 1000 < SECS) {
  for (let i = 0; i < 100; i++) run();
  const c = consoleText();
  if (c.length > lastCon) { if (!process.env.QUIET) log('CONSOLE: ' + c.slice(lastCon).replace(/\n/g, '\n    ')); for (const l of c.slice(lastCon).split('\n')) if (/DIAG|killed|fault|panic|lost/.test(l)) log('KERNEL: ' + l); lastCon = c.length; }
  const q = wins();
  // the live quakes, by the last whole top screen on the console
  const tops = c.split('top: ');
  const scr = tops.length > 2 ? tops[tops.length - 2] : '';
  const nq = (scr.match(/ quake\s*$/gm) || []).length;
  if (tops.length > 2) { if (nq > maxq) maxq = nq; if (nq < maxq) minq = Math.min(minq, nq); }
  log(`rgb +${rgb - lastRgb} live quakes ${nq} (most ${maxq}, fewest since ${minq === 99 ? '-' : minq})`);
  lastRgb = rgb; lastQ = q;
}
if (process.env.OUT) writeFileSync(process.env.OUT, consoleText());
log(`done: most ${maxq} quakes at once, ${minq === 99 ? 'none lost' : 'down to ' + minq}`);
process.exit(0);
