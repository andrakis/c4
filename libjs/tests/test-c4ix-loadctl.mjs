// test-c4ix-loadctl.mjs -- more programs than memory, and memory afterwards.
//
// Four programs of 16 MB in 32 MB take turns walking their memory, on threads
// at a nominal 400 MHz, the browser's way of running. Each one's pages go out
// before its next turn, unless load control holds some of them back
// (docs/c4ix-mmu.md 11): which must happen. It measured paging against the
// cycles the clock said a CPU had, and on threads a CPU runs a fraction of what
// it says, so it saw a twentieth where paging took all there was, held nothing,
// and innerbench -Tln 50 never finished. All four must finish with every word as
// they left it. And with them gone free memory must be in one piece: the
// kernel's blocks at the heap's bottom and programs' at its top, where a block
// of the kernel's among programs' would be left in the middle of it.
// PARALLEL=0: on simulated CPUs, where the clock is right.
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
files.set('hog.c', new TextEncoder().encode(`#include "c4ix_user.h"
int main(int argc, char **argv) {
    int mb, n, i, k, tag;
    int *p;
    char *s;
    mb = 0;
    s = argv[1];
    while (*s) { mb = mb * 10 + *s - '0'; ++s; }
    n = mb * 256;
    tag = getpid() * 65536;
    if (!(p = (int *)malloc(mb * 1048576))) { uprintf("hog %d: no memory\\n", getpid()); return 1; }
    k = 0;
    while (k < 3) {
        i = 0;
        while (i < n) {
            if (k && p[i * 1024] != tag + i + k - 1) { uprintf("hog %d: BAD\\n", getpid()); return 1; }
            p[i * 1024] = tag + i + k;
            ++i;
        }
        ++k;
    }
    uprintf("hog %d: fine\\n", getpid());
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
    if (r === STOP.SLEEP && par.othersRunning()) par.sleepSync(vm.dev.napMs);
    else if (r === STOP.PARK || r === STOP.CPUHALT || vm.cpuState !== 'run') par.parkSync(10);
  }
  return cond();
};
let fail = 0;
const check = (ok, what, detail) => {
  console.log(`test-c4ix-loadctl: ${what} ${ok ? 'OK' : 'FAILED'}`);
  if (!ok) { fail = 1; if (detail) console.log(detail.slice(-1500)); }
};
const type = s => typeBytes(vm, new TextEncoder().encode(s + '\n'));
const runFor = (c, re, budget) => { const n0 = out.length; type(c); until(() => re.test(text(n0)), budget); return text(n0); };

until(() => text().includes('c4ix:/$'), 30000);
const unpack = (/unpacking c4ix-root\.tgz in the background as task (\d+)/.exec(text()) || [])[1] || 7;
runFor(`wait ${unpack}`, new RegExp(`\\[${unpack}\\] done`), 5000000);
let t = runFor('cc -l c4ix -o /tmp/hog.c4r hog.c', /c4rlink: wrote|error|did not compile/, 5000000);
check(/c4rlink: wrote/.test(t) && !/error|did not compile/i.test(t), 'hog.c compiles inside C4IX', t);

const n0 = out.length;
for (let i = 0; i < 4; i++) type('/tmp/hog.c4r 16 &');
// (thrashing, they would go on for hours: a real-time limit, for the suite)
const t0 = Date.now();
const finished = () => [...text(n0).matchAll(/hog \d+: (fine|BAD|no memory)|killed/g)].length >= 4;
until(() => finished() || Date.now() - t0 > 300000, 50000000);
if (!finished()) { check(false, 'all four finish in 5 minutes', text(n0)); console.log('test-c4ix-loadctl: FAILED'); process.exit(1); }
t = text(n0);
check(!/killed/.test(t), 'no program is killed', t);
check(!/BAD|no memory/.test(t), 'every word of every program is as it left it', t);
check((t.match(/hog \d+: fine/g) || []).length === 4, 'all four finish', t);

const d = runFor('dmesg -t\necho @@dmesg@@', /@@dmesg@@\n/, 200000);
check(/\bheld, \d+ held/.test(d), 'load control held a task', d);
check(/let go, 0 held/.test(d), '... and let every one go again', d);

runFor('wait\necho @@waited@@', /@@waited@@\n/, 200000);
const f = runFor('cat /proc/meminfo\necho @@mem@@', /@@mem@@\n/, 200000);
const kv = name => Number((new RegExp(`^${name} (\\d+)`, 'm').exec(f) || [])[1]);
const free = kv('free'), big = kv('largest');
console.log(`test-c4ix-loadctl: ${free} KB free, ${big} KB of it in one run`);
check(free > 20000 && big > free * 0.8, 'free memory is in one piece afterwards', f);
check(/^rqlost 0$/m.test(f), 'no task was found off its queue', f);
console.log(fail ? 'test-c4ix-loadctl: FAILED' : 'test-c4ix-loadctl: OK');
process.exit(fail);
