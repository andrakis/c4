// test-c4ix-mmu.mjs -- C4IX with libjs's MMU (docs/c4ix-mmu.md): a program
// that writes where it should not is killed by a page fault, and the
// kernel and every other program carry on.
//
// A program compiled inside C4IX does the damage on request:
//   wild victim SECS   keeps a word in a block of its own, says where, and
//                      checks it is untouched for SECS seconds
//   wild kernel        writes into the kernel's code
//   wild poke ADDR     writes at ADDR: the victim's word
//   wild stack         recurses until its stack runs out (the guard page)
//   wild text          writes over its own code (read-only)
//   wild exec          jumps into its own data (no-execute)
//   wild null          reads through a null pointer's neighbourhood
// The victim runs in the background throughout; at the end it must say
// its word was never touched, and the kernel's log (dmesg) must name
// every fault.
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
files.set('wild.c', new TextEncoder().encode(`#include "c4ix_user.h"
int hexval(char *s) {
    int v, c;
    v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s + 2;
    while ((c = *s)) {
        if (c >= '0' && c <= '9') v = v * 16 + c - '0';
        else if (c >= 'a' && c <= 'f') v = v * 16 + c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = v * 16 + c - 'A' + 10;
        else break;
        ++s;
    }
    return v;
}
int deep(int n) {
    int a, b, c, d, e, f, g, h;
    a = n; b = n; c = n; d = n; e = n; f = n; g = n; h = n;
    return deep(n + 1) + a + b + c + d + e + f + g + h;
}
int main(int argc, char **argv) {
    int *p, t0, secs, e;
    int code[4];
    if (argc < 2) { uprintf("usage: wild victim SECS | kernel | poke ADDR | stack | text | exec | null\\n"); return 2; }
    if (!ustrcmp(argv[1], "victim")) {
        secs = 0;
        if (argc > 2) { e = 0; while (argv[2][e]) { secs = secs * 10 + argv[2][e] - '0'; ++e; } }
        if (!secs) secs = 5;
        p = ualloc(64);
        p[0] = 1611526157;
        uprintf("victim: pid %d, its word at 0x%x\\n", getpid(), (int)p);
        t0 = __time();
        while (__time() - t0 < secs * 1000) {
            if (p[0] != 1611526157) { uprintf("victim: corrupted: %d\\n", p[0]); return 1; }
            umsleep(10);
        }
        uprintf("victim: intact\\n");
        return 0;
    }
    if (!ustrcmp(argv[1], "kernel")) {
        p = (int *)8192;
        uprintf("wild: writing into the kernel at 0x%x\\n", (int)p);
        *p = 0;
        uprintf("wild: wrote it\\n");
        return 0;
    }
    if (!ustrcmp(argv[1], "poke") && argc > 2) {
        p = (int *)hexval(argv[2]);
        uprintf("wild: writing at 0x%x\\n", (int)p);
        *p = 47806;
        uprintf("wild: wrote it\\n");
        return 0;
    }
    if (!ustrcmp(argv[1], "stack")) { uprintf("wild: recursing\\n"); return deep(0); }
    if (!ustrcmp(argv[1], "text")) {
        p = (int *)&deep;
        uprintf("wild: writing over its own code at 0x%x\\n", (int)p);
        *p = 0;
        uprintf("wild: wrote it\\n");
        return 0;
    }
    if (!ustrcmp(argv[1], "exec")) {
        code[0] = 1; code[1] = 42; code[2] = 8; code[3] = 8;
        e = (int)code;
        uprintf("wild: jumping into its data at 0x%x\\n", e);
        e();
        uprintf("wild: came back\\n");
        return 0;
    }
    if (!ustrcmp(argv[1], "null")) {
        p = (int *)4096;
        uprintf("wild: reading at 0x%x\\n", (int)p);
        uprintf("wild: read %d\\n", *p);
        return 0;
    }
    return 2;
}
`));

// STAGE: which checks this kernel has the MMU for (docs/c4ix-mmu.md): 1
// translation only, 2 the kernel's memory, 3 another task's, 4 the guard
// page, read-only text and no-execute. Default all.
const stage = Number(process.env.STAGE || 4);
const out = [];
const vm = createMachine({ arenaMb: 64, mhz: 100, mmu: true, drives: [{ files, writable: false, sink: null }], onByte: b => out.push(b) });
// C4IX_IMAGE: another kernel than the disk's, built on its own
boot(vm, new Uint8Array(readFileSync(process.env.C4IX_IMAGE || join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
const until = (cond, budget) => { for (let i = 0; i < budget && !cond(); i++) vm.run(2e6); return cond(); };
let fail = 0;
const check = (ok, what, detail) => {
  console.log(`test-c4ix-mmu: ${what} ${ok ? 'OK' : 'FAILED'}`);
  if (!ok) { fail = 1; if (detail) console.log(detail.slice(-1500)); }
};
// the shell's prompt, after the command's output
const back = t => /(^|\n)c4ix:[^\n]*\$/.test(t);
const prompt = from => back(text(from));
const type = s => typeBytes(vm, new TextEncoder().encode(s + '\n'));
// a command at the prompt, and what it printed
const run = (c, budget = 20000) => { const n0 = out.length; type(c); until(() => prompt(n0), budget); return text(n0); };

until(() => text().includes('c4ix:/$'), 3000);
check(vm.mmu && vm.mmu.on, 'C4IX turned the MMU on');
run('wait 4', 40000);
let t = run('cc -l c4ix -o /tmp/wild.c4r wild.c', 40000);
check(back(t) && !/error/i.test(t), 'wild.c compiles inside C4IX', t);

// the victim, in the background for the whole test
const v0 = out.length;
type('/tmp/wild.c4r victim 8 &');
until(() => /victim: pid \d+, its word at 0x[0-9a-f]+/.test(text(v0)), 4000);
const m = /victim: pid (\d+), its word at (0x[0-9a-f]+)/.exec(text(v0));
check(!!m, 'the victim keeps a word of its own', text(v0));
until(() => prompt(v0), 2000);

const fault = (what, re) => {
  const t = run(`/tmp/wild.c4r ${what}`);
  check(re.test(t) && !/wild: (wrote it|came back|read )/.test(t) && back(t),
    `wild ${what}: killed by a page fault, and the shell carries on`, t);
  return t;
};
if (stage >= 2) fault('kernel', /page fault: write to 0x00002000 \(the kernel's image, not mapped\).*killed/);
if (stage >= 3 && m) fault(`poke ${m[2]}`, new RegExp(`page fault: write to 0x0*${m[2].slice(2)} \\(task wild\\.c4r, not mapped\\).*killed`));
if (stage >= 4) {
  fault('stack', /page fault: write to 0x[0-9a-f]+ \(the stack guard of task wild\.c4r, not mapped\).*killed/);
  fault('text', /page fault: write to 0x[0-9a-f]+ \(task wild\.c4r, not allowed\).*killed/);
  fault('exec', /page fault: jump to 0x[0-9a-f]+ \(task wild\.c4r, not allowed\).*killed/);
}
t = run('echo the kernel is fine');
check(/^the kernel is fine$/m.test(t), 'the kernel carries on', t);
t = run('dmesg');
check(/\] mmu: /.test(t) && (stage < 2 || /\] c4ix: task \d+ \(wild\.c4r\): page fault: write to 0x00002000/.test(t)),
  'dmesg has the MMU coming on, and the faults', t);
// the victim, at the end of its time
until(() => /victim: (intact|corrupted)/.test(text(v0)), 20000);
check(/victim: intact/.test(text(v0)), 'the victim\'s word was never touched', text(v0));
process.exit(fail);
