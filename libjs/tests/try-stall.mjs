// try-stall.mjs -- the in-C4IX build in the background; every EVERY ms of machine time
// ps, free, /proc/meminfo and the log's size, until LIMIT ms; then the log's tail.
//   node libjs/tests/try-stall.mjs      (MB, EVERY, LIMIT)
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
const vm = createMachine({ arenaMb: +(process.env.MB || 32), mmu: true, cpus: 4,
  drives: [{ files: new LazyDisk(), writable: false, sink: null },
           { files: new Map(), writable: true, sink: (n, b) => store.set(n, b), remove: n => store.delete(n) }],
  onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
const until = (c, b = 30000) => { for (let i = 0; i < b && !c(); i++) vm.run(2e6); return c(); };
let seq = 0;
const sh = (cmds, b) => { const n0 = out.length, mark = `@@${++seq}@@`; typeBytes(vm, new TextEncoder().encode(`${cmds}\necho ${mark}\n`)); const ok = until(() => text(n0).includes(mark + '\n'), b); return (ok ? '' : '(no answer) ') + text(n0); };
sh('wait 7');
const t0 = vm.dev.simMs();
typeBytes(vm, new TextEncoder().encode('cd /usr\nb4ke -B -j 3 > /tmp/b.log 2>&1 &\n'));
const look = () => sh('ps\nfree\ncat /proc/meminfo\nwc /tmp/b.log\ndmesg -t | wc', 3000);
const limit = +(process.env.LIMIT || 9000000);
const every = +(process.env.EVERY || 600000);
let next = every;
for (;;) {
  for (let i = 0; i < 200; i++) vm.run(2e6);
  const ms = vm.dev.simMs() - t0;
  if (ms >= next || ms >= limit) { next += every; console.log(`--- ${Math.round(ms)} ms sim\n` + look().split('\n').slice(-40).join('\n')); }
  if (ms >= limit) break;
}
console.log(sh('dmesg -t', 3000).split('\n').slice(-60).join('\n'));
process.exit(0);
