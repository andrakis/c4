// try50.mjs -- innerbench -Tln N, with the kernel's swap counters as it goes; when its
// output stops, where each CPU is, the tasks by state, the log's tail and the heap.
// Struct task offsets are c4ix.h's.
//   node libjs/tests/try50.mjs          (MB, N, QUIET, EVERY, IMAGE)
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createMachine, boot } from '../boot.js';
import { typeBytes } from '../keys.js';
import { readC4r } from '../c4r.js';
import { heapDump } from './heapdump.mjs';

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
const store = new Map();
const out = [];
const vm = createMachine({ arenaMb: +(process.env.MB || 32), mmu: true, cpus: 4,
  drives: [{ files: new LazyDisk(), writable: false, sink: null },
           { files: new Map(), writable: true, sink: (n, b) => store.set(n, b), remove: n => store.delete(n) }],
  onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(process.env.IMAGE || join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
const until = (c, b = 30000) => { for (let i = 0; i < b && !c(); i++) vm.run(2e6); return c(); };
let seq = 0;
const sh = cmds => { const n0 = out.length, mark = `@@${++seq}@@`; typeBytes(vm, new TextEncoder().encode(`${cmds}\necho ${mark}\n`)); until(() => text(n0).includes(mark + '\n')); return text(n0); };
sh('wait 7');

const syms = vm.symbols.slice().sort((a, b) => a.addr - b.addr);
const lo = syms[0].addr, hi = syms[syms.length - 1].addr + 4096;
const fn = pc => { if (pc < lo || pc >= hi) return 'user@' + pc.toString(16); let a = 0, b = syms.length - 1;
  while (a < b) { const m = (a + b + 1) >> 1; if (syms[m].addr <= pc) a = m; else b = m - 1; } return syms[a].name; };
const cpus = vm.smp ? vm.smp.machines : [vm];
const KS = readC4r(new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r')))).symbols;
const sym = n => { const s = KS.find(x => x.name === n && x.cls === 131); return s ? vm.dev.img.data + s.value : 0; };
const i32 = vm.arena.i32, u8 = new Uint8Array(i32.buffer, i32.byteOffset, i32.byteLength);
const str = (a, n) => { let s = ''; for (let k = 0; k < n && u8[a + k]; k++) s += String.fromCharCode(u8[a + k]); return s; };
const pr = s => /^[\x20-\x7e]{2,}$/.test(s);
const g = n => i32[sym(n) >> 2];

const n0 = out.length;
typeBytes(vm, new TextEncoder().encode(`cd /usr\ninnerbench -Tln ${process.env.N || 50}\nfree\nps\necho @@done@@\n`));
let last = out.length, quiet = 0, it = 0;
const QUIET = +(process.env.QUIET || 3000);
const EVERY = +(process.env.EVERY || 250);
while (quiet < QUIET && !text(n0).includes('@@done@@\n') && it < 3000000) {
  vm.run(2e6); it++;
  if (out.length !== last) { process.stdout.write(text(last)); last = out.length; quiet = 0; } else quiet++;
  if (it % EVERY == 0) {
    const h = cpus.map(m => (m.cpuState || 'run') + ':' + fn(m.pc)).join(' ');
    console.log(`[${it} sim ${Math.round(vm.dev.simMs())} out ${g('mmu_swapouts')} in ${g('mmu_swapins')} moves ${g('mmu_moves')} firsts ${g('mmu_firsts')} sfree ${g('mmu_sfree')} heap ${Math.round(vm.heap.used / 1024)}K largest ${Math.round(vm.heap.largest() / 1024)}K | ${h}]`);
  }
}
console.log('\nstopped: quiet', quiet, 'sim ms', vm.dev.simMs());
const hist = cpus.map(() => new Map());
for (let s = 0; s < 2000; s++) {
  vm.run(20000);
  cpus.forEach((m, i) => { const k = (m.cpuState || 'run') + ' ' + fn(m.pc); hist[i].set(k, (hist[i].get(k) || 0) + 1); });
}
hist.forEach((h, i) => console.log('cpu', i, [...h].sort((a, b) => b[1] - a[1]).slice(0, 8).map(([k, v]) => `${k}:${v}`).join('  ')));
let t = i32[sym('task_head') >> 2], guard = 0;
const ST = ['?', 'READY', 'RUNNING', 'ZOMBIE', 'WAITING', 'BLOCKED', 'SLEEPING', 'VFORK'];
const states = new Map();
while (t && guard++ < 400) {
  const w = k => i32[(t >> 2) + k];
  const nm = pr(str(t + 151 * 4, 16)) ? str(t + 151 * 4, 16) : str(t + 152 * 4, 16);
  const k = `${nm} ${ST[w(1)] || w(1)} wait_for ${w(24)}`;
  states.set(k, (states.get(k) || 0) + 1);
  t = w(4);
}
for (const [k, n] of states) console.log(n + 'x', k);
const tid = p => p ? i32[p >> 2] : '-';
cpus.forEach((m, i) => { const L = m.local || [];
  console.log(`cpu ${i} state ${m.cpuState} cur ${tid(L[0])} intrap ${L[1]} lockdepth ${L[2]} prev ${tid(L[12])} idle ${tid(L[13])} waitAddr ${(m.waitAddr >>> 0).toString(16)}`); });
const kn = i32[sym('klog_n') >> 2], kr = sym('klog_ring');
let kl = ''; for (let k = Math.max(0, kn - 4000); k < kn; k++) kl += String.fromCharCode(u8[kr + (k & 65535)]);
console.log('klog tail:\n' + kl);
console.log(heapDump(vm));
process.exit(0);
