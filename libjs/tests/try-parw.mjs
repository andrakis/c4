// try-parw.mjs -- innerbench -Tln N on real threads, as the browser's parallel mode runs
// it, the kernel's swap and scheduler counters every 20 s; at the end (or LIMIT s),
// the tasks by state and the log's tail. Struct task offsets are c4ix.h's.
//   node libjs/tests/try-parw.mjs       (N, LIMIT, MHZ, IMAGE)
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { Worker } from 'node:worker_threads';
import { createMachine, boot } from '../boot.js';
import { typeBytes } from '../keys.js';
import { readC4r } from '../c4r.js';
import { STOP } from '../c4m.js';

const ROOT = fileURLToPath(new URL('../../src/c4bb/images/', import.meta.url));
const DISK = join(ROOT, 'disk');
class LazyDisk extends Map {
  constructor() { super(); this.sizes = new Map(JSON.parse(readFileSync(join(DISK, 'index.json'), 'utf8'))); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) {
    if (super.has(n)) return super.get(n);
    if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(DISK, n))); super.set(n, b); return b;
  }
}
const out = [];
const IMG = process.env.IMAGE || join(ROOT, 'c4ix32.c4r');
const vm = createMachine({ arenaMb: 32, mhz: +(process.env.MHZ || 400), mmu: true, cpus: 4, parallel: true, spawn: url => new Worker(url),
  drives: [{ files: new LazyDisk(), writable: false, sink: null },
    { files: new Map(), writable: true, sink: () => {}, remove: () => {} }], onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(IMG)), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
const par = vm.smp && vm.smp.parallel ? vm.smp : null;
const KS = readC4r(new Uint8Array(readFileSync(IMG))).symbols;
const sym = n => { const s = KS.find(x => x.name === n && x.cls === 131); return s ? vm.dev.img.data + s.value : 0; };
const i32 = vm.arena.i32, u8 = new Uint8Array(i32.buffer, i32.byteOffset, i32.byteLength);
const g = n => { const a = sym(n); return a ? i32[a >> 2] : -1; };
const str = (a, n) => { let s = ''; for (let k = 0; k < n && u8[a + k]; k++) s += String.fromCharCode(u8[a + k]); return s; };
const printable = s => /^[\x20-\x7e]{2,}$/.test(s);
const tname = t => { const a = str(t + 151 * 4, 16); return printable(a) ? a : str(t + 152 * 4, 16); };
let lastT = Date.now(), watching = false, t0 = Date.now(), last = {};
const status = () => {
  const c = { out: g('mmu_swapouts'), in: g('mmu_swapins'), moves: g('mmu_moves'), held: g('mmu_nheld'), sfree: g('mmu_sfree'), cpms: g('klog_cpms'), pg: g('mmu_pgcyc'), lwin: g('mmu_lwin'), idle: g('smp_idlemask') };
  const hu = Math.round(vm.heap.used / 1024), ms = Math.round(vm.dev.simMs());
  console.log(`[${Math.round((Date.now() - t0) / 1000)} s real, sim ${ms}] out ${c.out} (+${c.out - (last.out || 0)}) in ${c.in} (+${c.in - (last.in || 0)}) moves ${c.moves} held ${c.held} cpms ${c.cpms} pgcyc ${c.pg} lwin ${c.lwin} idlemask ${c.idle} sfree ${c.sfree} heap ${hu}K switches ${g('sched_switches')} bench-done ${(text().match(/Benchmark complete/g) || []).length}`);
  last = c;
};
const until = (cond, budget) => {
  for (let i = 0; i < budget && !cond() && !vm.dev.halted; i++) {
    const r = vm.run(2e6);
    if (watching && Date.now() - lastT > 20000) { lastT = Date.now(); status(); }
    if (!par || vm.dev.halted) continue;
    par.service();
    if (r === STOP.SLEEP && par.othersRunning()) par.sleepSync(vm.dev.napMs);
    else if (r === STOP.PARK || r === STOP.CPUHALT || vm.cpuState !== 'run') par.parkSync(10);
  }
  return cond();
};
let seq = 0;
const sh = cmds => { const n0 = out.length, mark = `@@${++seq}@@`; typeBytes(vm, new TextEncoder().encode(`${cmds}\necho ${mark}\n`)); until(() => text(n0).includes(mark + '\n'), 5000000); return text(n0); };
until(() => text().includes('c4ix:/$'), 30000);
const unpack = (/unpacking c4ix-root\.tgz in the background as task (\d+)/.exec(text()) || [])[1] || 7;
sh(`wait ${unpack}`);
const N = process.env.N || 50, LIMIT = +(process.env.LIMIT || 600) * 1000;
watching = true; t0 = Date.now(); lastT = t0;
const n0 = out.length, mark = `@@go@@`;
typeBytes(vm, new TextEncoder().encode(`cd /usr\ninnerbench -Tln ${N}\necho ${mark}\n`));
const done = until(() => text(n0).includes(mark + '\n') || Date.now() - t0 > LIMIT, 50000000) && text(n0).includes(mark + '\n');
status();
console.log(done ? 'FINISHED' : 'NOT FINISHED');
console.log(text(n0).split('\n').filter(l => /killed|panic|fault|benchmark processes/.test(l)).slice(0, 12).join('\n'));
let t = i32[sym('task_head') >> 2], guard = 0;
const ST = ['?', 'READY', 'RUNNING', 'ZOMBIE', 'WAITING', 'BLOCKED', 'SLEEPING', 'VFORK'];
const states = new Map();
while (t && guard++ < 200) {
  const w = k => i32[(t >> 2) + k];
  const off = printable(str(t + 151 * 4, 16)) ? 151 : 152;
  const held = w(off + 4), flags = w(off + 6);
  const k = `${tname(t)} ${ST[w(1)] || w(1)}${held ? ' held' : ''}`;
  states.set(k, (states.get(k) || 0) + 1);
  t = w(4);
}
for (const [k, n] of states) console.log(n + 'x', k);
const kn = i32[sym('klog_n') >> 2], kr = sym('klog_ring');
let kl = ''; for (let k = Math.max(0, kn - 2500); k < kn; k++) kl += String.fromCharCode(u8[kr + (k & 65535)]);
console.log('klog tail:\n' + kl);
process.exit(0);
