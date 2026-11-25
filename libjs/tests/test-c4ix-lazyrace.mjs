// test-c4ix-lazyrace.mjs -- a program's free and another's malloc at once.
//
// A program's malloc and free take no kernel lock (sched.c sched_light),
// so on several CPUs they run at the same time, and two races lost a big
// block's entries in the kernel's map (docs/c4ix-mmu.md 11). Two CPUs'
// blocks in one 4 MB of the map each made it a table, and the second put in
// over the first lost what the first had written; and a block's user
// addresses went back to the window (mmu_release) before its entries were
// cleared, so another CPU's malloc could have them and have its own entries
// emptied. A c4sc in a -j 3 build was killed "not mapped" at its block's
// first page.
//
// A program compiled inside C4IX allocates blocks of 2 to 64 KB in a loop,
// writes its tag at every page, looks at them again and frees them. Four
// of them run at once on 4 CPUs, in memory plenty for them all: no swap to
// take the kernel lock, so all of it on the light path. Each must say its
// memory was its own, and none may be killed. PARALLEL=1: on threads.
import { readFileSync } from 'node:fs';
import { Worker } from 'node:worker_threads';
import { join } from 'node:path';
import { createMachine, boot } from '../boot.js';
import { typeBytes } from '../keys.js';
import { STOP } from '../c4m.js';
import { fileURLToPath } from 'node:url';

const ROOT = fileURLToPath(new URL('../../src/c4bb/images/', import.meta.url));
const index = JSON.parse(readFileSync(join(ROOT, 'disk/index.json'), 'utf8'));
class LazyDisk extends Map {
  constructor(idx) { super(); this.sizes = new Map(idx); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) { if (super.has(n)) return super.get(n); if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(ROOT, 'disk', n))); super.set(n, b); return b; }
}
const files = new LazyDisk(index);
files.set('racetest.c', new TextEncoder().encode(`#include "c4ix_user.h"
int main(int argc, char **argv) {
    int rounds, i, k, n, tag, seed;
    int *p[4];
    int size[4];
    char *s;
    rounds = 0;
    s = argv[1];
    while (*s) { rounds = rounds * 10 + *s - '0'; ++s; }
    tag = getpid();
    seed = tag * 7919 + 13;
    i = 0;
    while (i < rounds) {
        k = 0;
        while (k < 4) {
            seed = (seed * 1103515 + 12345) & 0x7fffffff;
            size[k] = 2048 + (seed >> 8) % 62000;
            if (!(p[k] = (int *)malloc(size[k]))) { uprintf("racetest %d: no memory\\n", tag); return 1; }
            n = 0;
            while (n * 4096 < size[k]) { p[k][n * 1024] = tag + n; ++n; }
            ++k;
        }
        k = 0;
        while (k < 4) {
            n = 0;
            while (n * 4096 < size[k]) {
                if (p[k][n * 1024] != tag + n) { uprintf("racetest %d: BAD at round %d\\n", tag, i); return 1; }
                ++n;
            }
            free((char *)p[k]);
            ++k;
        }
        ++i;
    }
    uprintf("racetest %d: fine\\n", tag);
    return 0;
}
`));
const out = [];
const CPUS = Number(process.env.CPUS || 4);
const vm = createMachine({ arenaMb: 64, mhz: 100, mmu: true, cpus: CPUS,
  parallel: !!process.env.PARALLEL, spawn: url => new Worker(url),
  drives: [{ files, writable: false, sink: null }], onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(process.env.C4IX_IMAGE || join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
// (on threads, vm.run comes back at once when CPU 0 naps or parks: it
// sleeps and parks for real here, as test-c4ix-par.mjs has it do)
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
  console.log(`test-c4ix-lazyrace: ${what} ${ok ? 'OK' : 'FAILED'}`);
  if (!ok) { fail = 1; if (detail) console.log(detail.slice(-2000)); }
};
const type = s => typeBytes(vm, new TextEncoder().encode(s + '\n'));
const runFor = (c, re, budget) => { const n0 = out.length; type(c); until(() => re.test(text(n0)), budget); return text(n0); };

until(() => text().includes('c4ix:/$'), 3000);
const unpack = (/unpacking c4ix-root\.tgz in the background as task (\d+)/.exec(text()) || [])[1] || 7;
runFor(`wait ${unpack}`, new RegExp(`\\[${unpack}\\] done`), 40000);
let t = runFor('cc -l c4ix -o /tmp/racetest.c4r racetest.c', /c4rlink: wrote|error|did not compile/, 400000);
check(/c4rlink: wrote/.test(t) && !/error|did not compile/i.test(t), 'racetest.c compiles inside C4IX', t);

const ROUNDS = Number(process.env.ROUNDS || 400);
const n0 = out.length;
type(`/tmp/racetest.c4r ${ROUNDS} &`);
type(`/tmp/racetest.c4r ${ROUNDS} &`);
type(`/tmp/racetest.c4r ${ROUNDS} &`);
type(`/tmp/racetest.c4r ${ROUNDS} &`);
const finished = () => [...text(n0).matchAll(/racetest \d+: (fine|BAD|no memory)|killed/g)].length >= 4;
until(finished, 60000);
t = text(n0);
check(!/killed/.test(t), 'no program is killed', t);
check(!/BAD|no memory/.test(t), 'every page of every block is its own', t);
check((t.match(/racetest \d+: fine/g) || []).length === 4, 'all four finish', t);
console.log(fail ? 'test-c4ix-lazyrace: FAILED' : 'test-c4ix-lazyrace: OK');
process.exit(fail);
