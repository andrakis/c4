// repro-save.mjs -- persist save while other tasks read big files from drive 1,
// on 4 CPUs, ROUNDS times; a save that says a write failed counts as a fail.
//   node libjs/tests/repro-save.mjs     (MB, CPUS, Q quantum, PARALLEL=1, ROUNDS)
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { gunzipSync } from 'node:zlib';
import { createMachine, boot } from '../boot.js';
import { STOP } from '../c4m.js';
import { typeBytes } from '../keys.js';
import { Worker } from 'node:worker_threads';

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
const disk = new LazyDisk();
const store = new Map();
const out = [];
const drives = [{ files: disk, writable: false, sink: null },
  { files: new Map(), writable: true, sink: (n, b) => store.set(n, b), remove: n => store.delete(n) }];
const vm = createMachine({ arenaMb: +(process.env.MB || 64), cpus: +(process.env.CPUS || 4), quantum: +(process.env.Q || 200), parallel: !!process.env.PARALLEL, spawn: url => new Worker(url), drives, onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
const par = vm.smp && vm.smp.parallel ? vm.smp : null;
const run = () => {
  const r = vm.run(2e6);
  if (par) {
    par.service();
    if (r === STOP.SLEEP && par.othersRunning()) par.sleepSync(vm.dev.napMs);
    else if (r === STOP.PARK || r === STOP.CPUHALT || vm.cpuState !== 'run') par.parkSync(10);
  }
};
const until = (c, b = 4000) => { for (let i = 0; i < b && !c(); i++) run(); return c(); };
let seq = 0;
const sh = (cmds, budget = 8000) => {
  const n0 = out.length, mark = `@@${++seq}@@`;
  typeBytes(vm, new TextEncoder().encode(`${cmds}\necho ${mark}\n`));
  until(() => text(n0).includes(mark + '\n'), budget);
  return text(n0);
};
sh('wait 7');
// big files, so a save is many writes
const tar = gunzipSync(readFileSync(join(DISK, 'c4ix-root.tgz')));
const bigs = [];
for (let pos = 0; pos + 512 <= tar.length && tar[pos]; ) {
  const h = tar.subarray(pos, pos + 512), str = (a, n) => h.subarray(a, a + n).toString('latin1').replace(/\0.*$/s, '');
  const name = (h[345] ? str(345, 155) + '/' : '') + str(0, 100), size = parseInt(str(124, 12).trim() || '0', 8);
  if (h[156] === 48 && size > 200000) bigs.push('/' + name.replace(/^\.\//, ''));
  pos += 512 + Math.ceil(size / 512) * 512;
}
console.log('bigs', bigs.length);
console.log(sh(bigs.slice(0, 6).map((b, i) => `cat ${b} > /usr/w${i}.bin`).join('\n') + '\ncrc32 /usr/w0.bin /usr/w1.bin ' + bigs[0] + '\ncat ' + bigs[0] + ' | wc').slice(-800));
let fails = 0;
for (let round = 0; round < +(process.env.ROUNDS || 6); round++) {
  const readers = bigs.slice(6, 14).map(b => `crc32 ${b} &`).join('\n');
  const t = sh(`${readers}\necho r${round} >> /usr/w0.bin\npersist save\nwait`);
  const bad = /writing [^\n]*failed|cannot write/.test(t);
  if (bad) fails++;
  console.log(`round ${round}: ${bad ? 'FAILED' : 'ok'} ${(t.match(/persist: [^\n]*/g) || []).join(' | ')}`);
}
console.log(`fails ${fails}`);
process.exit(0);
