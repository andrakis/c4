// test-c4ix-c4m.mjs -- c4m as a C4IX program (/bin/c4m, c4m.c built as
// cc builds it): it runs a program compiled inside C4IX, and boots C4KE
// from /usr/lib/c4ke.c4r to its shell, and \q comes back to C4IX's.
// Paced, one command at a time: a pipe fed ahead would be read by
// whichever program asked first (drive.mjs's rule).
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
const out = [];
const vm = createMachine({ arenaMb: 64, mhz: 100, drives: [{ files: new LazyDisk(index), writable: false, sink: null }], onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
const until = (cond, budget) => { for (let i = 0; i < budget && !cond(); i++) vm.run(2e6); return cond(); };
let fail = 0;
const check = (ok, what, detail) => {
  console.log(`test-c4ix-c4m: ${what} ${ok ? 'OK' : 'FAILED'}`);
  if (!ok) { fail = 1; if (detail) console.log(detail.slice(-1200)); }
};
// the console shell's prompt ends its line ("c4ix:/usr$" and a newline)
const prompt = from => /(^|\n)c4ix:[^\n]*\$/.test(text(from));
const type = (s, from) => typeBytes(vm, new TextEncoder().encode(s + '\n'));

until(() => text().includes('c4ix:/$'), 3000);
for (const c of ['wait 4', 'cd /usr', 'cc -o /tmp/h.c4r /usr/src/c4ix/examples/hello.c']) {
  const n0 = out.length; type(c); until(() => prompt(n0), 20000);
}
let n0 = out.length;
type('c4m load-c4r.c -- /tmp/h.c4r one');
check(until(() => prompt(n0), 20000) && text(n0).includes('hello from a program compiled inside C4IX')
  && text(n0).includes('argv[1] = one'), 'c4m runs a program compiled inside C4IX', text(n0));
n0 = out.length;
const t0 = vm.dev.simMs();
type('c4m load-c4r.c -- /usr/lib/c4ke.c4r');
const up = until(() => text(n0).includes('c4sh>'), 60000);
check(up && text(n0).includes('Kernel ready') && /vfsload: (\d+)\/\1 entries loaded/.test(text(n0)),
  'c4m boots C4KE to its shell, its VFS loaded', text(n0));
console.log(`  (C4KE up in ${((vm.dev.simMs() - t0) / 1000).toFixed(1)} machine-seconds)`);
// C4KE's shell polls /dev/stdin: a command typed at it runs and comes back
const inC4ke = (cmd, what) => {
  const n = out.length;
  type(cmd);
  check(until(() => /from-c4ke/.test(text(n)) && /c4sh>/.test(text(n)), 20000), what, text(n));
};
inC4ke('echo from-c4ke', 'a command typed at C4KE\'s shell runs, and its prompt comes back');
n0 = out.length;
type('\\q');
check(until(() => prompt(n0), 20000), '\\q leaves C4KE, and C4IX\'s prompt comes back', text(n0));

// C4KE's kernel compiled inside C4IX: from /usr, as the repo builds it. c4sc
// once overran the 1 MB of stack cc gave it here (c4lc's g:resolvecode, fixed
// in 29cbac9), and cc said only that the source did not compile.
n0 = out.length;
type('cc -o /tmp/c4ke.c4r src/c4ke/c4ke.c');
check(until(() => prompt(n0), 60000) && !/did not compile|overran/.test(text(n0)),
  'cc compiles src/c4ke/c4ke.c inside C4IX', text(n0));
n0 = out.length;
type('c4m load-c4r.c -- /tmp/c4ke.c4r');
check(until(() => text(n0).includes('c4sh>'), 60000) && text(n0).includes('Kernel ready'),
  'c4m boots the C4KE compiled there to its shell', text(n0));
inC4ke('echo from-c4ke', 'and runs a command there');
n0 = out.length;
type('\\q');
check(until(() => prompt(n0), 20000), 'and \\q comes back again', text(n0));
// the kernel at the top of the repo too, where the host's make leaves it
n0 = out.length;
type('ls /usr');
check(until(() => prompt(n0), 2000) && /(^|\n)c4ke\.c4r\n/.test(text(n0)), '/usr/c4ke.c4r is there, beside c4m.c and load-c4r.c', text(n0));

// A kernel run as a program. C4KE's c4ke.c4r started from C4IX's shell
// called ITH, took the machine's trap handler from C4IX, and its own
// allocations then failed into its early trap handler: under c4m, which
// runs its guests on raw addresses, it wrote 0 and then text over the
// TLEV word at 0x10, and every task on the machine died ("illegal opcode
// 541148492", the bytes "LLA "). ITH traps in protected mode now, and
// C4IX kills the task -- here, and one level down: C4IX booted by C4KE
// under c4m. libjs drops a store to the ROM words besides (test-rom.mjs).
const refused = /tried to install the machine's trap handler/;
const romOk = () => vm.arena.i32[0x10 >> 2] === 64 && !(vm.romStores | 0);   // TLEV
n0 = out.length;
type('c4ke');
check(until(() => prompt(n0), 20000) && refused.test(text(n0)) && romOk(),
  'c4ke started from C4IX\'s shell is refused, and the machine runs on', text(n0));
n0 = out.length;
type('c4m load-c4r.c -- c4ke');
check(until(() => text(n0).includes('c4sh>'), 60000), 'c4m boots C4KE again', text(n0));
n0 = out.length;
type('c4ix');
check(until(() => prompt(n0), 60000), 'C4KE boots C4IX to its shell', text(n0));
n0 = out.length;
type('c4ke');
check(until(() => prompt(n0), 20000) && refused.test(text(n0)) && romOk(),
  'and c4ke there is refused too, with nothing written over the ROM', text(n0));
process.exit(fail);
