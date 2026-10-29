// test-c4ix-swap.mjs -- C4IX's swap (docs/c4ix-mmu.md 11): more memory
// than the machine has, the blocks of idle tasks put out to drive 1 and
// brought back when they are touched, every byte as it was.
//
// A program compiled inside C4IX does it:
//   swaptest hold MB SECS   fills MB of blocks of its own, sleeps SECS,
//                           then checks every word
//   swaptest hog MB         fills MB, and checks every word at once
//   swaptest probe          takes blocks of a MB until there are no more,
//                           and says how many: the memory there is
// Two holders take most of the memory and sleep; the hog then wants more
// than is left, which only swap can give it. All three must say their
// memory is intact, the kernel's log must have blocks going out and
// coming back, and drive 1 must be empty of swap at the end.
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { createMachine, boot } from '../boot.js';
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
    uprintf("%s %d: %d MB filled\\n", argv[1], getpid(), mb);
    if (!ustrcmp(argv[1], "hold")) {
        secs = argc > 3 ? num(argv[3]) : 10;
        t0 = __time();
        while (__time() - t0 < secs * 1000) umsleep(200);
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

// drive 1: writable, in memory, what swap writes and removes kept here
const swapFiles = new Map();
const drive1 = { files: swapFiles, writable: true, sink: () => {}, remove: () => {} };

// MB: memory (SWAP_MB). What the programs take is made from what the
// probe finds free: two holders a third of it each, and a hog six tenths,
// so that together they want more than there is by a half, and the hog
// alone could have it.
const MB = Number(process.env.SWAP_MB || 48);
let HOLD = 0, HOG = 0;
const out = [];
const vm = createMachine({ arenaMb: MB, mhz: 100, mmu: true,
  drives: [{ files, writable: false, sink: null }, drive1], onByte: b => out.push(b) });
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

until(() => text().includes('c4ix:/$'), 3000);
run('wait 4', 40000);
let t = run('cc -l c4ix -o /tmp/swaptest.c4r swaptest.c', 40000);
check(back(t) && !/error/i.test(t), 'swaptest.c compiles inside C4IX', t);
t = run('dmesg');
check(/\] swap: on drive 1/.test(t), 'the kernel found drive 1 for swap', t);
t = run('/tmp/swaptest.c4r probe', 40000);
const free = Number((/probe: (\d+) MB/.exec(t) || [])[1] || 0);
HOLD = Math.floor(free / 3); HOG = Math.floor(free * 6 / 10);
check(free >= 6, `a probe finds ${free} MB free`, t);

// two holders, in the background, each until it has its memory
const h0 = out.length;
type(`/tmp/swaptest.c4r hold ${HOLD} 40 &`);
until(() => /hold \d+: \d+ MB filled/.test(text(h0)), 20000);
until(() => prompt(h0), 2000);
const h1 = out.length;
type(`/tmp/swaptest.c4r hold ${HOLD} 40 &`);
until(() => /hold \d+: \d+ MB filled|out of memory/.test(text(h1)), 20000);
until(() => prompt(h1), 2000);
check(/hold \d+: \d+ MB filled/.test(text(h0)) && /hold \d+: \d+ MB filled/.test(text(h1)),
  `two holders have ${HOLD} MB each`, text(h0));

// the hog: more than is left
t = run(`/tmp/swaptest.c4r hog ${HOG}`, 60000);
check(new RegExp(`hog \\d+: ${HOG} MB intact`).test(t), `the hog has ${HOG} MB of a ${MB} MB machine, intact`, t);

// the holders wake, touch their blocks, and find them as they left them
until(() => (text(h0).match(/hold \d+: \d+ MB (intact|corrupted)|corrupted/g) || []).length >= 2, 60000);
const held = text(h0);
check((held.match(/hold \d+: \d+ MB intact/g) || []).length === 2 && !/corrupted/.test(held),
  'both holders\' memory came back from swap intact', held);

t = run('dmesg');
const outs = (t.match(/\] swap: \d+ KB of task \d+ \(swaptest\.c4r\) at 0x[0-9a-f]+ out to drive 1/g) || []).length;
const ins = (t.match(/\] swap: \d+ KB of task \d+ \(swaptest\.c4r\) at 0x[0-9a-f]+ back in/g) || []).length;
check(outs > 0 && ins > 0, `the kernel's log has blocks going out (${outs}) and coming back (${ins})`, t);
check(!/page fault|panic|nothing is at/.test(text()), 'no fault, no panic', text());
// what is still on drive 1 is some other task's idle block (the shell's,
// say), never one of the programs', which have all exited
const mine = new Set([...t.matchAll(/\(swaptest\.c4r\) at 0x[0-9a-f]+ out to drive 1, (c4ix-swap-\d+)/g)].map(m => m[1]));
const left = [...swapFiles.keys()].filter(n => mine.has(n));
check(left.length === 0, `drive 1 has none of the programs' swap left (${swapFiles.size} files there in all)`,
  left.join(' ') + '\n' + (t.match(/[^\n]*swap: [^\n]*/g) || []).join('\n'));
process.exit(fail);
