// try-heapwho.mjs -- innerbench -Tln N, recording which kernel call chain allocated
// each heap block; then the blocks left above ABOVE bytes, grouped by who, and the gaps.
//   node libjs/tests/try-heapwho.mjs    (MB, MMU=0, N, CMD, ABOVE, DESKTOP=1, DIAG=1, IMAGE)
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createMachine, boot } from '../boot.js';
import { typeBytes } from '../keys.js';
import { heapDump } from './heapdump.mjs';
import { GuiDevice } from '../gui-device.js';

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
const store = new Map();
const hogsrc = `#include "c4ix_user.h"
int main(int argc, char **argv) {
    int mb, n, i, k;
    int *p;
    char *s;
    mb = 0; s = argv[1];
    while (*s) { mb = mb * 10 + *s - '0'; ++s; }
    n = mb * 256;
    p = (int *)malloc(mb * 1048576);
    k = 0;
    while (k < 3) { i = 0; while (i < n) { p[i * 1024] = i; ++i; } ++k; }
    return 0;
}
`;
const out = [];
const gui = process.env.DESKTOP ? new GuiDevice({ w: 800, h: 600 }) : null;
const vm = createMachine({ arenaMb: +(process.env.MB || 32), mmu: process.env.MMU !== "0", cpus: 4, gui,
  drives: [{ files: (() => { const d = new LazyDisk(); d.set('hog.c', new TextEncoder().encode(hogsrc)); return d; })(), writable: false, sink: null },
           { files: new Map(), writable: true, sink: (n, b) => store.set(n, b), remove: n => store.delete(n) }],
  onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(process.env.IMAGE || join(ROOT, 'c4ix32.c4r'))), process.env.DESKTOP ? ['c4ix32.c4r', '--desktop'] : ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
const until = (c, b = 300000) => { for (let i = 0; i < b && !c(); i++) { vm.run(2e6); if (gui) gui.drain(); if (process.env.DIAG && i % 3000 === 2999) console.log("... sim", Math.round(vm.dev.simMs()), "out", out.length, JSON.stringify(text(Math.max(0, out.length - 120)))); } return c(); };
let seq = 0;
const sh = cmds => { const n0 = out.length, mark = `@@${++seq}@@`; typeBytes(vm, new TextEncoder().encode(`${cmds}\necho ${mark}\n`)); until(() => text(n0).includes(mark + '\n')); return text(n0); };

const syms = vm.symbols.slice().sort((a, b) => a.addr - b.addr);
const lo = syms[0].addr, hi = syms[syms.length - 1].addr + 4096;
const fn = pc => { if (pc < lo || pc >= hi) return '?'; let a = 0, b = syms.length - 1;
  while (a < b) { const m = (a + b + 1) >> 1; if (syms[m].addr <= pc) a = m; else b = m - 1; } return syms[a].name; };
const i32 = vm.arena.i32;
const who = new Map();
const h = vm.heap, alloc = h.alloc.bind(h);
h.alloc = n => {
  const a = alloc(n);
  if (a) {
    const m = vm.dev.machine || vm;
    // the kernel's call chain, through the frames' saved bp and return pc
    const chain = [fn(m.pc)];
    let bp = m.bp;
    for (let k = 0; k < 5 && bp > 4096 && bp < i32.length * 4; k++) { chain.push(fn(i32[(bp >> 2) + 1])); bp = i32[bp >> 2]; }
    who.set(a, chain.filter(x => x !== '?').slice(0, 4).join('<'));
  }
  return a;
};

if (process.env.DESKTOP) {
  until(() => /desktop: running on the display/.test(text()));
  until(() => /top: \d+ tasks/.test(text()));
  for (let i = 0; i < 200; i++) { vm.run(2e6); gui.drain(); }
  const n1 = out.length;
  vm.pendingSignal = 2;
  until(() => /c4ix:\/\$/.test(text(n1)), 20000);
  console.log('at the shell');
} else sh('wait 7');
const fr0 = sh('free');
console.log(fr0.split('\n').filter(l => /^memory/.test(l)).join(''), 'before');
const cmd = process.env.CMD || `cd /usr\ninnerbench -Tln ${process.env.N || 40}`;
console.log(sh(cmd).split('\n').filter(l => /killed|benchmark processes/.test(l)).join('\n'));
const fr = sh('free');
console.log(fr.split('\n').filter(l => /^memory|pages:|other:/.test(l)).join('\n'));
console.log(heapDump(vm));
const FROM = +(process.env.ABOVE || 5 * 1048576) + h.base;
const groups = new Map();
for (const [a, s] of h.spans) {
  if (a < FROM) continue;
  const w = who.get(a) || 'before the watch';
  const g = groups.get(w) || { n: 0, bytes: 0, at: [] };
  g.n++; g.bytes += s; g.at.push(Math.round((a - h.base) / 1024));
  groups.set(w, g);
}
const list = [...h.spans].sort((a, b) => a[0] - b[0]);
let prevEnd = h.base;
console.log('the heap, block by block, where a gap of 64 KB or more comes before it:');
for (const [a, s] of list) {
  const gap = a - prevEnd;
  if (gap >= 65536) console.log(`  gap ${Math.round(gap / 1024)} KB, then ${s} bytes at ${Math.round((a - h.base) / 1024)} K: ${who.get(a) || 'before the watch'}`);
  prevEnd = a + s;
}
console.log(`to the end: ${Math.round((h.end - prevEnd) / 1024)} KB; ${list.length} blocks in all`);
console.log(`live blocks above ${Math.round((FROM - h.base) / 1048576)} MB, by who allocated them:`);
for (const [w, g] of [...groups].sort((x, y) => y[1].bytes - x[1].bytes)) console.log(` ${g.bytes} bytes in ${g.n}: ${w}  at ${g.at.slice(0, 8).join(' ')}K`);
process.exit(0);
