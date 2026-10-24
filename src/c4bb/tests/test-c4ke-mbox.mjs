// test-c4ke-mbox.mjs - the mailbox under C4KE on c4bb: boot the kernel with a
// mailbox fitted, start mbecho from the shell, feed frames from the host (with
// the interrupt), pin the echoes; then mbpair: two tasks, one message; then a
// second binder. Run from the repo root.
//
// --slow-clock: the PIT's clock is the cycle counter at a tenth of
// CYCLES_PER_MS -- a host ten times slower than the board assumes, the
// same on every run. There C4KE's handlers outlast the 10 ms tick, so ticks
// come due inside them; before machine.js held the PIT until the handler's
// TLEV, one landing between C4KE's re-arm and that TLEV wrecked the task it
// had just switched to, and the shell hung after mbecho (helios, at ~12000
// cycles/ms, did it by wall clock most runs).
//
// --libjs: the same on libjs (libjs/c4m.js), the page's interpreter, with
// the board's devices. Its window was narrower (only one of its 4096-cycle
// looks at the clock falling between the re-arm and the TLEV), too narrow
// for the slow clock to hit; bb_pit's third pass catches it instead.

import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { Arena, CONS_RET } from '../sim/arena.js';
import { Devices, CYCLES_PER_MS } from '../sim/devices.js';
import { Machine, R } from '../sim/machine.js';
import { Turbo } from '../sim/turbo.js';
import { assemble } from '../sim/ucode.js';
import { boot } from '../sim/loader.js';
import { HostMailbox } from '../sim/mbox.js';
import { createMachine, boot as bootLibjs } from '../../../libjs/boot.js';
import { STOP } from '../../../libjs/c4m.js';

const slow = process.argv.includes('--slow-clock');
const libjs = process.argv.includes('--libjs');
const name = 'test-c4ke-mbox' + (libjs ? ' (libjs)' : '') + (slow ? ' (slow clock)' : '');
let fails = 0;
const check = (ok, what) => { console.log(`${name}: ${ok ? 'OK  ' : 'FAIL'} ${what}`); if (!ok) fails++; };

const kernel = new Uint8Array(readFileSync('src/c4bb/images/c4ke32.c4r'));
const files = new Map();
const walk = (dir, prefix) => { for (const n of readdirSync(dir)) { const p = join(dir, n); if (statSync(p).isDirectory()) walk(p, prefix + n + '/'); else files.set(prefix + n, new Uint8Array(readFileSync(p))); } };
walk('src/c4bb/images/disk', '');

let out = '';
const bells = [];
let cycles = () => 0;                 // the machine does not exist yet
const onByte = (b) => { out += String.fromCharCode(b); };
const hostNow = slow ? { hostNow: () => Math.floor(cycles() / (CYCLES_PER_MS / 10)) } : {};

// The engine: the machine, its devices, and one slice of running it, which
// says whether the machine is still going.
let machine, dev, mbox, slice;
if (libjs) {
  machine = createMachine({ drives: [{ files, writable: false }], onByte, ...hostNow });
  dev = machine.dev;
  dev.onDoorbell = (v) => bells.push(v);
  ({ mbox } = bootLibjs(machine, kernel, ['c4ke32.c4r'], { mbox: 64 * 1024 }));
  slice = () => machine.run(20000) !== STOP.HALT;
} else {
  const uc = assemble(readFileSync('src/c4bb/hw/microcode.uc', 'utf8'));
  const fw = new Uint8Array(readFileSync('src/c4bb/fw/fw.c4r'));
  const arena = new Arena(32 * 1024 * 1024);
  dev = new Devices(arena, { onByte, files, onDoorbell: (v) => bells.push(v), ...hostNow });
  machine = new Machine(arena, uc, dev, { onLog: () => {} });
  ({ mbox } = boot(machine, fw, kernel, ['c4ke32.c4r'], { mbox: 64 * 1024 }));
  const turbo = new Turbo(machine);
  slice = () => { turbo.run(20000, CONS_RET); return machine.regs[R.PC] !== CONS_RET; };
}
cycles = () => machine.cycle;
const mb = new HostMailbox(machine.arena, mbox);

function runUntil(pred, budget) {
  const start = machine.cycle;
  while (machine.cycle - start < budget && !dev.halted) {
    if (!slice()) break;
    if (pred()) return true;
  }
  return pred();
}

// boot to the shell, start mbecho
check(runUntil(() => out.includes('c4sh>'), 30e6), 'C4KE booted to c4sh with the mailbox fitted');
dev.pushInput('mbecho.c4r\n');
check(runUntil(() => out.includes('mbecho: bound'), 20e6), 'mbecho bound to the host mailbox');

// frames in, with the interrupt; echoes out
mb.send(7, [1, 2, 3]); dev.raiseMbox();
mb.send(9, [10]); dev.raiseMbox();
check(runUntil(() => mb.pending() >= 2 * 3 + 3 + 1, 20e6), 'the task answered both frames');
let got = mb.drain();
check(got.length === 2 && got[0].type === 263 && Array.from(got[0].payload).join() === '2,3,4' && got[1].type === 265 && got[1].payload[0] === 11,
      `echoes: ${got.map((f) => `${f.type}[${Array.from(f.payload)}]`).join(' ')}`);
check(bells.includes(2), 'the idle task rang IDLE while the machine waited');

// a burst
for (let i = 0; i < 20; i++) { mb.send(100 + i, [i]); }
dev.raiseMbox();
check(runUntil(() => mb.pending() >= 20 * 4, 40e6), 'a burst of 20 frames answered');
got = mb.drain();
check(got.length === 20 && got.every((f, i) => f.type === 356 + i && f.payload[0] === i + 1), 'burst echoed in order');

// stop mbecho, then the pair
mb.send(0, []); dev.raiseMbox();
check(runUntil(() => out.includes('mbecho: done, 22 frames'), 20e6), 'mbecho exited on the type-0 frame having echoed 22');
// A second's idle at the prompt (more, on the slow clock): the shell polls
// stdin through sleeps, so every tick of it is a switch in and out of a
// handler. The hang was a shell that never read the next line.
runUntil(() => false, 20e6);
dev.pushInput('mbpair.c4r\n');
// the whole line: the check below reads it, and it may still be arriving
check(runUntil(() => /mbpair: got type 7 from pid [^\n]*\n/.test(out), 60e6), 'mbpair: the parent received the child\'s message');
const m = out.match(/mbpair: got type 7 from pid (\d+) \((\d+) words\): 10 20 30/);
check(!!m && m[2] === '3', `message contents: ${m ? m[0] : out.split('\n').filter((l) => l.startsWith('mbpair')).join(' | ')}`);

// A second binder, in another task slot. The host mailbox's owner is the
// first task to receive or await; it must be let go when that task exits.
// spin, in the background, takes the lowest free slot -- the one mbecho
// and then mbpair's parent had -- so the new mbecho gets another. Before
// the owner was cleared, it stayed the old slot's address: spin's now, so
// mbecho never got a frame.
runUntil(() => /c4sh> ?$/.test(out), 20e6);
dev.pushInput('spin.c4r &\n');
check(runUntil(() => out.includes('spin: running forever'), 20e6), 'spin started in the background');
runUntil(() => /c4sh> ?$/.test(out), 20e6);
dev.pushInput('mbecho.c4r\n');
check(runUntil(() => out.split('mbecho: bound').length > 2, 20e6), 'a second mbecho started');
mb.send(5, [41]); dev.raiseMbox();
check(runUntil(() => mb.pending() >= 3 + 1, 20e6), 'the second mbecho owns the host mailbox: it answered a frame');
got = mb.drain();
check(got.length === 1 && got[0].type === 261 && got[0].payload[0] === 42,
      `second echo: ${got.map((f) => `${f.type}[${Array.from(f.payload)}]`).join(' ')}`);
mb.send(0, []); dev.raiseMbox();
check(runUntil(() => out.includes('mbecho: done, 1 frames'), 20e6), 'the second mbecho exited');

dev.pushInput('\\q\n');
runUntil(() => dev.halted || out.includes('clean shutdown'), 20e6);
check(out.includes('clean shutdown') || dev.halted, 'kernel shut down cleanly');
if (fails) console.log(out.split('\n').slice(-25).join('\n'));
process.exit(fails ? 1 : 0);
