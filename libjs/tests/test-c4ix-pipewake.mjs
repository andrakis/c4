// test-c4ix-pipewake.mjs -- a reader on one CPU is woken by a writer on another.
//
// A task asleep in SYS_EVWAIT for its pipes (the desktop's loop) is looked at
// each ms, and its pipes are walked only once some pipe has had data or lost
// its last writer since (sched_pipegen, sched.c); a task blocked in read is on
// its pipe's own list (sched_pipewake). Either way a write it misses leaves it
// asleep: until its timeout, or until the watch finds it (rqlost). So a writer
// pinned to CPU 1 writes 200 lines 3 ms apart to a reader pinned to CPU 2,
// which waits for each in uevwait, then in read: every line must arrive, no
// wake may find nothing (the reader's timeout is 2 s), the whole must take
// about 600 ms of the machine's time, not seconds, and rqlost stays 0. (The
// writer's sleeps check one more wakeup: a task asleep on the clock, pinned to
// a CPU other than 0, once woke up to 100 ms late, the next nap of CPU 0's idle
// loop, which woke it but had no way to tell its CPU: each 3 ms took 100.)
// PARALLEL=0: on simulated CPUs.
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { Worker } from 'node:worker_threads';
import { createMachine, boot } from '../boot.js';
import { typeBytes } from '../keys.js';
import { STOP } from '../c4m.js';

const ROOT = new URL('../../src/c4bb/images/', import.meta.url).pathname;
const index = JSON.parse(readFileSync(join(ROOT, 'disk/index.json'), 'utf8'));
class LazyDisk extends Map {
  constructor(idx) { super(); this.sizes = new Map(idx); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) { if (super.has(n)) return super.get(n); if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(ROOT, 'disk', n))); super.set(n, b); return b; }
}
const files = new LazyDisk(index);
files.set('pw.c', new TextEncoder().encode(`#include "c4ix_user.h"
// pw w MASK N MS: N lines to stdout, MS ms apart, on the CPUs in MASK
// pw r MASK: stdin read as the desktop reads its pipes, waiting in uevwait
// pw b MASK: stdin read in read itself, which blocks
int num(char *s) {
    int v;
    v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + *s - '0'; ++s; }
    return v;
}

int main(int argc, char **argv) {
    char buf[64];
    int i, n, ms, got, lines, empty, wakes, k;
    if (argc < 3) return 2;
    uaffinity(0, num(argv[2]));
    if (argv[1][0] == 'w') {
        if (argc < 5) return 2;
        n = num(argv[3]);
        ms = num(argv[4]);
        i = 0;
        while (i < n) { ufprintf(1, "x\\n"); umsleep(ms); ++i; }
        return 0;
    }
    lines = 0; empty = 0; wakes = 0;
    while (1) {
        if (argv[1][0] == 'r') {
            uevwait(2000, 0, 1);
            ++wakes;
            if (!uavail(0)) { ++empty; continue; }
        }
        if ((got = uread(0, buf, 64)) <= 0) break;
        k = 0;
        while (k < got) { if (buf[k] == 10) ++lines; ++k; }
    }
    uprintf("pw %s: %d lines, %d wakes, %d with nothing\\n", argv[1], lines, wakes, empty);
    return 0;
}
`));
const out = [];
const vm = createMachine({ arenaMb: 32, mhz: 400, mmu: true, cpus: 4, parallel: process.env.PARALLEL !== '0',
  spawn: url => new Worker(url),
  drives: [{ files, writable: false, sink: null },
           { files: new Map(), writable: true, sink: () => {}, remove: () => {} }], onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(process.env.C4IX_IMAGE || join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
const par = vm.smp && vm.smp.parallel ? vm.smp : null;
const until = (cond, budget) => {
  for (let i = 0; i < budget && !cond() && !vm.dev.halted; i++) {
    const r = vm.run(2e6);
    if (!par || vm.dev.halted) continue;
    par.service();
    // CPU 0 naps while the others work as cli.js has it: for real, until an
    // IPI, and the machine's clock gets only what it slept. (The clock took
    // the whole nap as it began, so a nap an IPI ended early would count in
    // full, and a wakeup that came in time would look 100 ms late.) And an
    // IPI already come, from a CPU that has parked since, ends it at once.
    if (r === STOP.SLEEP && (par.othersRunning() || par.ipiFor0())) {
      const nap = vm.dev.napMs;
      let slept = 0;
      while (slept < nap && !par.ipiFor0() && !vm.dev.halted) slept += par.sleepSync(nap - slept);
      if (slept < nap) vm.dev.sleepMs -= nap - slept;
    } else if (r === STOP.PARK || r === STOP.CPUHALT || vm.cpuState !== 'run') par.parkSync(10);
  }
  return cond();
};
let fail = 0;
const check = (ok, what, detail) => {
  console.log(`test-c4ix-pipewake: ${what} ${ok ? 'OK' : 'FAILED'}`);
  if (!ok) { fail = 1; if (detail) console.log(detail.slice(-1500)); }
};
const type = s => typeBytes(vm, new TextEncoder().encode(s + '\n'));
const runFor = (c, re, budget, ms = 300000) => { const n0 = out.length, t0 = Date.now(); type(c); until(() => re.test(text(n0)) || Date.now() - t0 > ms, budget); return text(n0); };

until(() => text().includes('c4ix:/$'), 30000);
const unpack = (/unpacking c4ix-root\.tgz in the background as task (\d+)/.exec(text()) || [])[1] || 7;
runFor(`wait ${unpack}`, new RegExp(`\\[${unpack}\\] done`), 5000000);
let t = runFor('cc -l c4ix -o /tmp/pw.c4r pw.c', /c4rlink: wrote|error|did not compile/, 5000000);
check(/c4rlink: wrote/.test(t) && !/error|did not compile/i.test(t), 'pw.c compiles inside C4IX', t);

for (const [mode, how] of [['r', 'asleep in uevwait'], ['b', 'blocked in read']]) {
  const m0 = vm.dev.simMs();
  t = runFor(`/tmp/pw.c4r w 2 200 3 | /tmp/pw.c4r ${mode} 4\necho @@${mode}@@`, new RegExp(`^@@${mode}@@\\n`, 'm'), 5000000);
  const ms = Math.round(vm.dev.simMs() - m0);
  const m = new RegExp(`pw ${mode}: (\\d+) lines, (\\d+) wakes, (\\d+) with nothing`).exec(t);
  console.log(`test-c4ix-pipewake: ${how}: ${m ? m[0] : 'no report'}, ${ms} machine ms`);
  check(m && m[1] === '200', `every line reaches a reader ${how}, from another CPU`, t);
  check(m && m[3] === '0', `no wake of a reader ${how} finds nothing`, t);
  check(ms < 1800, `nothing waits out a timeout (${ms} ms for 200 lines 3 ms apart)`, t);
}
const f = runFor('cat /proc/meminfo\necho @@mem@@', /^@@mem@@\n/m, 200000);
check(/^rqlost 0$/m.test(f), 'no task was found off its queue or asleep with its pipe ready', f);
console.log(fail ? 'test-c4ix-pipewake: FAILED' : 'test-c4ix-pipewake: OK');
process.exit(fail);
