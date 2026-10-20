// test-c4ix-zombie.mjs - a task whose parent exits first is still reaped.
//
// vfsload starts c4ix-tar to unpack the root archive in the background and
// exits; nobody waits for tar. Booted without `wait`, given time for the
// unpack, ps must list no zombie (docs/c4ix-cpu.md, step 2).
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { createMachine, boot } from '../boot.js';
import { typeBytes } from '../keys.js';

const ROOT = new URL('../../src/c4bb/images/', import.meta.url).pathname;
const index = JSON.parse(readFileSync(join(ROOT, 'disk/index.json'), 'utf8'));
class LazyDisk extends Map {
  constructor(idx) { super(); this.sizes = new Map(idx); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) {
    if (super.has(n)) return super.get(n);
    if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(ROOT, 'disk', n))); super.set(n, b); return b;
  }
}
const out = [];
const vm = createMachine({ arenaMb: 64, drives: [{ files: new LazyDisk(index), writable: false, sink: null }], onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = () => Buffer.from(out).toString('latin1');
let fail = 0;
const check = (ok, what, extra = '') => { console.log(`test-c4ix-zombie: ${what} ${ok ? 'OK' : 'FAILED'}${!ok && extra ? '\n' + extra : ''}`); if (!ok) fail = 1; };

for (let i = 0; i < 3000 && !text().includes('c4ix:/$'); i++) vm.run(2e6);
check(text().includes('c4ix:/$'), 'C4IX boots to its shell');
// the unpack: a few machine-seconds of tar; then ps
const t0 = vm.dev.simMs();
while (vm.dev.simMs() - t0 < 40000) vm.run(2e6);
const n0 = out.length;
typeBytes(vm, new TextEncoder().encode('ps\n'));
for (let i = 0; i < 300 && !/tasks,/.test(Buffer.from(out.slice(n0)).toString('latin1')); i++) vm.run(2e6);
const ps = Buffer.from(out.slice(n0)).toString('latin1');
check(/tasks,/.test(ps), 'ps runs', ps);
check(!/zombie/.test(ps), 'no zombie is left: c4ix-tar was reaped though nobody waited for it', ps);
process.exit(fail);
