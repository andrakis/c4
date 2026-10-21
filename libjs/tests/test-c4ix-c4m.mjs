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
n0 = out.length;
type('\\q');
check(until(() => prompt(n0), 20000), '\\q leaves C4KE, and C4IX\'s prompt comes back', text(n0));
process.exit(fail);
