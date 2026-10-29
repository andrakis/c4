// test-c4ix-guard.mjs -- the boot stack's guard (STACK_GUARD, c4m.js
// guardHit). C4IX's kernel task leaves the boot stack for a stack in the
// kernel arena (sched.c, sched_offboot) and has libjs guard it, so a
// program that writes past its heap block, up to the top of memory, halts
// the machine there, naming the pc and the address -- where before it
// wrote over the kernel task's frames and the machine crashed at some
// later switch.
//
// A program compiled inside C4IX takes a block and runs off its end: with
// one memset from the block into the boot stack (MSET), word by word into
// it from just below (SI), and reading (LI). On one CPU, and on two
// threads with the program on CPU 1, whose hit CPU 0 tells. And with
// nothing run off the end, C4IX boots, says its layout, and shuts down
// with the guard lifted again.
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { Worker } from 'node:worker_threads';
import { createMachine, boot } from '../boot.js';
import { STOP } from '../c4m.js';
import { typeBytes } from '../keys.js';

const ROOT = new URL('../../src/c4bb/images/', import.meta.url).pathname;
const index = JSON.parse(readFileSync(join(ROOT, 'disk/index.json'), 'utf8'));
class LazyDisk extends Map {
  constructor(idx) { super(); this.sizes = new Map(idx); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) { if (super.has(n)) return super.get(n); if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(ROOT, 'disk', n))); super.set(n, b); return b; }
}
// the program, as a file only the host disk has (sys_open falls back to it)
const SRC = `#include "c4ix_user.h"
int main(int argc, char **argv) {
    char *p;
    int *q;
    int top, how, x;
    p = (char *)malloc(64);
    top = *(int *)0x1e4;          // libjs's STACK_BASE: the boot stack
    how = argc > 1 ? argv[1][0] : 'w';
    uprintf("overrun: a block at 0x%x, the boot stack at 0x%x\\n", (int)p, top);
    if (how == 'm') memset(p, 85, top - (int)p + 64);
    else {
        q = (int *)(top - 64);
        x = 0;
        while (1) {
            if (how == 'r') x = x + *q; else *q = 0x55555555;
            q = q + 1;
        }
    }
    uprintf("overrun: still running\\n");
    return 0;
}
`;

let fail = 0;
const check = (ok, what, detail) => {
  console.log(`test-c4ix-guard: ${what} ${ok ? 'OK' : 'FAILED'}`);
  if (!ok) { fail = 1; if (detail) console.log(detail.slice(-1500)); }
};

// A machine, booted; run(cond, budget) drives it until cond() or a halt,
// on threads as cli.js does: CPU 0 answers the others between runs and
// waits for them when it is parked.
function start(cpus) {
  const files = new LazyDisk(index);
  files.set('overrun.c', new TextEncoder().encode(SRC));
  const out = [];
  const vm = createMachine({
    arenaMb: 64, mhz: 100, drives: [{ files, writable: false, sink: null }], onByte: b => out.push(b),
    cpus, parallel: cpus > 1, spawn: url => new Worker(url),
    // the guard is what stops a wild program where there is no MMU; with
    // one (the default) its page fault would, first
    mmu: false,
  });
  boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
  const par = vm.smp && vm.smp.parallel ? vm.smp : null;
  const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
  const run = (cond, budget) => {
    for (let i = 0; i < budget && !cond() && !vm.dev.halted; i++) {
      const r = vm.run(2e6);
      if (!par || vm.dev.halted) continue;
      par.service();
      if (r === STOP.SLEEP && par.othersRunning()) par.sleepSync(vm.dev.napMs);
      else if (r === STOP.PARK || r === STOP.CPUHALT || vm.cpuState !== 'run') par.parkSync(10);
    }
    return cond();
  };
  const type = s => typeBytes(vm, new TextEncoder().encode(s + '\n'));
  const prompt = from => /(^|\n)c4ix:[^\n]*\$/.test(text(from));
  const cmd = (s, budget) => { const n0 = out.length; type(s); run(() => prompt(n0), budget); return text(n0); };
  return { vm, par, out, text, run, cmd, end: () => { if (par) par.shutdown(); } };
}

const hex = v => '0x' + (v >>> 0).toString(16).toUpperCase().padStart(8, '0');

// Nothing run off the end: the layout, and the way back at shutdown
{
  const m = start(1);
  m.run(() => m.text().includes('c4ix:/$'), 3000);
  const boot = m.text();
  check(/Kernel arena - tasks, files, kernel stacks/.test(boot) && /Boot stack, guarded \(1024 KB\)/.test(boot) &&
        m.vm.dev.stackGuard === 1,
        'C4IX boots off its boot stack, says the layout, and has the boot stack guarded', boot);
  const b = /\[mem (0x[0-9a-f]{8})-0x[0-9a-f]{8}\] Boot stack, guarded/.exec(boot);
  check(b && parseInt(b[1], 16) === m.vm.dev.stackBase, 'the layout names the guard where libjs keeps it', boot);
  const n0 = m.out.length;
  m.cmd('exit', 20000);
  m.run(() => m.vm.dev.halted, 20000);
  const tail = m.text(n0);
  check(m.vm.dev.halted && m.vm.dev.status === 0 && tail.includes('c4ix: shutdown complete, 0 tasks remain') &&
        m.vm.dev.stackGuard === 0 && !/libjs:|panic/.test(tail),
        'shutdown comes back to the boot stack, the guard lifted, and completes', tail);
  m.end();
}

// Each way off the end, on a fresh machine: compiled, run, and halted by the guard
for (const [cpus, how, what] of [
  [1, 'm', 'a memset from a block into the boot stack halts at its first guarded byte, before it writes'],
  [1, 'w', 'stores run off the end of the heap halt at the boot stack'],
  [1, 'r', 'loads run off the end of the heap halt at the boot stack'],
  [2, 'w', 'on two threads, stores from CPU 1 halt the machine, and CPU 0 says where'],
]) {
  const m = start(cpus);
  m.run(() => m.text().includes('c4ix:/$'), 3000);
  for (const c of ['wait ' + (4 + cpus - 1), 'cc -l c4ix -o /tmp/overrun.c4r overrun.c']) m.cmd(c, 20000);
  const n0 = m.out.length;
  m.cmd((cpus > 1 ? 'pin 1 ' : '') + '/tmp/overrun.c4r ' + how, 20000);
  const t = m.text(n0);
  const base = m.vm.dev.stackBase;
  const want = how === 'm' ? `MSET of ` : how === 'w' ? `write of 0x55555555 to ${hex(base)}` : `read of a word at ${hex(base)}`;
  check(m.vm.dev.halted && m.vm.dev.status === -103 && t.includes('overrun: a block at') && t.includes(want) &&
        (how !== 'm' || t.includes(`writes ${hex(base)}`)) && /at pc 0x[0-9A-F]{8}/.test(t) &&
        t.includes('that is the boot stack, guarded') && (cpus === 1 || t.includes('on cpu 1')) &&
        !t.includes('still running') && !/panic|illegal/.test(t),
        what, t);
  m.end();
}

process.exit(fail);
