// try-inner.mjs -- C4IX booted with the MMU on 4 CPUs; its shell runs CMDS, split at |,
// and each command's output is printed with its machine time.
//   CMDS='cd /usr|innerbench -n 1' node libjs/tests/try-inner.mjs
// MB, BUDGET; PIN=ms pins Date to simulated time from there; HEAP=1 ends with
// heapdump's map; LIVE=1 prints the output as it comes, and says when it is quiet.
import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createMachine, boot } from '../boot.js';
import { typeBytes } from '../keys.js';
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
let shown = 0;
const until = (c, b = +(process.env.BUDGET || 30000)) => {
  for (let i = 0; i < b && !c(); i++) {
    vm.run(2e6);
    if (!process.env.LIVE || i % 50 != 49) continue;
    const t = text();
    if (t.length > shown) { process.stdout.write(t.slice(shown)); shown = t.length; }
    else if (i % 500 == 499) process.stdout.write(`[quiet at ${i}, ${Math.round(vm.dev.simMs())} ms]\n`);
  }
  return c();
};
let seq = 0;
const sh = cmds => { const n0 = out.length, mark = `@@${++seq}@@`; typeBytes(vm, new TextEncoder().encode(`${cmds}\necho ${mark}\n`)); until(() => text(n0).includes(mark + '\n')); return text(n0); };
sh('wait 7');

const cmds = (process.env.CMDS || '').split('|');
for (const c of cmds) { const t0 = vm.dev.simMs(); const o = sh(c); console.log('$ ' + c + '   [' + Math.round(vm.dev.simMs() - t0) + ' ms]\n' + o.split('\n').slice(-4000).join('\n')); }
if (process.env.HEAP) console.log(heapDump(vm));
process.exit(0);
