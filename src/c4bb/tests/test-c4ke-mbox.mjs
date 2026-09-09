// test-c4ke-mbox.mjs - the mailbox under C4KE on c4bb: boot the kernel with a
// mailbox fitted, start mbecho from the shell, feed frames from the host (with
// the interrupt), pin the echoes; then mbpair: two tasks, one message.
// Run from the repo root.

import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { Arena, CONS_RET } from '../sim/arena.js';
import { Devices } from '../sim/devices.js';
import { Machine, R } from '../sim/machine.js';
import { Turbo } from '../sim/turbo.js';
import { assemble } from '../sim/ucode.js';
import { boot } from '../sim/loader.js';
import { HostMailbox } from '../sim/mbox.js';

let fails = 0;
const check = (ok, what) => { console.log(`${ok ? 'test-c4ke-mbox: OK  ' : 'test-c4ke-mbox: FAIL'} ${what}`); if (!ok) fails++; };

const uc = assemble(readFileSync('src/c4bb/hw/microcode.uc', 'utf8'));
const fw = new Uint8Array(readFileSync('src/c4bb/fw/fw.c4r'));
const kernel = new Uint8Array(readFileSync('src/c4bb/images/c4ke32.c4r'));
const files = new Map();
const walk = (dir, prefix) => { for (const n of readdirSync(dir)) { const p = join(dir, n); if (statSync(p).isDirectory()) walk(p, prefix + n + '/'); else files.set(prefix + n, new Uint8Array(readFileSync(p))); } };
walk('src/c4bb/images/disk', '');

let out = '';
const bells = [];
const arena = new Arena(32 * 1024 * 1024);
const dev = new Devices(arena, { onByte: (b) => { out += String.fromCharCode(b); }, files, onDoorbell: (v) => bells.push(v) });
const machine = new Machine(arena, uc, dev, { onLog: () => {} });
const { mbox } = boot(machine, fw, kernel, ['c4ke32.c4r'], { mbox: 64 * 1024 });
const turbo = new Turbo(machine);
const mb = new HostMailbox(arena, mbox);

function runUntil(pred, budget) {
  const start = machine.cycle;
  while (machine.cycle - start < budget && !dev.halted) {
    turbo.run(20000, CONS_RET);
    if (machine.regs[R.PC] === CONS_RET) break;
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
dev.pushInput('mbpair.c4r\n');
check(runUntil(() => out.includes('mbpair: got type 7 from pid'), 60e6), 'mbpair: the parent received the child\'s message');
const m = out.match(/mbpair: got type 7 from pid (\d+) \((\d+) words\): 10 20 30/);
check(!!m && m[2] === '3', `message contents: ${m ? m[0] : out.split('\n').filter((l) => l.startsWith('mbpair')).join(' | ')}`);

dev.pushInput('\\q\n');
runUntil(() => dev.halted || out.includes('clean shutdown'), 20e6);
check(out.includes('clean shutdown') || dev.halted, 'kernel shut down cleanly');
if (fails) console.log(out.split('\n').slice(-25).join('\n'));
process.exit(fails ? 1 : 0);
