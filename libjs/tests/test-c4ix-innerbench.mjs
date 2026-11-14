// test-c4ix-innerbench.mjs - innerbench runs its benchmarks in C4IX.
//
// innerbench (src/bench/innerbench.c) is a C4KE program: it starts each
// inner benchmark as c4m booting C4KE from source, through C4IX's
// compatibility layer (c4ke.c, ck_spawn), which finds a bare name as
// C4KE does -- NAME, then NAME.c4r. On this disk c4m.c4r is the C4DOS
// build, which C4IX starts and which dies at its first store, printing
// nothing: innerbench said it was spawning c4m, and its benchmarks ended
// at once. It runs /bin/c4m, C4IX's, where there is one -- and /bin/top,
// where it ran C4KE's, whose shares of a 32-bit machine's cycles went
// negative; and stops it when the benchmarks are done.
//
//   node libjs/tests/test-c4ix-innerbench.mjs
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { createMachine, boot } from '../boot.js';
import { typeBytes } from '../keys.js';
import { fileURLToPath } from 'node:url';

const ROOT = fileURLToPath(new URL('../../src/c4bb/images/', import.meta.url));
const DISK = join(ROOT, 'disk');
class LazyDisk extends Map {
  constructor() { super(); this.sizes = new Map(JSON.parse(readFileSync(join(DISK, 'index.json'), 'utf8'))); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) {
    if (super.has(n)) return super.get(n);
    if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(DISK, n))); super.set(n, b); return b;
  }
}
let fail = 0;
const check = (ok, what, extra = '') => {
  console.log(`test-c4ix-innerbench: ${what} ${ok ? 'OK' : 'FAILED'}${!ok && extra ? '\n  ' + String(extra).slice(-1200) : ''}`);
  if (!ok) fail = 1;
};
const out = [];
// Drive 1 as the page has it, one that takes what is written: persist
// keeps the root archive there and C4IX reads its files from it, so the
// tree is not in RAM (persist.c). On a drive that would not take the tar
// (sink: null, as this had it), the tree was unpacked into RAM, 24 MB of
// the 32, and C4KE's stacks barely fitted beside it.
const vm = createMachine({ arenaMb: 32, mmu: true,
  drives: [{ files: new LazyDisk(), writable: false, sink: null },
           { files: new Map(), writable: true, sink: () => {}, remove: () => {} }],
  onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
const until = (c, b = 30000) => { for (let i = 0; i < b && !c(); i++) vm.run(2e6); return c(); };
let seq = 0;
const sh = cmds => {
  const n0 = out.length, mark = `@@${++seq}@@`;
  typeBytes(vm, new TextEncoder().encode(`${cmds}\necho ${mark}\n`));
  until(() => text(n0).includes(mark + '\n'));
  return text(n0);
};

check(until(() => text().includes('c4ix:/$')), 'C4IX boots to its shell, the MMU on');
sh('wait 4');                 // the root archive's unpack (one CPU)
check(/persist: c4ix-root\.tgz: \d+ files, unpacked onto drive 1 and read from there/.test(text()),
  'the root archive is read from drive 1, not unpacked into RAM', text().split('\n').filter(l => /persist/.test(l)).join(' | '));
const t = sh('cd /usr\ninnerbench -n 1');
check(/^innerbench: invoking '\/bin\/c4m load-c4r\.c src\/c4ke\/c4ke\.c -- /m.test(t), 'innerbench starts /bin/c4m, C4IX\'s', t);
check(/Benchmark complete: +\d/.test(t) && /^innerbench complete$/m.test(t) && !/page fault|cannot start/.test(t),
  'its C4KE, booted from source, runs bench to the end', t);
check(/(^|\x1b\[H)top: \d+ tasks, CPU usage \d+% -- Ctrl-C to stop$/m.test(t) && !/c4top|-\d+%/.test(t),
  'top beside it is C4IX\'s, its shares none of them negative', t);
const ps = sh('ps');
check(!/ top$/m.test(ps), 'and is stopped when the benchmarks are done', ps);
process.exit(fail);
