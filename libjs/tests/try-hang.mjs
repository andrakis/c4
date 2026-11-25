// try-hang.mjs -- the in-C4IX build until its output stops; then where each CPU is
// (samples of its pc), what each MMU is faulting on, the tasks and the log's tail.
// The struct task offsets are c4ix.h's; change them with it.
//   node libjs/tests/try-hang.mjs       (MB, QUIET, PIN=ms to pin Date to simulated time; OUT=file)
import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createMachine, boot } from '../boot.js';
import { typeBytes } from '../keys.js';
import { readC4r } from '../c4r.js';

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
if (process.env.PIN !== undefined) { const RD = Date, BASE = +process.env.PIN; let vmr = null;
  const now = () => BASE + Math.floor(vmr ? vmr.dev.simMs() : 0);
  globalThis.Date = class extends RD { constructor(...a) { super(...(a.length ? a : [now()])); } static now() { return now(); } };
  globalThis.__pinvm = v => { vmr = v; }; }
const store = new Map();
const out = [];
const vm = createMachine({ arenaMb: +(process.env.MB || 32), mmu: true, cpus: 4,
  drives: [{ files: new LazyDisk(), writable: false, sink: null },
           { files: new Map(), writable: true, sink: (n, b) => store.set(n, b), remove: n => store.delete(n) }],
  onByte: b => out.push(b) });
if (globalThis.__pinvm) globalThis.__pinvm(vm);
boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
const until = (c, b = 30000) => { for (let i = 0; i < b && !c(); i++) vm.run(2e6); return c(); };
let seq = 0;
const sh = cmds => { const n0 = out.length, mark = `@@${++seq}@@`; typeBytes(vm, new TextEncoder().encode(`${cmds}\necho ${mark}\n`)); until(() => text(n0).includes(mark + '\n')); return text(n0); };
sh('wait 7');
sh('persist -d 60 &\nfree');
const syms = vm.symbols.slice().sort((a, b) => a.addr - b.addr);
const lo = syms[0].addr, hi = syms[syms.length - 1].addr + 4096;
const fn = pc => { if (pc < lo || pc >= hi) return 'user@' + pc.toString(16); let a = 0, b = syms.length - 1;
  while (a < b) { const m = (a + b + 1) >> 1; if (syms[m].addr <= pc) a = m; else b = m - 1; } return syms[a].name; };
const cpus = vm.smp ? vm.smp.machines : [vm];
const n0 = out.length, t0 = performance.now() / 1000 * 1000;
typeBytes(vm, new TextEncoder().encode('cd /usr\nb4ke -B -j 3\nfree\ncat /proc/meminfo\ndmesg -t\necho @@done@@\n'));
let last = out.length, quiet = 0, it = 0;
const QUIET = +(process.env.QUIET || 1500);
while (quiet < QUIET && !text(n0).includes('@@done@@\n') && it < 3000000) {
  vm.run(2e6); it++;
  if (out.length !== last) { last = out.length; quiet = 0; } else quiet++;
}
const b = text(n0);
if (process.env.OUT) writeFileSync(process.env.OUT, b);
console.log('stopped after', Math.round((performance.now() - t0) / 1000), 's wall, quiet', quiet, 'sim ms', vm.dev.simMs());
console.log(b.split('\n').slice(-12).join('\n'));
// where each CPU is: samples of its pc over a while
const tid0 = m => { const L = m.local || []; return L[0] ? vm.arena.i32[L[0] >> 2] : -1; };
const hist = cpus.map(() => new Map());
for (let s = 0; s < 4000; s++) {
  vm.run(20000);
  cpus.forEach((m, i) => { const k = (m.cpuState || 'run') + ' ' + fn(m.pc); hist[i].set(k, (hist[i].get(k) || 0) + 1); });
}
hist.forEach((h, i) => console.log('cpu', i, [...h].sort((a, b) => b[1] - a[1]).slice(0, 8).map(([k, v]) => `${k}:${v}`).join('  ')));
// what each is faulting on
for (let s = 0; s < 6; s++) { vm.run(100000);
  cpus.forEach((m, i) => { const u = m.mmu; if (!u) return; const p = u.probe(u.faddr);
    console.log('fault cpu', i, 'cur', tid0(m), 'faddr', (u.faddr>>>0).toString(16), 'finfo', u.finfo, 'fpc', (u.fpc>>>0).toString(16), 'ptbr', (u.ptbr>>>0).toString(16), 'pte', (p>>>0).toString(16), 'at', u.pteAt, 'faults', u.faults, 'rd/wr/ex/nx', u.rd[u.faddr>>>12], u.wr[u.faddr>>>12], u.ex[u.faddr>>>12], u.nx[u.faddr>>>12], 'pc', (m.pc>>>0).toString(16), 'vm', m.vm); }); }
console.log('sim ms now', vm.dev.simMs(), 'output since', out.length - last);
// the kernel's tasks (struct task, c4ix.h: words 0 id, 1 state, 4 next,
// 24 wait_for, 26 block_vn, 110 parent; name at 151 or 152) and its log
const KS = readC4r(new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r')))).symbols;
const sym = n => { const s = KS.find(x => x.name === n && x.cls === 131); return s ? vm.dev.img.data + s.value : 0; };
const i32 = vm.arena.i32, u8 = new Uint8Array(i32.buffer, i32.byteOffset, i32.byteLength);
const str = (a, n) => { let s = ''; for (let k = 0; k < n && u8[a + k]; k++) s += String.fromCharCode(u8[a + k]); return s; };
const pr = s => /^[\x20-\x7e]{2,}$/.test(s);
console.log('task_head at', sym('task_head').toString(16), 'klog_ring', sym('klog_ring').toString(16));
let t = i32[sym('task_head') >> 2], guard = 0;
const ST = ['?', 'READY', 'RUNNING', 'ZOMBIE', 'WAITING', 'BLOCKED', 'SLEEPING', 'VFORK'];
while (t && guard++ < 200) {
  const w = k => i32[(t >> 2) + k];
  const nm = pr(str(t + 151 * 4, 16)) ? str(t + 151 * 4, 16) : str(t + 152 * 4, 16);
  console.log(`task ${w(0)} ${nm} ${ST[w(1)] || w(1)} wait_for ${w(24)} block_vn ${(w(26) >>> 0).toString(16)} parent ${w(110)} words130-140 ${Array.from({ length: 11 }, (_, k) => w(130 + k)).join(',')}`);
  t = w(4);
}
// each CPU's window (struct cpu): 0 cur, 1 intrap, 2 lockdepth, 12 prev, 13 idle
const tid = p => p ? i32[p >> 2] : '-';
cpus.forEach((m, i) => { const L = m.local || [];
  console.log(`cpu ${i} state ${m.cpuState} cur ${tid(L[0])} intrap ${L[1]} lockdepth ${L[2]} prev ${tid(L[12])} idle ${tid(L[13])} waitAddr ${(m.waitAddr >>> 0).toString(16)}`); });
for (const n of ['smp_idle', 'smp_park']) { const a = sym(n); console.log(n, a.toString(16), Array.from({ length: 4 }, (_, k) => i32[(a >> 2) + k]).join(',')); }
const kn = i32[sym('klog_n') >> 2], kr = sym('klog_ring');
let kl = ''; for (let k = Math.max(0, kn - 6000); k < kn; k++) kl += String.fromCharCode(u8[kr + (k & 65535)]);
console.log('klog tail:\n' + kl);
process.exit(0);
