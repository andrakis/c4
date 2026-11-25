// try-bench.mjs -- C4IX's /bin/bench under libjs, the MMU on (MMU=0 off), on CPUS cpus.
//   node libjs/tests/try-bench.mjs      (MB, CPUS, N runs, IMAGE)
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createMachine, boot } from '../boot.js';
import { typeBytes } from '../keys.js';

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
const vm = createMachine({ arenaMb: +(process.env.MB || 32), mmu: process.env.MMU !== '0', cpus: +(process.env.CPUS || 4),
  drives: [{ files: new LazyDisk(), writable: false, sink: null },
           { files: new Map(), writable: true, sink: (n, b) => store.set(n, b), remove: n => store.delete(n) }],
  onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(process.env.IMAGE || join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
const until = (c, b = 30000) => { for (let i = 0; i < b && !c(); i++) vm.run(2e6); return c(); };
let seq = 0;
const sh = cmds => { const n0 = out.length, mark = `@@${++seq}@@`; typeBytes(vm, new TextEncoder().encode(`${cmds}\necho ${mark}\n`)); until(() => text(n0).includes(mark + '\n')); return text(n0); };
sh(`wait ${3 + +(process.env.CPUS || 4)}`);   // the root archive's unpack: task 4 on one CPU, one more per CPU
const t = sh(`/bin/bench /bin/hello ${process.env.N || 50}`);
console.log(`cpus ${process.env.CPUS || 4} mmu ${process.env.MMU !== '0'}\n` + t.split('\n').filter(l => /^bench:|killed|fault/.test(l)).join('\n'));
process.exit(0);
