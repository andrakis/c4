// test-c4ix-softpin.mjs -- a program that takes the display runs on CPU 0, by default.
//
// sched_softpin (sched.c): what draws goes where the display is, unless something
// chose its CPUs, and what it starts may run anywhere. softpin.c takes the display,
// says its CPUs, then starts itself to say its child's.
//
//   node libjs/tests/test-c4ix-softpin.mjs

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
files.set('softpin.c', new TextEncoder().encode(`#include "c4ix_user.h"
int main(int argc, char **argv) {
    char *av[3];
    int pid;
    if (argc > 1) { uprintf("child %d\\n", uaffinity(0, -1)); return 0; }
    uprintf("before %d\\n", uaffinity(0, -1));
    if (!udisplay_take(0)) { uprintf("no display\\n"); return 1; }
    uprintf("self %d\\n", uaffinity(0, -1));
    av[0] = argv[0]; av[1] = "c"; av[2] = 0;
    pid = spawn(argv[0], 2, av);
    if (pid < 0) { uprintf("no child\\n"); return 1; }
    uwait(pid);
    udisplay_give();
    return 0;
}
`));
const out = [];
const vm = createMachine({ arenaMb: 64, mmu: true, cpus: 4,
  drives: [{ files, writable: false, sink: null }, { files: new Map(), writable: true, sink: () => {}, remove: () => {} }],
  onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
const until = (c, b) => { for (let i = 0; i < b && !c() && !vm.dev.halted; i++) vm.run(2e6); return c(); };
let seq = 0;
const sh = c => { const n0 = out.length, mark = `@@${++seq}@@`; typeBytes(vm, new TextEncoder().encode(`${c}\necho ${mark}\n`)); until(() => new RegExp(`^${mark}$`, 'm').test(text(n0)), 3000000); return text(n0); };
let fail = 0;
const check = (ok, what, detail) => { console.log(`test-c4ix-softpin: ${what} ${ok ? 'OK' : 'FAILED'}`); if (!ok) { fail = 1; if (detail) console.log(detail.slice(-800)); } };

until(() => text().includes('c4ix:/$'), 30000);
sh('wait 7');
let t = sh('cc -l c4ix -o /tmp/softpin.c4r softpin.c');
check(/c4rlink: wrote/.test(t), 'softpin.c compiles inside C4IX', t);
t = sh('/tmp/softpin.c4r');
check(/^before 0$/m.test(t) && /^self 1$/m.test(t), 'taking the display puts a program on CPU 0', t);
check(/^child 0$/m.test(t), 'its child may run anywhere', t);
t = sh('pin 2 /tmp/softpin.c4r');
check(/^before 4$/m.test(t) && /^self 4$/m.test(t), 'pin wins: on CPU 2 it stays', t);
check(/^child 4$/m.test(t), 'and pin is inherited, as ever', t);
console.log(fail ? 'test-c4ix-softpin: FAILED' : 'test-c4ix-softpin: OK');
process.exit(fail);
