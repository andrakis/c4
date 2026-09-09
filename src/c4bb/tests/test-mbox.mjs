// test-mbox.mjs - the mailbox, host side: lay the rings out, feed the
// probe image (tests/src/bb_mbox.c), pin the echoes, the doorbell, the
// wrap marker and the interrupt. Run from the repo root.

import { readFileSync } from 'node:fs';
import { Arena } from '../sim/arena.js';
import { Devices } from '../sim/devices.js';
import { Machine } from '../sim/machine.js';
import { Turbo } from '../sim/turbo.js';
import { assemble } from '../sim/ucode.js';
import { boot, runToExit } from '../sim/loader.js';
import { CONS_RET } from '../sim/arena.js';
import { R } from '../sim/machine.js';
import { HostMailbox } from '../sim/mbox.js';

let fails = 0;
const check = (ok, what) => { console.log(`${ok ? 'test-mbox: OK  ' : 'test-mbox: FAIL'} ${what}`); if (!ok) fails++; };

const uc = assemble(readFileSync('src/c4bb/hw/microcode.uc', 'utf8'));
const fw = new Uint8Array(readFileSync('src/c4bb/fw/fw.c4r'));
const prog = new Uint8Array(readFileSync('src/c4bb/images/bb_mbox.c4r'));

let out = '';
const bells = [];
const arena = new Arena(4 * 1024 * 1024);
const dev = new Devices(arena, { onByte: (b) => { out += String.fromCharCode(b); }, onDoorbell: (v) => bells.push(v) });
const machine = new Machine(arena, uc, dev, { onLog: () => {} });
const { progImg, mbox } = boot(machine, fw, prog, ['bb_mbox.c4r'], { mbox: 4096 });
const turbo = new Turbo(machine);
const mb = new HostMailbox(arena, mbox);

check(mbox && mbox.len === 4096, `region carved out: ${JSON.stringify(mbox)}`);
check(dev.heapEnd === mbox.base, 'HEAP_END stops at the region');
check(mb.toGuest.cap === 509 && mb.toHost.cap === 509, `ring caps ${mb.toGuest.cap}/${mb.toHost.cap}`);

// run until the guest says idle (or halts / spends the budget)
function runUntilIdle(budget = 2e6) {
  const start = bells.length;
  let spent = 0;
  while (spent < budget && !dev.halted) {
    turbo.run(2000, CONS_RET);
    spent += 2000;
    if (machine.regs[R.PC] === CONS_RET) break;
    if (bells.slice(start).includes(2)) break;
  }
  return spent;
}

// leg 1: two frames, echoed +1 with type|256, in order
mb.send(7, [1, 2, 3]);
mb.send(9, [10]);
runUntilIdle();
let got = mb.drain();
check(got.length === 2, `two echoes (${got.length})`);
check(got[0]?.type === 263 && Array.from(got[0].payload).join() === '2,3,4', `echo 1 ${got[0] && got[0].type} [${got[0] && got[0].payload}]`);
check(got[1]?.type === 265 && got[1].payload[0] === 11 && got[1].seq === 1, `echo 2 ${got[1] && got[1].type} seq ${got[1] && got[1].seq}`);
check(bells.includes(1) && bells.includes(2), `doorbell rang reply (1) and idle (2): ${[...new Set(bells)]}`);

// leg 2: 200 frames of 5 words through a 509-word ring -> the wrap marker
// is exercised many times on both rings; every echo must arrive in order
let sent = 0, received = 0, inOrder = true, lastSeq = got[1].seq;
for (let i = 0; i < 200; i++) {
  while (!mb.send(100 + i, [i, i * 2])) { runUntilIdle(); for (const f of mb.drain()) { if (f.seq !== lastSeq + 1) inOrder = false; lastSeq = f.seq; received++; } }
  sent++;
}
runUntilIdle();
for (const f of mb.drain()) { if (f.seq !== lastSeq + 1) inOrder = false; lastSeq = f.seq; received++; }
check(sent === 200 && received === 200, `wrap: sent ${sent}, received ${received}`);
check(inOrder, 'wrap: sequence numbers contiguous across the markers');

// leg 3: the interrupt reaches the guest's cycle handler
dev.raiseMbox();
turbo.run(20000, CONS_RET);
check(dev.mboxIrq === 0, 'irq cleared by delivery');

// leg 4: QUIT, run to exit, read the report
mb.send(0, []);
const status = runToExit(machine, progImg, 5e6, turbo);
check(status === 0, `exit status ${status}`);
check(out.includes('mbox: fitted, in cap 509, out cap 509'), 'guest saw the caps');
check(out.includes('echoed 202 frames, outbox full 0 times, irqs 1'), `guest report: ${out.trim().split('\n').pop()}`);

process.exit(fails ? 1 : 0);
