// test-c4ix-tidy.mjs -- the kernel's own memory, tidied once tasks have gone.
//
// A crowd of programs fills the heap. Then more of what the kernel keeps than its
// arena holds: thousands of directories' vnodes, and with the MMU thousands of
// regions' records, whose slabs come from wherever the heap has room. The crowd goes
// and only a little of it stays. Slabs nothing is in must go back to the heap; a
// region's record still alive must move out of the way (sl4b_tidy, mmu_rmove), with
// its program none the wiser; and free memory must be in one piece. Without the MMU
// programs cannot move: their blocks come from the heap's top instead, the kernel's
// from the bottom (umem_block), so a file written among them is not left in the
// middle when they go.
//
//   node libjs/tests/test-c4ix-tidy.mjs
// PARALLEL=0: on simulated CPUs. MMU=0: without the MMU, on simulated CPUs.

import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { Worker } from 'node:worker_threads';
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
const MMU = process.env.MMU !== '0';
files.set('rgn.c', new TextEncoder().encode(`#include "c4ix_user.h"
// N blocks, each a region of its own, kept until told; then all but the last KEEP,
// which are looked at until told to stop
enum { KEEP = 200 };
int main(int argc, char **argv) {
    int n, i, fd, tag;
    int **b;
    char *s;
    n = 0; s = argv[1];
    while (*s) { n = n * 10 + *s - '0'; ++s; }
    tag = getpid() * 100000;
    if (!(b = (int **)malloc(n * sizeof(int *)))) { uprintf("rgn: no memory\\n"); return 1; }
    i = 0;
    while (i < n) {
        if (!(b[i] = (int *)malloc(3000))) { uprintf("rgn: no memory at %d\\n", i); return 1; }
        b[i][0] = tag + i;
        ++i;
    }
    uprintf("rgn: made %d\\n", n);
    while ((fd = uopen(argv[2], O_RD)) < 0) umsleep(20);
    uclose(fd);
    i = 0;
    while (i < n - KEEP) { free((char *)b[i]); ++i; }
    uprintf("rgn: kept %d\\n", KEEP);
    while ((fd = uopen(argv[3], O_RD)) < 0) {
        i = n - KEEP;
        while (i < n) { if (b[i][0] != tag + i) { uprintf("rgn: BAD block %d\\n", i); return 1; } ++i; }
        umsleep(20);
    }
    uclose(fd);
    i = n - KEEP;
    while (i < n) { if (b[i][0] != tag + i) { uprintf("rgn: BAD block %d\\n", i); return 1; } free((char *)b[i]); ++i; }
    uprintf("rgn: fine\\n");
    return 0;
}
`));
files.set('dirs.c', new TextEncoder().encode(`#include "c4ix_user.h"
// m N: N directories, a score to a parent; r N: gone again
char p[64];
char *num(char *d, int v) {
    if (v >= 10) d = num(d, v / 10);
    *d = '0' + v % 10;
    d[1] = 0;
    return d + 1;
}
void parent(int i) { ustrcpy(p, "/tmp/p"); num(p + 6, i / 20); }
void entry(int i) { char *d; parent(i); d = p; while (*d) ++d; *d = '/'; d[1] = 'e'; num(d + 2, i % 20); }
int main(int argc, char **argv) {
    int n, i, make;
    char *s;
    make = argv[1][0] == 'm';
    n = 0; s = argv[2];
    while (*s) { n = n * 10 + *s - '0'; ++s; }
    i = make ? 0 : n - 1;
    while (make ? i < n : i >= 0) {
        parent(i);
        if (make && i % 20 == 0 && umkdir(p) < 0) { uprintf("dirs: cannot make %s\\n", p); return 1; }
        entry(i);
        if (make ? umkdir(p) < 0 : uunlink(p) < 0) { uprintf("dirs: cannot do %s\\n", p); return 1; }
        if (!make && i % 20 == 0) { parent(i); uunlink(p); }
        i = make ? i + 1 : i - 1;
    }
    uprintf("dirs: done\\n");
    return 0;
}
`));
files.set('big.c', new TextEncoder().encode(`#include "c4ix_user.h"
// w: a file of 256 KB; c: is it still what was written?
enum { WORDS = 65536 };
int main(int argc, char **argv) {
    int fd, i;
    int *b;
    if (!(b = (int *)malloc(WORDS * sizeof(int)))) return 1;
    if (argv[1][0] == 'w') {
        i = 0;
        while (i < WORDS) { b[i] = i * 7; ++i; }
        fd = uopen("/tmp/big", O_WR | O_CREATE);
        if (fd < 0 || write(fd, (char *)b, WORDS * sizeof(int)) != WORDS * sizeof(int)) { uprintf("big: cannot write\\n"); return 1; }
        uclose(fd);
        uprintf("big: wrote\\n");
        return 0;
    }
    fd = uopen("/tmp/big", O_RD);
    if (fd < 0 || uread(fd, (char *)b, WORDS * sizeof(int)) != WORDS * sizeof(int)) { uprintf("big: cannot read\\n"); return 1; }
    uclose(fd);
    i = 0;
    while (i < WORDS) { if (b[i] != i * 7) { uprintf("big: BAD at %d\\n", i); return 1; } ++i; }
    uprintf("big: intact\\n");
    return 0;
}
`));
files.set('holder.c', new TextEncoder().encode(`#include "c4ix_user.h"
// fills MB and sleeps until told to go
int main(int argc, char **argv) {
    int mb, i, fd;
    int *p;
    char *s;
    mb = 0;
    s = argv[1];
    while (*s) { mb = mb * 10 + *s - '0'; ++s; }
    if (!(p = (int *)malloc(mb * 1048576))) { uprintf("holder: no memory\\n"); return 1; }
    i = 0;
    while (i < mb * 256) { p[i * 1024] = i; ++i; }
    uprintf("holder: holds %d MB\\n", mb);
    while ((fd = uopen(argv[2], O_RD)) < 0) umsleep(20);
    uclose(fd);
    uprintf("holder: gone\\n");
    return 0;
}
`));
const out = [];
const vm = createMachine({ arenaMb: 32, mhz: 400, mmu: MMU, cpus: 4, parallel: MMU && process.env.PARALLEL !== '0',
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
    if (r === STOP.SLEEP && par.othersRunning()) par.sleepSync(vm.dev.napMs);
    else if (r === STOP.PARK || r === STOP.CPUHALT || vm.cpuState !== 'run') par.parkSync(10);
  }
  return cond();
};
let fail = 0;
const check = (ok, what, detail) => {
  console.log(`test-c4ix-tidy: ${what} ${ok ? 'OK' : 'FAILED'}`);
  if (!ok) { fail = 1; if (detail) console.log(detail.slice(-1500)); }
};
const type = s => typeBytes(vm, new TextEncoder().encode(s + '\n'));
const runFor = (c, re, budget, ms = 300000) => { const n0 = out.length, t0 = Date.now(); type(c); until(() => re.test(text(n0)) || Date.now() - t0 > ms, budget); return text(n0); };

const name = 'test-c4ix-tidy';
until(() => text().includes('c4ix:/$'), 30000);
const unpack = (/unpacking c4ix-root\.tgz in the background as task (\d+)/.exec(text()) || [])[1] || 7;
runFor(`wait ${unpack}`, new RegExp(`\\[${unpack}\\] done`), 5000000);
for (const p of ['rgn', 'dirs', 'big', 'holder']) {
  const t = runFor(`cc -l c4ix -o /tmp/${p}.c4r ${p}.c`, /c4rlink: wrote|error|did not compile/, 5000000);
  check(/c4rlink: wrote/.test(t) && !/error|did not compile/i.test(t), `${p}.c compiles inside C4IX`, t);
}
const mem = () => {
  const f = runFor('cat /proc/meminfo\necho @@mem@@', /^@@mem@@\n/m, 400000);
  const kv = n => Number((new RegExp(`^${n} (\\d+)`, 'm').exec(f) || [])[1]);
  return { free: kv('free'), largest: kv('largest'), spilled: kv('spilled'), text: f };
};
const n0 = out.length;
// (the root archive's thousands of files are in slabs of their own already, for good)
const base = mem().spilled;
let t = runFor('/tmp/holder.c4r 14 /tmp/go &', /holder: holds/, 5000000);
check(/holder: holds/.test(t), 'a big program holds the heap', t);
t = runFor('/tmp/big.c4r w', /big: (wrote|cannot)/, 5000000);
check(/big: wrote/.test(t), 'a file of 256 KB written among the programs', t);
t = runFor('/tmp/dirs.c4r m 2600', /dirs: (done|cannot)/, 5000000);
check(/dirs: done/.test(t), 'thousands of directories', t);
if (MMU) {
  t = runFor('/tmp/rgn.c4r 5000 /tmp/go /tmp/stop &', /rgn: (made|no memory)/, 5000000);
  check(/rgn: made/.test(t), 'thousands of regions', t);
}
const peak = mem();
console.log(`${name}: at the peak ${peak.spilled} slabs out of the arena, ${peak.free} KB free`);
check(peak.spilled > base, 'slabs came from the heap', peak.text);
// the crowd goes, but for 200 regions made last, whose records are in the last slabs
t = runFor('mkdir /tmp/go', MMU ? /rgn: kept/ : /holder: gone/, 5000000);
t = runFor('/tmp/dirs.c4r r 2600', /dirs: (done|cannot)/, 5000000);
check(/dirs: done/.test(t), 'the directories gone', t);
// a task's going says there is tidying to do; a second or so later it is done
// (200 live regions' records still need 7 slabs of 32 when the arena is full)
const left = MMU ? 8 : 2;
let m = mem();
for (let i = 0; i < 40 && m.spilled > base + left; i++) { until(() => false, par ? 100 : 200); m = mem(); }
console.log(`${name}: ${m.spilled} slabs out of the arena now, against ${base} at the start`);
check(m.spilled <= base + left, 'slabs back to the heap, but for what is still in use', m.text);
const d = runFor('dmesg -t\necho @@dmesg@@', /^@@dmesg@@\n/m, 400000);
const count = re => [...d.matchAll(re)].reduce((s, q) => s + Number(q[1]), 0);
const moved = count(/sl4b: cache 'urgn': (\d+) objects moved/g), given = count(/sl4b: cache '\w+': \d+ objects moved, (\d+) slabs back/g);
console.log(`${name}: ${moved} records moved, ${given} slabs given back`);
check(given > 0, 'the kernel says so', d);
if (MMU) {
  check(moved > 0, 'records of live regions moved', d);
  t = runFor('mkdir /tmp/stop', /rgn: (fine|BAD)/, 5000000);
  check(/rgn: fine/.test(t), 'the program whose regions they were cannot tell', t);
  m = mem();
  for (let i = 0; i < 40 && m.spilled > base + 2; i++) { until(() => false, par ? 100 : 200); m = mem(); }
  console.log(`${name}: ${m.spilled} slabs out of the arena once the last program has gone`);
  check(m.spilled <= base + 2, 'the last slabs back to the heap too', m.text);
}
t = runFor('/tmp/big.c4r c', /big: (intact|BAD|cannot)/, 5000000);
check(/big: intact/.test(t), 'the file is what was written', t);
const all = text(n0);
check(!/killed|BAD|page fault|Trap type/.test(all), 'no program killed, nothing faulted', all);
m = mem();
for (let i = 0; i < 10 && !(m.largest >= m.free * 0.85); i++) { until(() => false, par ? 100 : 200); m = mem(); }
console.log(`${name}: ${m.free} KB free, ${m.largest} KB of it in one run`);
check(m.free > 20000 && m.largest >= m.free * 0.85, 'free memory is in one piece afterwards', m.text);
check(/^rqlost 0$/m.test(m.text), 'no task was found off its queue', m.text);
console.log(fail ? `${name}: FAILED` : `${name}: OK`);
process.exit(fail);
