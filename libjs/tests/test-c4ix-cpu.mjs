// test-c4ix-cpu.mjs - C4IX's CPU accounting, as top reports it.
//
// docs/c4ix-cpu.md, steps 3 and 4. Every share must be within 0..100,
// and a screen's rows -- the tasks, the idle task, and System -- must
// add up to exactly 100. An idle machine is at least 95% idle, which it
// only is once the time the kernel naps is counted; and a busy one does
// not overflow, as a task that ran 21M cycles in a second once did.
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


// top N's screens, from what it printed after output offset n0: each
// task's row, and the System line
const row = /^\s*(\d+)\s+(\d+)\s+(\S+)\s+(user|kernel)\s+(-?\d+)\s.*?(\S+)\s*$/;
const sysrow = /^\s+(-?\d+)\s+\S+\s+System\s*$/;
const top = n => {
  const n0 = out.length;
  type(`top ${n}\n`);
  until(() => (text(n0).match(/top: \d+ tasks/g) || []).length >= n && /c4ix:\/\$ *$/.test(text(n0)), 3000);
  if (process.env.SHOW) console.log(text(n0));
  return text(n0).split(/top: (?=\d+ tasks)/).slice(1).map(s => {
    const lines = s.split('\n');
    const rows = lines.map(l => row.exec(l)).filter(Boolean).map(m => ({ id: +m[1], pct: +m[5], name: m[6] }));
    const sys = lines.map(l => sysrow.exec(l)).filter(Boolean).map(m => +m[1]);
    return { s, rows, sys: sys.length === 1 ? sys[0] : null, usage: +(/CPU usage (\d+)%/.exec(s) || [])[1] };
  });
};
const sum = sc => sc.rows.reduce((a, r) => a + r.pct, 0) + sc.sys;
const valid = sc => sc.rows.length >= 3 && sc.sys !== null && [...sc.rows.map(r => r.pct), sc.sys].every(p => p >= 0 && p <= 100);

check(until(() => text().includes('c4ix:/$'), 3000), 'C4IX boots to its shell');
// the root archive's unpack first, which would otherwise be busy too
type('wait 3\n');
until(() => text().includes('[3] done'), 3000);

// An idle machine is idle: the kernel naps, and the naps count.
let screens = top(2);
check(screens.length === 2, 'top prints two screens', screens.map(s => s.s).join(''));
screens.forEach((sc, k) => {
  const idle = sc.rows.find(r => r.id === 0);
  check(valid(sc), `idle, screen ${k + 1}: every share is within 0..100, with a System line`, sc.s);
  check(sum(sc) === 100, `idle, screen ${k + 1}: the rows add up to 100 (${sum(sc)})`, sc.s);
  check(idle && idle.pct >= 95 && sc.usage <= 5, `idle, screen ${k + 1}: the idle task has at least 95% (${idle && idle.pct}), usage ${sc.usage}%`, sc.s);
});

// A busy one: before step 3 a task that ran 21M cycles in a second
// overflowed, and top showed negative and huge percentages.
type('c4ix-spin.c4r 1000 > /ram/spin.txt &\n');
for (let i = 0; i < 50; i++) vm.run(2e6);
screens = top(3);
check(screens.length === 3, 'top prints three screens with spin running', screens.map(s => s.s).join(''));
screens.forEach((sc, k) => {
  const spin = sc.rows.find(r => /spin/.test(r.name));
  check(valid(sc), `busy, screen ${k + 1}: every share is within 0..100, with a System line`, sc.s);
  check(sum(sc) === 100, `busy, screen ${k + 1}: the rows add up to 100 (${sum(sc)})`, sc.s);
  check(spin && spin.pct > 0, `busy, screen ${k + 1}: spin is there, and busy`, sc.s);
});
process.exit(fail);
