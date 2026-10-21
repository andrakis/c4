// test-c4ix-sigh.mjs -- SIGH (__c4_signal) from a protected task. It sets
// the machine's signal handler, which in C4IX is the kernel's own, so the
// machine traps it now and C4IX gives the task a handler of its own (as a
// C4KE program's signal() does). Before, the task's address replaced the
// kernel's handler, and the next Ctrl-C trapped into it.
//
// A program compiled inside C4IX installs a SIGINT handler with SIGH; a
// Ctrl-C must reach it through C4IX, and then still cancel an ordinary
// job (spin), which only the kernel's own handler does.
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
// the program, as a file only the host disk has (sys_open falls back to it)
files.set('test-sigh.c', new TextEncoder().encode(`#include "c4ix_user.h"
int caught;
// entered through a trap frame: the arguments are the trap's
void on_int(int type, int sig, int mode, int a, int *bp, int *sp, int *returnpc) { caught = sig; }
int main() {
    int t0;
    __c4_signal(2, (int *)&on_int);
    uprintf("sigh: waiting\\n");
    t0 = __time();
    while (!caught && __time() - t0 < 20000) umsleep(10);
    uprintf("sigh: caught %d\\n", caught);
    return 0;
}
`));
const out = [];
const vm = createMachine({ arenaMb: 64, mhz: 100, drives: [{ files, writable: false, sink: null }], onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
const until = (cond, budget) => { for (let i = 0; i < budget && !cond(); i++) vm.run(2e6); return cond(); };
let fail = 0;
const check = (ok, what, detail) => {
  console.log(`test-c4ix-sigh: ${what} ${ok ? 'OK' : 'FAILED'}`);
  if (!ok) { fail = 1; if (detail) console.log(detail.slice(-1200)); }
};
const prompt = from => /(^|\n)c4ix:[^\n]*\$/.test(text(from));
const type = s => typeBytes(vm, new TextEncoder().encode(s + '\n'));
const ctrlC = () => { vm.pendingSignal = 2; };

until(() => text().includes('c4ix:/$'), 3000);
for (const c of ['wait 4', 'cc -l c4ix -o /tmp/sigh.c4r test-sigh.c']) {
  const n0 = out.length; type(c); until(() => prompt(n0), 20000);
}
let n0 = out.length;
type('/tmp/sigh.c4r');
check(until(() => text(n0).includes('sigh: waiting'), 4000), 'a program compiled inside C4IX installs a SIGINT handler with SIGH', text(n0));
ctrlC();
check(until(() => prompt(n0), 4000) && text(n0).includes('sigh: caught 2'), 'Ctrl-C reaches it, through C4IX', text(n0));
n0 = out.length;
type('spin 1000');
until(() => vm.dev.simMs() > 0 && text(n0).length > 8, 400);
for (let i = 0; i < 20; i++) vm.run(2e6);
ctrlC();
check(until(() => prompt(n0), 4000) && /cancelling task \d+/.test(text(n0)),
  'and the kernel\'s own handler is still the machine\'s: Ctrl-C cancels spin', text(n0));
process.exit(fail);
