// try-tiny.mjs -- the desktop's boot in machines of a few MB: whether it starts,
// and what it says of memory where it stops.
//   node libjs/tests/try-tiny.mjs       (SIZES=3,4,5,6,7)
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createMachine, boot } from '../boot.js';
import { GuiDevice } from '../gui-device.js';
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
for (const mb of (process.env.SIZES || '3,4,5,6,7').split(',').map(Number)) {
  const out = [];
  const gui = new GuiDevice({ w: 640, h: 480 });
  const vm = createMachine({ arenaMb: mb, mmu: true, gui, drives: [{ files: new LazyDisk(), writable: false, sink: null }], onByte: b => out.push(b) });
  boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r', '--desktop']);
  const text = () => Buffer.from(out).toString('latin1');
  let halted = false;
  for (let i = 0; i < 3000 && !halted && !/desktop: running/.test(text()); i++) { if (vm.run(2e6) === STOP.HALT) halted = true; gui.drain(); }
  const t = text();
  const lines = t.split('\n').filter(l => /memory|desktop|cannot|panic|fault|Trap/i.test(l)).slice(0, 12);
  console.log(`=== ${mb} MB: halted ${halted}, running ${/desktop: running/.test(t)}, heap ${vm.heap.end - vm.heap.base}\n` + lines.join('\n').slice(0, 900));
}
process.exit(0);
