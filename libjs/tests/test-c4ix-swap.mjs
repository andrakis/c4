// test-c4ix-swap.mjs -- C4IX's swap (docs/c4ix-mmu.md 11): more memory
// than the machine has, the blocks of idle tasks put out to the host's
// swap and brought back when they are touched, every byte as it was.
//
// A program compiled inside C4IX does it:
//   swaptest hold MB SECS   fills MB of blocks of its own, sleeps SECS,
//                           then checks every word
//   swaptest spin MB SECS   as hold, but runs the whole time, touching
//                           one word of its first block and no other
//   swaptest hog MB         fills MB, and checks every word at once
//   swaptest probe          takes blocks of a MB until there are no more,
//                           and says how many: the memory there is
// Two holders take most of the memory and sleep; the hog then wants more
// than is left, which only swap can give it. All three must say their
// memory is intact, and the kernel's log must have blocks going out and
// coming back. While a holder's block is out, the kernel's view of it
// (libjs's user window) is the host's copy, word for word; and when every
// program has gone, the host keeps none of their pages.
//
// CPUS=2 (PARALLEL=1: on threads): two processors, and the holders spin
// rather than sleep, so that pages go while another CPU runs their task
// -- which must forget them first, told by IPI (the TLB's shootdown).
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { createMachine, boot } from '../boot.js';
import { typeBytes } from '../keys.js';
import { Worker } from 'node:worker_threads';

const ROOT = new URL('../../src/c4bb/images/', import.meta.url).pathname;
const index = JSON.parse(readFileSync(join(ROOT, 'disk/index.json'), 'utf8'));
class LazyDisk extends Map {
  constructor(idx) { super(); this.sizes = new Map(idx); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) { if (super.has(n)) return super.get(n); if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(ROOT, 'disk', n))); super.set(n, b); return b; }
}
const files = new LazyDisk(index);
files.set('swaptest.c', new TextEncoder().encode(`#include "c4ix_user.h"
int num(char *s) {
    int v;
    v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + *s - '0'; ++s; }
    return v;
}
void fill(int *p, int words, int seed) {
    int i;
    i = 0;
    while (i < words) { p[i] = seed + i * 7; ++i; }
}
int check(int *p, int words, int seed) {
    int i;
    i = 0;
    while (i < words) { if (p[i] != seed + i * 7) return i; ++i; }
    return -1;
}
int main(int argc, char **argv) {
    int **b;
    int mb, secs, i, t0, seed, bad;
    if (argc > 1 && !ustrcmp(argv[1], "probe")) {
        i = 0;
        while (ualloc(1048576)) ++i;
        uprintf("probe: %d MB\\n", i);
        return 0;
    }
    if (argc < 3) { uprintf("usage: swaptest hold MB SECS | hog MB | probe\\n"); return 2; }
    mb = num(argv[2]);
    seed = getpid() * 1000003;
    b = (int **)ualloc(mb * sizeof(int *));
    i = 0;
    while (i < mb) {
        if (!(b[i] = (int *)ualloc(1048576))) { uprintf("%s: out of memory at %d MB\\n", argv[1], i); return 1; }
        fill(b[i], 262144, seed + i);
        ++i;
    }
    uprintf("%s %d: %d MB filled, the first at 0x%x\\n", argv[1], getpid(), mb, (int)b[0]);
    if (!ustrcmp(argv[1], "hold")) {
        secs = argc > 3 ? num(argv[3]) : 10;
        t0 = __time();
        while (__time() - t0 < secs * 1000) umsleep(200);
    }
    if (!ustrcmp(argv[1], "spin")) {
        secs = argc > 3 ? num(argv[3]) : 10;
        t0 = __time();
        bad = 0;
        while (__time() - t0 < secs * 1000) bad = bad + b[0][0];
    }
    i = 0;
    while (i < mb) {
        if ((bad = check(b[i], 262144, seed + i)) >= 0) {
            uprintf("%s %d: MB %d corrupted at word %d\\n", argv[1], getpid(), i, bad);
            return 1;
        }
        ++i;
    }
    uprintf("%s %d: %d MB intact\\n", argv[1], getpid(), mb);
    return 0;
}
`));

// MB: memory (SWAP_MB). What the programs take is made from what the
// probe finds free: two holders a third of it each, and a hog six tenths,
// so that together they want more than there is by a half, and the hog
// alone could have it.
const MB = Number(process.env.SWAP_MB || 48);
let HOLD = 0, HOG = 0;
const out = [];
const CPUS = Number(process.env.CPUS || 1);
const vm = createMachine({ arenaMb: MB, mhz: 100, mmu: true, cpus: CPUS,
  parallel: !!process.env.PARALLEL, spawn: url => new Worker(url),
  drives: [{ files, writable: false, sink: null }], onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(process.env.C4IX_IMAGE || join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
const until = (cond, budget) => { for (let i = 0; i < budget && !cond() && !vm.dev.halted; i++) vm.run(2e6); return cond(); };
let fail = 0;
const check = (ok, what, detail) => {
  console.log(`test-c4ix-swap: ${what} ${ok ? 'OK' : 'FAILED'}`);
  if (!ok) { fail = 1; if (detail) console.log(detail.slice(-2000)); }
};
const back = t => /(^|\n)c4ix:[^\n]*\$/.test(t);
const prompt = from => back(text(from));
const type = s => typeBytes(vm, new TextEncoder().encode(s + '\n'));
const run = (c, budget = 20000) => { const n0 = out.length; type(c); until(() => prompt(n0), budget); return text(n0); };
// lines of the kernel's log about swap: [task, name, address, KB, out | back in]
const swapLines = t => [...t.matchAll(/swap: (\d+) KB of task (\d+) \(([^)]*)\) at (0x[0-9a-f]+) (out|back in)/g)]
  .map(m => ({ kb: Number(m[1]), task: m[2], name: m[3], va: m[4], dir: m[5] }));

until(() => text().includes('c4ix:/$'), 3000);
run('wait 4', 40000);
let t = run('cc -l c4ix -o /tmp/swaptest.c4r swaptest.c', 40000);
check(back(t) && !/error/i.test(t), 'swaptest.c compiles inside C4IX', t);
t = run('dmesg');
check(/\] swap: the host keeps up to \d+ MB/.test(t), 'the kernel found the host\'s swap', t);
t = run('/tmp/swaptest.c4r probe', 40000);
const free = Number((/probe: (\d+) MB/.exec(t) || [])[1] || 0);
// (with two CPUs both holders spin, and the hog wants nine tenths: more
// than the one not running at that moment can give it)
HOLD = Math.floor(free / 3); HOG = Math.floor(free * (CPUS > 1 ? 9 : 6) / 10);
check(free >= 6, `a probe finds ${free} MB free`, t);

// two holders, in the background, each until it has its memory
const h0 = out.length;
type(`/tmp/swaptest.c4r ${CPUS > 1 ? 'spin' : 'hold'} ${HOLD} 40 &`);
until(() => /(hold|spin) \d+: \d+ MB filled/.test(text(h0)), 20000);
until(() => prompt(h0), 2000);
const h1 = out.length;
type(`/tmp/swaptest.c4r ${CPUS > 1 ? 'spin' : 'hold'} ${HOLD} 40 &`);
until(() => /(hold|spin) \d+: \d+ MB filled|out of memory/.test(text(h1)), 20000);
until(() => prompt(h1), 2000);
check(/(hold|spin) \d+: \d+ MB filled/.test(text(h0)) && /(hold|spin) \d+: \d+ MB filled/.test(text(h1)),
  `two holders have ${HOLD} MB each`, text(h0));

// the hog: more than is left
t = run(`/tmp/swaptest.c4r hog ${HOG}`, 60000);
check(new RegExp(`hog \\d+: ${HOG} MB intact`).test(t), `the hog has ${HOG} MB of a ${MB} MB machine, intact`, t);

// a holder's block put out while the hog ran: the kernel's view of it is
// the host's copy (c4m.js kld32 through the window, kswap), the words as
// the holder wrote them -- block i of a holder is i MB past its first
// (user addresses are handed out in order), its words seed + i + j * 7
const firsts = [...text(h0).matchAll(/(?:hold|spin) (\d+): \d+ MB filled, the first at (0x[0-9a-f]+)/g)].map(m => ({ task: m[1], va: parseInt(m[2], 16) }));
const now = swapLines(run('dmesg'));
let seen = null;
for (const f of firsts) {
  for (const l of now.filter(x => x.task === f.task && x.dir === 'out')) {
    const va = parseInt(l.va, 16), i = (va - f.va) / 0x100000;
    const back = now.some(x => x.task === f.task && x.dir === 'back in' && parseInt(x.va, 16) >= va && parseInt(x.va, 16) < va + l.kb * 1024);
    if (Number.isInteger(i) && i >= 0 && i < HOLD && !back && vm.kswap(va) >= 0) { seen = { task: f.task, va, i }; break; }
  }
  if (seen) break;
}
if (seen) {
  const seed = (Math.imul(Number(seen.task), 1000003) + seen.i) | 0;
  const ok = [0, 1, 1000, 262143].every(j => vm.kld32(seen.va + 4 * j) === ((seed + j * 7) | 0));
  check(ok, `the kernel reads holder ${seen.task}'s block ${seen.i}, which is out, from the host's copy`);
} else check(false, 'a holder\'s block is out while the hog runs', now.map(x => JSON.stringify(x)).join('\n'));

// the holders wake, touch their blocks, and find them as they left them
until(() => (text(h0).match(/(hold|spin) \d+: \d+ MB (intact|corrupted)|corrupted/g) || []).length >= 2, 60000);
const held = text(h0);
check((held.match(/(hold|spin) \d+: \d+ MB intact/g) || []).length === 2 && !/corrupted/.test(held),
  'both holders\' memory came back from swap intact', held);

t = run('dmesg');
const lines = swapLines(t);
const outs = lines.filter(l => l.name === 'swaptest.c4r' && l.dir === 'out').length;
const ins = lines.filter(l => l.name === 'swaptest.c4r' && l.dir === 'back in').length;
check(outs > 0 && ins > 0, `the kernel's log has blocks going out (${outs}) and coming back (${ins})`, t);
// a page at a time: what a fault brings back is its page and those after
// it that are out too, 64 KB at most, never a whole 1 MB block
const biggest = Math.max(0, ...lines.filter(l => l.dir === 'back in').map(l => l.kb));
if (process.env.STAGE !== '8') check(biggest > 0 && biggest <= 64, `pages come back a few at a time (at most ${biggest} KB at once)`, t);
check(!/page fault|panic|nothing is at/.test(text()), 'no fault, no panic', text());
// what the host still keeps is some other task's idle block (the shell's,
// say): the programs have all gone, and their pages with them
if (CPUS > 1) check(/swap: \d+ KB of task \d+ \(swaptest\.c4r\) at 0x[0-9a-f]+ out, the CPU running it told to forget them first \(an IPI\)/.test(t),
  'a page went from a task another CPU was running, which forgot it first (an IPI)', t);
const kept = vm.dev.swapPages.size;
const others = lines.filter(l => l.name !== 'swaptest.c4r' && l.dir === 'out').reduce((n, l) => n + l.kb / 4, 0);
check(kept <= others, `the host keeps ${kept} pages, none of the programs' (others put out: ${others})`,
  lines.map(x => JSON.stringify(x)).join('\n'));
process.exit(fail);
