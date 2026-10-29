// bench-desktop.mjs [--mmu] [RUNS] -- boot to the desktop, timed, for
// docs/c4ix-mmu.md section 12: C4IX booted as the web page boots it
// (--desktop, a display fitted, the disk read lazily), unpaced, until the
// desktop says it is running on the display. Prints the wall time and the
// machine's cycles to there, the best of RUNS (default 1).
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createMachine, boot } from '../boot.js';
import { STOP } from '../c4m.js';
import { GuiDevice } from '../gui-device.js';

const mmu = process.argv.includes('--mmu');
const runs = Number(process.argv.filter(a => /^\d+$/.test(a))[0] || 1);
const ROOT = fileURLToPath(new URL('../../src/c4bb/images/', import.meta.url));
const index = JSON.parse(readFileSync(join(ROOT, 'disk/index.json'), 'utf8'));
class LazyDisk extends Map {
  constructor(idx) { super(); this.sizes = new Map(idx); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) { if (super.has(n)) return super.get(n); if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(ROOT, 'disk', n))); super.set(n, b); return b; }
}
// C4IX_IMAGE: another kernel than the disk's, built on its own
const image = new Uint8Array(readFileSync(process.env.C4IX_IMAGE || join(ROOT, 'c4ix32.c4r')));
let best = null;
for (let r = 0; r < runs; r++) {
  const out = [];
  const gui = new GuiDevice({ w: 640, h: 480 });
  const vm = createMachine({ arenaMb: 64, mhz: 100, gui, mmu, drives: [{ files: new LazyDisk(index), writable: false, sink: null }], onByte: b => out.push(b) });
  boot(vm, image, ['c4ix32.c4r', '--desktop']);
  const text = () => Buffer.from(out).toString('latin1');
  const t0 = performance.now();
  let ok = false;
  for (let i = 0; i < 20000; i++) {
    if (vm.run(2e6) === STOP.HALT) break;
    gui.drain();
    if (text().includes('desktop: running on the display')) { ok = true; break; }
  }
  const s = (performance.now() - t0) / 1000;
  if (!ok) { console.log(text().slice(-800)); process.exit(1); }
  if (!best || s < best.s) best = { s, cycles: vm.cycle };
}
console.log(`desktop ${mmu ? 'mmu' : 'plain'} ${best.s.toFixed(2)} s ${best.cycles} cycles ${(best.cycles / best.s / 1e6).toFixed(1)} M/s`);
