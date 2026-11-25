// try-fault.mjs -- the 32 MB build until a task is killed; then what the kernel's books
// say of the address: its MMU region, the page tables, the tasks and the log's tail.
// The struct task and region word offsets are c4ix.h's; change them with it.
//   node libjs/tests/try-fault.mjs      (MB, IMAGE, CMD with ; for newlines)
import { readFileSync } from 'node:fs';
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
const store = new Map();
const out = [];
let dump = null, line = '';
const onByte = b => {
  out.push(b);
  if (b === 10) { if (dump && line.endsWith('killed')) { const d = dump; dump = null; d(line); } line = ''; }
  else line += String.fromCharCode(b);
};
const vm = createMachine({ arenaMb: +(process.env.MB || 32), mmu: true, cpus: 4,
  drives: [{ files: new LazyDisk(), writable: false, sink: null },
           { files: new Map(), writable: true, sink: (n, b) => store.set(n, b), remove: n => store.delete(n) }],
  onByte });
boot(vm, new Uint8Array(readFileSync(process.env.IMAGE || join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
const until = (c, b = 30000) => { for (let i = 0; i < b && !c(); i++) vm.run(2e6); return c(); };
let seq = 0;
const sh = cmds => { const n0 = out.length, mark = `@@${++seq}@@`; typeBytes(vm, new TextEncoder().encode(`${cmds}\necho ${mark}\n`)); until(() => text(n0).includes(mark + '\n')); return text(n0); };
sh('wait 7');
sh('cd /usr');

const KS = readC4r(new Uint8Array(readFileSync(process.env.IMAGE || join(ROOT, 'c4ix32.c4r')))).symbols;
const sym = n => { const s = KS.find(x => x.name === n && x.cls === 131); return s ? vm.dev.img.data + s.value : 0; };
const i32 = vm.arena.i32, u8 = new Uint8Array(i32.buffer, i32.byteOffset, i32.byteLength);
const g = n => i32[sym(n) >> 2];
const str = (a, n) => { let s = ''; for (let k = 0; k < n && u8[a + k]; k++) s += String.fromCharCode(u8[a + k]); return s; };
const hx = v => '0x' + (v >>> 0).toString(16);
const printable = s => /^[\x20-\x7e]{2,}$/.test(s);
const tname = t => { const a = str(t + 151 * 4, 16); return printable(a) ? a : str(t + 152 * 4, 16); };
const pte = (d, v) => { if (!d) return null; const p = i32[(d >> 2) + ((v >>> 22) & 1023)]; if (!(p & 1)) return { pde: hx(p) }; const pt = p & ~4095; return { pde: hx(p), e: hx(i32[(pt >> 2) + ((v >>> 12) & 1023)]) }; };

dump = msg => {
  console.log(msg, 'sim', vm.dev.simMs());
  const va = parseInt(/0x([0-9a-f]+) \(/.exec(msg)[1], 16) & ~4095;
  const uoff = g('mmu_uoff'), vpg = g('mmu_vpg'), kdir = g('mmu_kdir');
  const idx = (va - uoff) >> 12;
  const r = i32[(vpg >> 2) + idx];
  const rw = k => i32[(r >> 2) + k];
  console.log('region', hx(r));
  if (r) {
    const o = rw(5);
    console.log(` kind ${rw(0)} base ${hx(rw(1))} npages ${rw(2)} raw ${hx(rw(3))} flags ${hx(rw(4))} owner ${o ? i32[o >> 2] + ' ' + tname(o) : 0} shared ${rw(6)} heap ${rw(7)} dir ${hx(rw(8))} guard ${rw(9)} code ${rw(10)} va ${hx(rw(11))} swapped ${rw(12)} lazy ${rw(13)}`);
    console.log(` page index in region ${(va - rw(11)) >> 12}`);
  }
  console.log('kdir', JSON.stringify(pte(kdir, va)));
  const rn = v => { const q = i32[(vpg >> 2) + ((v - uoff) >> 12)]; if (!q) return 'none'; const w = k => i32[(q >> 2) + k]; const o = w(5);
    return `${hx(q)} kind ${w(0)} va ${hx(w(11))} np ${w(2)} flags ${hx(w(4))} owner ${o ? i32[o >> 2] : 0} heap ${w(7)} guard ${w(9)} code ${w(10)} shared ${w(6)} lazy ${w(13)}`; };
  const mm = /task (\d+) /.exec(msg), me = mm ? +mm[1] : 0;
  let tt = i32[sym('task_head') >> 2], g2 = 0, medir = 0;
  while (tt && g2++ < 200) { if (i32[tt >> 2] === me) { const off = printable(str(tt + 151 * 4, 16)) ? 150 : 151; medir = i32[(tt >> 2) + off];
      console.log('faulting task: mmdir', hx(medir), 'its pte', JSON.stringify(pte(medir, va)), 'ckabi', i32[(tt >> 2) + off - 1], 'cons', hx(i32[(tt >> 2) + 16]), 'ncons', i32[(tt >> 2) + 17], 'ndes', i32[(tt >> 2) + 19], 'libs', hx(i32[(tt >> 2) + 20]), 'nsyscalls', i32[(tt >> 2) + 23], 'state', i32[(tt >> 2) + 1], 'sv', i32[(tt >> 2) + 5], 'entry', hx(i32[(tt >> 2) + 2]), 'code', hx(i32[(tt >> 2) + 14]), 'argv_vec', hx(i32[(tt >> 2) + 44 + 200])); }
    tt = i32[(tt >> 2) + 4]; }
  for (const v of [va - 8192, va - 4096, va, va + 4096, va + 8192]) console.log(' region at', hx(v), rn(v));
  console.log(' pc in msg:', /at pc (0x[0-9a-f]+)/.exec(msg)[1]);
  if (r) console.log('r->dir', JSON.stringify(pte(rw(8), va)));
  let t = i32[sym('task_head') >> 2], guard = 0;
  while (t && guard++ < 100) {
    const off = printable(str(t + 151 * 4, 16)) ? 150 : 151;
    const nm = tname(t);
    console.log(`task ${i32[t >> 2]} ${nm} state ${i32[(t >> 2) + 1]} ckabi ${i32[(t >> 2) + off - 1]} ncons ${i32[(t >> 2) + 17]} cons ${hx(i32[(t >> 2) + 16])} nsys ${i32[(t >> 2) + 23]} code ${hx(i32[(t >> 2) + 14])} pc ${hx(i32[(t >> 2) + 9])}`);
    if (nm === 'c4sc') console.log('   its dir:', JSON.stringify(pte(i32[(t >> 2) + off], va)));
    t = i32[(t >> 2) + 4];
  }
  const vb = g('mmu_vbits');
  console.log('vbit', (i32[(vb >> 2) + (idx >> 5)] >>> (idx & 31)) & 1, 'vnext', g('mmu_vnext'), 'idx', idx);
  for (let k = -3; k <= 3; k++) console.log(' idx', idx + k, 'region', hx(i32[(vpg >> 2) + idx + k]), 'kdir', JSON.stringify(pte(kdir, uoff + (idx + k) * 4096)));
  const kn = i32[sym('klog_n') >> 2], kr = sym('klog_ring');
  let kl = ''; for (let k = Math.max(0, kn - 2500); k < kn; k++) kl += String.fromCharCode(u8[kr + (k & 65535)]);
  console.log('klog tail:\n' + kl);
  process.exit(0);
};
typeBytes(vm, new TextEncoder().encode((process.env.CMD || 'b4ke -B -j 3').replace(/;/g, '\n') + '\necho @@done@@\n'));
const n0 = out.length;
for (let i = 0; i < 100000 && !text(n0).includes('@@done@@\n'); i++) vm.run(2e6);
console.log('no kill:', text(n0).split('\n').filter(l => !/Benchmark complete|cycles over| score|^ *$/.test(l)).slice(-30).join('\n'));
process.exit(0);
