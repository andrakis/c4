#!/usr/bin/env node
// lockstep.js - prove the step and turbo engines are the same machine.
//
//   node src/c4bb/tools/lockstep.js image.c4r [maxInstructions]
//
// Boots the same image on two machines, then advances one instruction
// at a time on each - the microstep interpreter on one, the compiled
// turbo engine on the other - comparing every register (including the
// microarchitectural MAR/MDR/B/T/U) and the cycle counter at each
// boundary. Any divergence prints the offending instruction and
// fails. Output bytes are compared at the end.

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { Arena } from '../sim/arena.js';
import { Devices } from '../sim/devices.js';
import { Machine, R, REG_NAMES } from '../sim/machine.js';
import { assemble, FETCH } from '../sim/ucode.js';
import { boot } from '../sim/loader.js';
import { Turbo } from '../sim/turbo.js';
import { CONS_RET } from '../sim/arena.js';

const here = dirname(fileURLToPath(import.meta.url));

function mkMachine(ucSource, fwBytes, progBytes, args) {
  const arena = new Arena(32 * 1024 * 1024);
  const out = [];
  const dev = new Devices(arena, { onByte: b => out.push(b) });
  const machine = new Machine(arena, assemble(ucSource), dev, { onLog: () => {} });
  const { progImg } = boot(machine, fwBytes, progBytes, args);
  return { machine, out, progImg };
}

function stepInstruction(m) {
  if (!m.step()) return false;              // starts (cycle++) and runs 1 ustep
  while (m.upc !== FETCH && !m.dev.halted) if (!m.step()) return false;
  return true;
}

const [img, maxArg] = process.argv.slice(2);
if (!img) { console.error('usage: lockstep.js image.c4r [maxInstructions]'); process.exit(1); }
const maxInst = maxArg ? parseInt(maxArg, 10) : 500000;

const ucSource = readFileSync(join(here, '..', 'hw', 'microcode.uc'), 'utf8');
const fwBytes = new Uint8Array(readFileSync(join(here, '..', 'fw', 'fw.c4r')));
const progBytes = new Uint8Array(readFileSync(img));
const args = [img];

const A = mkMachine(ucSource, fwBytes, progBytes, args);
const B = mkMachine(ucSource, fwBytes, progBytes, args);
const turbo = new Turbo(B.machine);

let n = 0;
for (; n < maxInst; n++) {
  const doneA = A.machine.dev.halted || A.machine.regs[R.PC] === CONS_RET;
  const doneB = B.machine.dev.halted || B.machine.regs[R.PC] === CONS_RET;
  if (doneA !== doneB) { console.error(`lockstep: termination diverged at inst ${n}`); process.exit(1); }
  if (doneA) break;
  stepInstruction(A.machine);
  turbo.run(1);
  if (A.machine.cycle !== B.machine.cycle) {
    console.error(`lockstep: cycle diverged at inst ${n}: ${A.machine.cycle} vs ${B.machine.cycle}`);
    process.exit(1);
  }
  for (let ri = 0; ri < 11; ri++) {
    if (A.machine.regs[ri] !== B.machine.regs[ri]) {
      console.error(`lockstep: ${REG_NAMES[ri]} diverged at inst ${n} (cycle ${A.machine.cycle}): ` +
        `step=0x${(A.machine.regs[ri] >>> 0).toString(16)} turbo=0x${(B.machine.regs[ri] >>> 0).toString(16)}`);
      process.exit(1);
    }
  }
}

const outA = Buffer.from(A.out).toString('latin1');
const outB = Buffer.from(B.out).toString('latin1');
if (outA !== outB) { console.error('lockstep: output diverged'); process.exit(1); }
console.log(`lockstep: OK (${n} instructions, output ${A.out.length} bytes identical)`);
