// try-build.mjs -- the in-C4IX build (cd /usr; b4ke -B -j J) with persist -d running:
// the faults, kills and failures it prints, its wall time, and free before and after.
//   node libjs/tests/try-build.mjs      (MB, MMU=0, CPUS, J, BUDGET; OUT=file for all it printed)
import { readFileSync, writeFileSync } from 'node:fs';
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
boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
const until = (c, b = +(process.env.BUDGET || 30000)) => { for (let i = 0; i < b && !c(); i++) vm.run(2e6); return c(); };
let seq = 0;
const sh = cmds => { const n0 = out.length, mark = `@@${++seq}@@`; typeBytes(vm, new TextEncoder().encode(`${cmds}\necho ${mark}\n`)); until(() => text(n0).includes(mark + '\n')); return text(n0); };
sh(`wait ${3 + +(process.env.CPUS || 4)}`);   // the root archive's unpack: task 4 on one CPU, one more per CPU
console.log(sh('persist -d 60 &\nfree'));
const t0 = Date.now();
const b = sh('cd /usr\nb4ke -B -j ' + (process.env.J || 3) + '\nfree\ncat /proc/meminfo\ndmesg -t');
if (process.env.OUT) writeFileSync(process.env.OUT, b);
const L = b.split('\n');
console.log(L.filter(l => /fault|killed|cannot|fail|b4ke: \/usr|ran,|^mmu:|^swap: .*(cannot|fail)/.test(l)).slice(-40).join('\n'));
console.log('build took', Math.round((Date.now() - t0) / 1000), 's wall');
console.log(b.slice(b.lastIndexOf('total      used') - 20));
console.log(sh('sync\nfree\ncat /proc/meminfo'));
process.exit(0);
