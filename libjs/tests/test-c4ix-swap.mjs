// test-c4ix-swap.mjs -- C4IX's swap (docs/c4ix-mmu.md 11): more memory
// than the machine has, pages put out to the host's swap and brought back
// when they are touched, every byte as it was.
//
// A program compiled inside C4IX does it:
//   swaptest hold MB SECS   fills MB of blocks of its own, sleeps until
//                           there is a /tmp/swapgo (SECS at most), then
//                           checks every word
//   swaptest spin MB SECS   as hold, but runs the whole time, touching
//                           one word of its first block and no other
//   swaptest hog MB         fills MB, and checks every word at once
// First one task alone asks for half as much again as the whole machine,
// which it is given (a block is not memory until it is used), fills it,
// and finds every word as it left it: its own pages go out to make room
// for its own. Then two holders take most of the memory and sleep, and
// the hog wants more than is left, which only swap can give it. All three
// must say their memory is intact, and the kernel's log must have pages
// going out and coming back. While a holder's page is out, the kernel's
// view of it (libjs's user window) is the host's copy, word for word; and
// when every program has gone, the host keeps none of their pages.
//
// CPUS=2 (PARALLEL=1: on threads): two processors, and the first holder
// spins on CPU 1 rather than sleeping, so that the hog, wanting more than
// the sleeper can give, takes pages of a task another CPU is running --
// which must forget them first, told by IPI or on its way back from the
// kernel (the TLB's shootdown).
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
// the test says when to stop holding (a spinner held for SECS ran all of
// them: on two CPUs a fixed 300 s was most of the test's time)
int told() {
    int st[4];
    return ustat("/tmp/swapgo", st) == 0;
}
int main(int argc, char **argv) {
    int **b;
    int mb, secs, i, t0, seed, bad;
    if (argc < 3) { uprintf("usage: swaptest hold MB SECS | spin MB SECS | hog MB | one MB\\n"); return 2; }
    mb = num(argv[2]);
    if (!ustrcmp(argv[1], "one")) {
        // one block of MB, set by one memset and copied by one memcpy, each
        // a single instruction over more pages than memory holds
        if (!(b = (int **)ualloc(mb * 1048576))) { uprintf("one: out of memory\\n"); return 1; }
        memset((char *)b, 90, mb * 1048576);
        memcpy((char *)b + mb * 524288, (char *)b, 4096);
        i = 0;
        while (i < mb * 262144) {
            if (((int *)b)[i] != 0x5a5a5a5a) { uprintf("one %d: corrupted at word %d\\n", getpid(), i); return 1; }
            ++i;
        }
        memcpy((char *)b + mb * 524288, (char *)b, mb * 524288);
        i = 0;
        while (i < mb * 262144) {
            if (((int *)b)[i] != 0x5a5a5a5a) { uprintf("one %d: corrupted at word %d\\n", getpid(), i); return 1; }
            ++i;
        }
        uprintf("one %d: %d MB in one block intact\\n", getpid(), mb);
        return 0;
    }
    if (!ustrcmp(argv[1], "hog")) uaffinity(getpid(), 1);   // CPU 0, where a spinner is not
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
        while (__time() - t0 < secs * 1000 && !told()) umsleep(200);
    }
    if (!ustrcmp(argv[1], "spin")) {
        uaffinity(getpid(), 2);           // CPU 1, alone there: running whenever swap looks
        secs = argc > 3 ? num(argv[3]) : 10;
        t0 = __time();
        bad = 0;
        // in user mode nearly all the time: the clock only now and then
        while (__time() - t0 < secs * 1000 && !told()) { i = 0; while (i < 1000000) { bad = bad + b[0][0]; ++i; } }
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
// heap has free (its registers, read from here: a program could not tell,
// since what it is given is not memory until it uses it): two holders a
// third of it each, and a hog six tenths, so that together they want more
// than there is by a half, and the hog alone could have it.
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
// a command, and what it printed up to the prompt after it: after its own
// echo, since with two CPUs the last command's prompt can come after it
const run = (c, budget = 20000) => {
  const n0 = out.length;
  type(c);
  const after = () => { const t = text(n0), i = t.indexOf(c); return i >= 0 && back(t.slice(i + c.length)); };
  until(after, budget);
  return text(n0);
};
// ... and one whose own last line RE says it is done (on two CPUs the
// prompts of the commands before it can come after its echo)
const runFor = (c, re, budget) => { const n0 = out.length; type(c); until(() => re.test(text(n0)), budget); until(() => prompt(n0), 2000); return text(n0); };
// lines of the kernel's log about swap: [task, name, address, KB, out | back in]
const swapLines = t => [...t.matchAll(/swap: (\d+) KB of task (\d+) \(([^)]*)\) at (0x[0-9a-f]+) (out|back in)/g)]
  .map(m => ({ kb: Number(m[1]), task: m[2], name: m[3], va: m[4], dir: m[5] }));

until(() => text().includes('c4ix:/$'), 3000);
// the root archive's unpack, which brings /usr/lib/libc4ix.c4l: task 4 on
// one CPU, but each CPU's idle task comes before init, so 5 on two -- and
// "wait 4" there waited for vfsload, long gone, and cc raced the unpack
// ("c4rlink: failed to load '/usr/lib/libc4ix.c4l'"; test-c4ix-par.mjs)
const unpack = (/unpacking c4ix-root\.tgz in the background as task (\d+)/.exec(text()) || [])[1] || 4;
runFor(`wait ${unpack}`, new RegExp(`\\[${unpack}\\] done`), 40000);
let t = runFor('cc -l c4ix -o /tmp/swaptest.c4r swaptest.c', /c4rlink: wrote|error|did not compile/, 400000);
check(back(t) && /c4rlink: wrote/.test(t) && !/error|did not compile/i.test(t), 'swaptest.c compiles inside C4IX', t);
t = run('dmesg');
check(/\] swap: the host keeps up to \d+ MB/.test(t), 'the kernel found the host\'s swap', t);
const free = Math.floor((vm.dev.read32(0x148) - vm.dev.read32(0x144) - vm.dev.read32(0x1c0)) / 1048576);

// one task alone, half as much again as the whole machine: its own pages
// go out for its own, and come back as it checks them
const BIG = Math.floor(MB * 3 / 2);
t = runFor(`/tmp/swaptest.c4r hog ${BIG}`, /hog \d+: \d+ MB (intact|corrupted)|out of memory|corrupted/, 200000);
check(new RegExp(`hog \\d+: ${BIG} MB intact`).test(t), `one task has ${BIG} MB of a ${MB} MB machine, intact`, t);
const alone = swapLines(run('dmesg')).filter(l => l.name === 'swaptest.c4r');
check(alone.some(l => l.dir === 'out') && alone.some(l => l.dir === 'back in'),
  `its own pages went out (${alone.filter(l => l.dir === 'out').length} runs) and came back (${alone.filter(l => l.dir === 'back in').length})`);
// ... and as one block, which one memset fills and one memcpy copies: an
// instruction over more pages than memory holds, which goes on from where
// its fault stopped it rather than from its first byte (libjs mmu.js block)
t = runFor(`/tmp/swaptest.c4r one ${BIG}`, /one \d+: \d+ MB in one block intact|out of memory|corrupted/, 200000);
check(new RegExp(`one \\d+: ${BIG} MB in one block intact`).test(t), `one block of ${BIG} MB, set and copied whole, intact`, t);
// (with two CPUs the first holder spins, and the hog wants nine tenths:
// more than the sleeper can give it)
HOLD = Math.floor(free / 3); HOG = Math.floor(free * (CPUS > 1 ? 9 : 6) / 10);
check(free >= 6, `the heap has ${free} MB free`);

// two holders, in the background, each until it has its memory
const h0 = out.length;
type(`/tmp/swaptest.c4r ${CPUS > 1 ? 'spin' : 'hold'} ${HOLD} 300 &`);
until(() => /(hold|spin) \d+: \d+ MB filled/.test(text(h0)), 20000);
until(() => prompt(h0), 2000);
const h1 = out.length;
type(`/tmp/swaptest.c4r hold ${HOLD} 300 &`);
until(() => /(hold|spin) \d+: \d+ MB filled|out of memory/.test(text(h1)), 20000);
until(() => prompt(h1), 2000);
check(/(hold|spin) \d+: \d+ MB filled/.test(text(h0)) && /(hold|spin) \d+: \d+ MB filled/.test(text(h1)),
  `two holders have ${HOLD} MB each`, text(h0));

// the hog: more than is left
t = runFor(`/tmp/swaptest.c4r hog ${HOG}`, /hog \d+: \d+ MB (intact|corrupted)|out of memory|corrupted/, 60000);
check(new RegExp(`hog \\d+: ${HOG} MB intact`).test(t), `the hog has ${HOG} MB of a ${MB} MB machine, intact`, t);

// a page of a holder's put out while the hog ran: the kernel's view of it
// is the host's copy (c4m.js kld32 through the window, kswap), the words
// as the holder wrote them -- block i of a holder is i MB past its first
// (user addresses are handed out in order), word j of it seed + i + j * 7
const firsts = [...text(h0).matchAll(/(?:hold|spin) (\d+): \d+ MB filled, the first at (0x[0-9a-f]+)/g)].map(m => ({ task: m[1], va: parseInt(m[2], 16) }));
let seen = null;
for (const f of firsts) {
  for (let i = 0; i < HOLD && !seen; i++) {
    for (let p = f.va + i * 0x100000; p < f.va + (i + 1) * 0x100000; p += 4096) {
      if (vm.kswap(p) >= 0) { seen = { task: f.task, block: f.va + i * 0x100000, p, i }; break; }
    }
  }
  if (seen) break;
}
if (seen) {
  const seed = (Math.imul(Number(seen.task), 1000003) + seen.i) | 0, j0 = (seen.p - seen.block) / 4;
  const ok = [0, 1, 500, 1023].every(j => vm.kld32(seen.p + 4 * j) === ((seed + (j0 + j) * 7) | 0));
  check(ok, `the kernel reads holder ${seen.task}'s page at 0x${seen.p.toString(16)}, which is out, from the host's copy`);
} else check(false, 'a page of a holder\'s is out after the hog has run', swapLines(run('dmesg')).map(x => JSON.stringify(x)).join('\n'));

// the holders wake, touch their blocks, and find them as they left them
type('echo go > /tmp/swapgo');
until(() => (text(h0).match(/(hold|spin) \d+: \d+ MB (intact|corrupted)|corrupted/g) || []).length >= 2, 200000);
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
// The CPU is sent an IPI if it is in user mode then; if a tick has it
// waiting for the kernel lock the swapping CPU holds (which it holds
// while it looks for pages nobody has used), it forgets them on its way
// back instead. Which one is the interleaving's: either is the shootdown.
if (CPUS > 1) {
  const ipi = /swap: \d+ KB of task \d+ \(swaptest\.c4r\) at 0x[0-9a-f]+ out, the CPU running it told to forget them first \(an IPI\)/.test(t);
  const later = /swap: \d+ KB of task \d+ \(swaptest\.c4r\) at 0x[0-9a-f]+ out, the CPU running it, in the kernel, told to forget them on its way back/.test(t);
  check(ipi || later, `a page went from a task another CPU was running, which forgot it first (${[ipi && 'an IPI', later && 'on its way back'].filter(Boolean).join(', ')})`,
    t.split('\n').filter(l => /swap: .* out/.test(l)).join('\n'));
}
// What the host still keeps is pages of tasks still there (the shell's,
// say: one put out, or one the kernel wrote before the shell touched it):
// each is a page the kernel's map has out, at its slot. The programs have
// all gone, and their pages with them.
const outSlots = new Set();
for (let p = 0x10000000; p < 0x20000000; p += 4096) { const s = vm.kswap(p); if (s >= 0) outSlots.add(s); }
const kept = [...vm.dev.swapPages.keys()];
check(kept.every(s => outSlots.has(s)), `the host keeps ${kept.length} pages, each a page out of a task still there (${outSlots.size} out)`,
  kept.filter(s => !outSlots.has(s)).join(' '));
process.exit(fail);
