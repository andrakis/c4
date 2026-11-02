// test-c4ix-spawn.mjs - the shell says why a program did not start.
//
// spawn's three failures (include/c4ix_user.h): the program is not there
// ("sh: cannot find"), there was not the memory for it ("sh: out of
// memory starting", the kernel saying how much), or it is there and will
// not load ("sh: cannot load", the kernel saying why). Without the MMU,
// where nothing makes room: the heap taken from outside but for a little,
// c4rlink.c4r (370 KB) does not fit; given back, it runs.
//
//   node libjs/tests/test-c4ix-spawn.mjs
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { createMachine, boot } from '../boot.js';
import { typeBytes } from '../keys.js';

const ROOT = new URL('../../src/c4bb/images/', import.meta.url).pathname;
const index = JSON.parse(readFileSync(join(ROOT, 'disk/index.json'), 'utf8'));
class LazyDisk extends Map {
  constructor(idx) { super(); this.sizes = new Map(idx); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) {
    if (super.has(n)) return super.get(n);
    if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(ROOT, 'disk', n))); super.set(n, b); return b;
  }
}
const out = [];
const vm = createMachine({ arenaMb: 64, mmu: false, drives: [{ files: new LazyDisk(index), writable: false, sink: null }], onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
let fail = 0;
const check = (ok, what, extra = '') => {
  console.log(`test-c4ix-spawn: ${what} ${ok ? 'OK' : 'FAILED'}${!ok && extra ? '\n  ' + String(extra).slice(-1200) : ''}`);
  if (!ok) fail = 1;
};
const until = (cond, budget = 3000) => { for (let i = 0; i < budget && !cond(); i++) vm.run(2e6); return cond(); };
// CMDS typed at the prompt, and what came of them by the time WANT did
const sh = (cmds, want) => {
  const n0 = out.length;
  typeBytes(vm, new TextEncoder().encode(cmds + '\n'));
  until(() => want.test(text(n0)));
  return text(n0);
};

check(until(() => text().includes('c4ix:/$')), 'C4IX boots to its shell, without the MMU');
// the root archive's unpack (task 4) done, so it takes no heap meanwhile
let t = sh('wait 4\necho @@1@@', /^@@1@@$/m);

t = sh('nosuchprog\necho @@2@@', /^@@2@@$/m);
check(/^sh: cannot find nosuchprog$/m.test(t), 'a program that is not there: "sh: cannot find nosuchprog"', t);

t = sh('echo hello > /tmp/notprog.c4r\n/tmp/notprog.c4r\necho @@3@@', /^@@3@@$/m);
check(/^sh: cannot load \/tmp\/notprog\.c4r$/m.test(t) && /not a \.c4r/.test(t),
  'a file that is not a program: "sh: cannot load", the kernel saying why', t);

// all but 160 KB of the heap, held from outside: a MB at a time, then
// 16 KB (the heap is in pieces; no one block is all that is free)
const h = vm.heap, room = () => h.end - h.base - h.used, hog = [];
for (const step of [1 << 20, 16 << 10])
  while (room() > (160 << 10) + step) { const p = h.alloc(step); if (!p) break; hog.push(p); }
const left = room();
t = sh('c4rlink.c4r', /^sh: [^\n]*c4rlink[^\n]*$/m);
for (const p of hog) h.free(p);
check(/^sh: out of memory starting c4rlink\.c4r$/m.test(t) && /out of memory loading c4rlink\.c4r \(\d+ KB\)/.test(t) &&
  !/cannot find|cannot load/.test(t), 'a program there is not the memory for: "sh: out of memory starting", the kernel saying how much',
  `${t}\n(${left} bytes of the heap left)`);

t = sh('c4rlink.c4r\necho @@4@@', /^@@4@@$/m);
check(/Link multiple \.c4r files/.test(t) && !/^sh: /m.test(t), 'with the memory back, it runs', t);

process.exit(fail);
