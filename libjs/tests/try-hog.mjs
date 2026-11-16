// try-hog.mjs -- one program bigger than memory, walking it four times: machine time
// and swap, on IMAGE.
//   node libjs/tests/try-hog.mjs        (HOG=MB, IMAGE)
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createMachine, boot } from '../boot.js';
import { typeBytes } from '../keys.js';

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
const files = new LazyDisk();
files.set('hog.c', new TextEncoder().encode(`#include "c4ix_user.h"
int main(int argc, char **argv) {
    int mb, i, k, n, t0;
    int *p;
    char *s;
    mb = 0; s = argv[1];
    while (*s) { mb = mb * 10 + *s - '0'; ++s; }
    n = mb * 256;
    p = (int *)malloc(mb * 1048576);
    if (!p) { uprintf("hog: no memory\\n"); return 1; }
    t0 = ucycles();
    k = 0;
    while (k < 4) {
        i = 0;
        while (i < n) { p[i * 1024] = p[i * 1024] + i; ++i; }
        ++k;
    }
    uprintf("hog: %d MB walked 4 times\\n", mb);
    return 0;
}
`));
const out = [];
const vm = createMachine({ arenaMb: 32, mmu: true, cpus: 4, drives: [{ files, writable: false, sink: null },
    { files: new Map(), writable: true, sink: () => {}, remove: () => {} }], onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(process.env.IMAGE || join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
const until = (c, b = 300000) => { for (let i = 0; i < b && !c(); i++) vm.run(2e6); return c(); };
let seq = 0;
const sh = cmds => { const n0 = out.length, mark = `@@${++seq}@@`; typeBytes(vm, new TextEncoder().encode(`${cmds}\necho ${mark}\n`)); until(() => text(n0).includes(mark + '\n')); return text(n0); };
sh('wait 7');
const c = sh('cc -l c4ix -o /tmp/hog.c4r hog.c');
if (!/c4rlink: wrote/.test(c)) { console.log(c.slice(-500)); console.log(sh("free")); console.log(sh("dmesg -t").slice(-1200)); process.exit(1); }
const t0 = vm.dev.simMs();
const t = sh('/tmp/hog.c4r ' + (process.env.HOG || 48));
console.log(t.split('\n').filter(l => /hog|killed/.test(l)).join(' | '), '::', Math.round(vm.dev.simMs() - t0), 'ms');
const f = sh('free');
console.log(f.split('\n').filter(l => /^swap/.test(l)).join(''));
process.exit(0);
