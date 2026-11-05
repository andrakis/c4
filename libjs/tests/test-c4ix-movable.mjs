// test-c4ix-movable.mjs -- a program's memory moves, and it cannot tell.
//
// A program's image, stack, arguments and small blocks are known to the kernel by
// their user addresses (mmu_adopt), so the frames behind them can change. A big
// program takes the top of the heap; programs started then land below it. When it
// goes there is room above them, and their blocks move up (mmu_raise), while they
// run, on other CPUs. Each checks all the time: its code, by running it and by
// summing it; its globals, a pattern over several pages; its stack, deep recursion
// with a pattern in every frame; its small blocks; its arguments and environment.
// It must find them all as they were, nothing may be killed, the kernel must say
// that such pages moved, and free memory must be in one piece afterwards.
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
files.set('mover.c', new TextEncoder().encode(`#include "c4ix_user.h"
enum { TABLE = 8192, DEEP = 60, FRAME = 40, SMALL = 12 };
int table[TABLE];
char *name;

// code over many pages, every word of it run each round: at some of its page
// boundaries an opcode is one page's last word and its operand the next's first
int stretch(int x) {
${Array(1500).fill('    x = x * 3 + 7;').join('\n')}
    return x;
}

// a stack: a pattern in each frame, looked at on the way back
int deep(int n, int tag) {
    int w[FRAME];
    int i, s;
    i = 0;
    while (i < FRAME) { w[i] = tag + n * 1000 + i; ++i; }
    s = n ? deep(n - 1, tag) : 0;
    i = 0;
    while (i < FRAME) { if (w[i] != tag + n * 1000 + i) return -1000000; ++i; }
    return s < 0 ? s : s + n;
}

int main(int argc, char **argv) {
    int i, k, round, tag, fd, sum, sum0, ok, st0;
    int *code;
    int *b[SMALL];
    char *e;
    if (argc < 3) return 2;
    name = argv[1];
    tag = getpid() * 1000000;
    i = 0;
    while (i < TABLE) { table[i] = tag + i; ++i; }
    st0 = stretch(tag);
    code = (int *)&deep;
    sum0 = 0;
    i = 0;
    while (i < 256) { sum0 = sum0 + code[i]; ++i; }
    round = 0;
    while (1) {
        // told to stop, once there has been time for it all to move
        if ((fd = uopen(argv[2], O_RD)) >= 0) { uclose(fd); break; }
        i = 0;
        while (i < TABLE) { if (table[i] != tag + i) { uprintf("mover %s: BAD table at %d\\n", name, i); return 1; } ++i; }
        sum = 0;
        i = 0;
        while (i < 256) { sum = sum + code[i]; ++i; }
        if (sum != sum0 || stretch(tag) != st0) { uprintf("mover %s: BAD code\\n", name); return 1; }
        if (deep(DEEP, tag) != DEEP * (DEEP + 1) / 2) { uprintf("mover %s: BAD stack\\n", name); return 1; }
        k = 0;
        while (k < SMALL) {
            if (!(b[k] = (int *)malloc(16 << (k % 6)))) { uprintf("mover %s: no memory\\n", name); return 1; }
            b[k][0] = tag + k;
            ++k;
        }
        k = 0;
        while (k < SMALL) {
            if (b[k][0] != tag + k) { uprintf("mover %s: BAD small block\\n", name); return 1; }
            free((char *)b[k]);
            ++k;
        }
        ok = argv[1][0] == name[0] && argv[0][0] == '/';
        if (!ok) { uprintf("mover %s: BAD arguments\\n", name); return 1; }
        ++round;
    }
    uprintf("mover %s: fine, %d rounds\\n", name, round);
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
  console.log(`test-c4ix-movable: ${what} ${ok ? 'OK' : 'FAILED'}`);
  if (!ok) { fail = 1; if (detail) console.log(detail.slice(-1500)); }
};
const type = s => typeBytes(vm, new TextEncoder().encode(s + '\n'));
const runFor = (c, re, budget, ms = 300000) => { const n0 = out.length, t0 = Date.now(); type(c); until(() => re.test(text(n0)) || Date.now() - t0 > ms, budget); return text(n0); };

until(() => text().includes('c4ix:/$'), 30000);
const unpack = (/unpacking c4ix-root\.tgz in the background as task (\d+)/.exec(text()) || [])[1] || 7;
runFor(`wait ${unpack}`, new RegExp(`\\[${unpack}\\] done`), 5000000);
let t = runFor('cc -l c4ix -o /tmp/mover.c4r mover.c', /c4rlink: wrote|error|did not compile/, 5000000);
check(/c4rlink: wrote/.test(t) && !/error|did not compile/i.test(t), 'mover.c compiles inside C4IX', t);
t = runFor('cc -l c4ix -o /tmp/holder.c4r holder.c', /c4rlink: wrote|error|did not compile/, 5000000);
check(/c4rlink: wrote/.test(t) && !/error|did not compile/i.test(t), 'holder.c compiles inside C4IX', t);

const n0 = out.length;
// a big program at the top, movers landing below it
let h = runFor('/tmp/holder.c4r 20 /tmp/go1 &', /holder: holds/, 5000000);
check(/holder: holds/.test(h), 'a big program holds the top of the heap', h);
for (const n of ['a', 'b', 'c']) type(`/tmp/mover.c4r ${n} /tmp/stop &`);
until(() => false, par ? 400 : 500);
// it goes: there is room above them
runFor('mkdir /tmp/go1', /holder: gone/, 5000000);
until(() => false, par ? 800 : 1000);
// again, with another big one, which lands below them and goes
h = runFor('/tmp/holder.c4r 12 /tmp/go2 &', /holder: holds/, 5000000);
until(() => false, par ? 300 : 400);
runFor('mkdir /tmp/go2', /holder: gone/, 5000000);
until(() => false, par ? 800 : 1000);
// a short program comes and goes among them, its stack and all
t = runFor('/bin/echo still-here', /still-here\n/, 5000000);
type('mkdir /tmp/stop');
const t0 = Date.now();
const finished = () => [...text(n0).matchAll(/mover \w: (fine|BAD|no memory)|killed/g)].length >= 3;
until(() => finished() || Date.now() - t0 > 120000, 50000000);
t = text(n0);
check(!/killed/.test(t), 'no program is killed', t);
check(!/BAD|no memory/.test(t), 'code, globals, stack, small blocks and arguments are as they were', t);
check((t.match(/mover \w: fine/g) || []).length === 3, 'all three finish', t);
console.log((t.match(/mover \w: fine, \d+ rounds/g) || []).join('; '));

const d = runFor('dmesg -t\necho @@dmesg@@', /^@@dmesg@@\n/m, 200000);
const moved = [...d.matchAll(/blocks moved up the heap, (\d+) pages of images, stacks and small blocks/g)].reduce((s, m) => s + Number(m[1]), 0);
console.log(`test-c4ix-movable: ${moved} pages of images, stacks and small blocks moved`);
check(moved > 50, 'images, stacks and small blocks moved', d);

// the movers have said so but may not be gone yet: a look or two
let f, tries = 0;
const kv = name => Number((new RegExp(`^${name} (\\d+)`, 'm').exec(f) || [])[1]);
const look = () => { f = runFor('cat /proc/meminfo\necho @@mem@@', /^@@mem@@\n/m, 200000); };
look();
while (tries < 10 && !(kv('free') > 20000 && kv('largest') > kv('free') * 0.8)) { ++tries; until(() => false, par ? 100 : 200); look(); }
console.log(`test-c4ix-movable: ${kv('free')} KB free, ${kv('largest')} KB of it in one run, after ${tries} more looks`);
check(kv('free') > 20000 && kv('largest') > kv('free') * 0.8, 'free memory is in one piece afterwards', f);
check(/^rqlost 0$/m.test(f), 'no task was found off its queue', f);
console.log(fail ? 'test-c4ix-movable: FAILED' : 'test-c4ix-movable: OK');
process.exit(fail);
