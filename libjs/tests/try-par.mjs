// try-par.mjs -- innerbench -Tln N on real threads, as the browser's parallel mode runs
// it, for each N in NS (space-separated) one after another on the same machine;
// free before, and after each run.
//   NS='30 40 50' node libjs/tests/try-par.mjs      (or N for one run)
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { Worker } from 'node:worker_threads';
import { createMachine, boot } from '../boot.js';
import { typeBytes } from '../keys.js';
import { STOP } from '../c4m.js';

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
const out = [];
const vm = createMachine({ arenaMb: 32, mhz: 400, mmu: true, cpus: 4, parallel: true, spawn: url => new Worker(url),
  drives: [{ files: new LazyDisk(), writable: false, sink: null },
    { files: new Map(), writable: true, sink: () => {}, remove: () => {} }], onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
const par = vm.smp && vm.smp.parallel ? vm.smp : null;
const until = (cond, budget) => {
  for (let i = 0; i < budget && !cond() && !vm.dev.halted; i++) {
    const r = vm.run(2e6);
    if (!par || vm.dev.halted) continue;
    par.service();
    if (r === STOP.SLEEP && par.othersRunning()) par.sleepSync(vm.dev.napMs);
    else if (r === STOP.PARK || r === STOP.CPUHALT || vm.cpuState !== 'run') par.parkSync(10);
  }
  return cond();
};
let seq = 0;
const sh = cmds => { const n0 = out.length, mark = `@@${++seq}@@`; typeBytes(vm, new TextEncoder().encode(`${cmds}\necho ${mark}\n`)); until(() => text(n0).includes(mark + '\n'), 5000000); return text(n0); };
until(() => text().includes('c4ix:/$'), 30000);
const unpack = (/unpacking c4ix-root\.tgz in the background as task (\d+)/.exec(text()) || [])[1] || 7;
sh(`wait ${unpack}`);
const show = t => t.split('\n').filter(l => /^memory|^swap|pages:|other:|killed|benchmark processes/.test(l)).join('\n');
console.log('before\n' + show(sh('free')));
const NS = (process.env.NS || process.env.N || '40').trim().split(/\s+/);
for (const N of NS) {
  const t0 = Date.now();
  const r = sh(`cd /usr\ninnerbench -Tln ${N}`);
  console.log(`-Tln ${N}: ${Math.round((Date.now() - t0) / 1000)} s real\n` + show(r));
  console.log('after\n' + show(sh('free')));
  console.log('complete:', (r.match(/innerbench complete/g) || []).length, 'killed:', (r.match(/killed/g) || []).length);
}
process.exit(0);
