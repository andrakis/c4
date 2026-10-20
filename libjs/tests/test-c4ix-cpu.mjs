// test-c4ix-cpu.mjs - C4IX's CPU accounting, as top reports it.
//
// A busy machine is where the numbers used to go wrong: a task that ran
// more than 21M cycles in a second overflowed 32 bits once multiplied by
// 100, and top showed negative or huge percentages (docs/c4ix-cpu.md,
// step 3). With spin running, top's CPU% must stay within 0..100 and add
// up to about 100.
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
const vm = createMachine({ arenaMb: 64, mhz: 100, drives: [{ files: new LazyDisk(index), writable: false, sink: null }], onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
const until = (cond, budget) => { for (let i = 0; i < budget && !cond(); i++) vm.run(2e6); return cond(); };
let fail = 0;
const check = (ok, what, extra = '') => { console.log(`test-c4ix-cpu: ${what} ${ok ? 'OK' : 'FAILED'}${!ok && extra ? '\n' + extra : ''}`); if (!ok) fail = 1; };
const type = s => typeBytes(vm, new TextEncoder().encode(s));

check(until(() => text().includes('c4ix:/$'), 3000), 'C4IX boots to its shell');
// the root archive's unpack first, which would otherwise be busy too
type('wait 3\n');
until(() => text().includes('[3] done'), 3000);
// spin, quietly: its progress would interleave with top's screens
type('c4ix-spin.c4r 1000 > /ram/spin.txt &\n');
for (let i = 0; i < 50; i++) vm.run(2e6);
const n0 = out.length;
type('top 3\n');
// three screens, each a header line and a table
until(() => (text(n0).match(/top: \d+ tasks/g) || []).length >= 3 && /c4ix:\/\$ *$/.test(text(n0)), 3000);
const screens = text(n0).split(/top: (?=\d+ tasks)/).slice(1);
check(screens.length === 3, 'top prints three screens', text(n0));
const row = /^\s*(\d+)\s+(\d+)\s+(\S+)\s+(user|kernel)\s+(-?\d+)\s/;
screens.forEach((s, k) => {
  const pcts = s.split('\n').map(l => row.exec(l)).filter(Boolean).map(m => +m[5]);
  const sum = pcts.reduce((a, b) => a + b, 0);
  check(pcts.length >= 3 && pcts.every(p => p >= 0 && p <= 100), `screen ${k + 1}: every CPU% is within 0..100`, s);
  check(sum >= 100 - pcts.length && sum <= 100 + pcts.length, `screen ${k + 1}: CPU% adds up to about 100 (${sum})`, s);
});
process.exit(fail);
