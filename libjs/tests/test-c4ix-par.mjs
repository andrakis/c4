// test-c4ix-par.mjs -- C4IX on two CPUs on threads (par.js, ap.js; cli.js
// --cpus 2 --parallel): a program compiled and linked inside C4IX, with
// libc4ix, and run on each CPU.
//
// cc links against /usr/lib/libc4ix.c4l, which comes with the root
// archive that vfsload unpacks in the background after the first prompt.
// Its task number is the boot's: task 4 on one CPU, one more for each CPU
// past the first (each CPU's idle task comes before init), so a test that
// says "wait 4" on two CPUs waits for vfsload, long gone, and cc races
// the unpack -- "c4rlink: failed to load '/usr/lib/libc4ix.c4l'". It
// waits here for the number the boot line gives, as test-c4ix-swap.mjs
// does. CPUS=N for another count, PARALLEL=0 for the simulated engine.
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { Worker } from 'node:worker_threads';
import { createMachine, boot } from '../boot.js';
import { STOP } from '../c4m.js';
import { typeBytes } from '../keys.js';

const ROOT = new URL('../../src/c4bb/images/', import.meta.url).pathname;
const index = JSON.parse(readFileSync(join(ROOT, 'disk/index.json'), 'utf8'));
class LazyDisk extends Map {
  constructor(idx) { super(); this.sizes = new Map(idx); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) { if (super.has(n)) return super.get(n); if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(ROOT, 'disk', n))); super.set(n, b); return b; }
}
const files = new LazyDisk(index);
// says which CPU ran it, from its own task's record
files.set('where.c', new TextEncoder().encode(`#include "c4ix_user.h"
int main(int argc, char **argv) {
    int info[TASKINFO_WORDS];
    int i, cpu, ncpu;
    cpu = -1; ncpu = 0; i = 0;
    while (utaskinfo(i, info)) { if (info[0] == getpid()) { cpu = info[TI_CPU]; ncpu = info[TI_NCPU]; } ++i; }
    uprintf("where: %s on CPU %d of %d\\n", argc > 1 ? argv[1] : "-", cpu, ncpu);
    return 0;
}
`));

const CPUS = Number(process.env.CPUS || 2);
const out = [];
const vm = createMachine({ arenaMb: 48, mhz: 100, cpus: CPUS, parallel: process.env.PARALLEL !== '0',
  spawn: url => new Worker(url), drives: [{ files, writable: false, sink: null }], onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
// On threads, vm.run comes back at once when CPU 0 naps or parks, so a
// budget of runs was a second or so of real time while CPU 1 unpacked:
// "wait 5" gave up before the archive was out (2026-10-30, with the MMU).
// CPU 0 sleeps and parks for real here, as test-c4ix-guard.mjs and
// cli.js have it do.
const par = vm.smp && vm.smp.parallel ? vm.smp : null;
const until = (cond, budget) => {
  for (let i = 0; i < budget && !cond() && !vm.dev.halted; i++) {
    const r = vm.run(2e6);
    if (!par || vm.dev.halted) continue;
    par.service();
    if (r === STOP.SLEEP && par.othersRunning()) par.sleepSync(vm.dev.napMs);
    else if (r === STOP.PARK || r === STOP.CPUHALT || vm.cpuState !== 'run') par.parkSync(10);
  }
  return cond();
};
let fail = 0;
const check = (ok, what, detail) => {
  console.log(`test-c4ix-par: ${what} ${ok ? 'OK' : 'FAILED'}`);
  if (!ok) { fail = 1; if (detail) console.log(detail.slice(-2000)); }
};
const back = t => /(^|\n)c4ix:[^\n]*\$/.test(t);
const type = s => typeBytes(vm, new TextEncoder().encode(s + '\n'));
// a command, until its own output matches RE, and the prompt after it
const runFor = (c, re, budget) => { const n0 = out.length; type(c); until(() => re.test(text(n0)), budget); until(() => back(text(n0)), 2000); return text(n0); };

until(() => back(text()), 3000);
const unpack = (/unpacking c4ix-root\.tgz in the background as task (\d+)/.exec(text()) || [])[1];
check(unpack === String(3 + CPUS), `the root archive unpacks as task ${unpack}, 3 + the ${CPUS} CPUs`, text());
let t = runFor(`wait ${unpack}`, new RegExp(`\\[${unpack}\\] done`), 40000);
check(new RegExp(`\\[${unpack}\\] done \\(0\\)`).test(t), `wait ${unpack} blocks until it is unpacked`, t);
t = runFor('ls /usr/lib', /libc4ix\.c4l[^]*\$|\$ *$/, 2000);
check(/(^|\n)libc4ix\.c4l\n/.test(t), '/usr/lib/libc4ix.c4l is there', t);

// three times: a link that failed did so every time, not now and then
for (let i = 0; i < 3; i++) {
  t = runFor(`cc -l c4ix -o /tmp/where${i}.c4r where.c`, /c4rlink: wrote|did not link|did not compile|error/, 400000);
  check(/c4rlink: wrote/.test(t) && !/failed to load|did not|error/.test(t), `cc compiles and links where.c with libc4ix (${i + 1} of 3)`, t);
}
for (let k = 0; k < CPUS; k++) {
  t = runFor(`pin ${k} /tmp/where0.c4r pinned`, /where: [^\n]*\n|cannot run/, 40000);
  check(new RegExp(`where: pinned on CPU ${k} of ${CPUS}\\n`).test(t), `it runs on CPU ${k}`, t);
}
check(!/page fault|panic|illegal/.test(text()), 'no fault, no panic', text());
process.exit(fail);
