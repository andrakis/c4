// test-tlev-wedge.mjs - the lockup that could not be interrupted.
//
// TLEV restores the return PC from bp+2 (hw/microcode.uc). If the word
// there is the address of the TLEV itself, the instruction reinstates
// its own address and runs again for ever -- and the machine refuses to
// interrupt at a TLEV, correctly, because it restores five registers
// and must not be cut in half. So nothing can break in: the cycle
// counter climbs and the machine never moves.
//
// That is the shape of the original c4bb report -- `ls` stopping
// mid-run, pc parked on a TLEV, cycles rising, no way back -- and the
// machine now says so instead of spinning.
//
// A unit test on a machine we build ourselves, because the alternative
// is to wait for a corrupted frame to happen, and a test that waits for
// a bug is not a test.
import { Arena } from '../sim/arena.js';
import { Devices } from '../sim/devices.js';
import { Machine, R, OP } from '../sim/machine.js';
import { assemble } from '../sim/ucode.js';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const uc = assemble(readFileSync(join(here, '..', 'hw', 'microcode.uc'), 'utf8'));

let fail = 0;
const check = (name, got, want) => {
  if (got === want) { console.log(`test-tlev-wedge: ${name} OK`); return; }
  console.log(`test-tlev-wedge: ${name} FAILED (got ${got}, want ${want})`);
  fail = 1;
};

function machine () {
  const arena = new Arena(1 << 20);
  const dev = new Devices(arena, { drives: [] });
  const m = new Machine(arena, uc, dev);
  dev.machine = m;
  return m;
}

// ---- the wedge: bp+2 holds the address of the TLEV itself -----------
{
  const m = machine();
  const tlev = 0x4000, bp = 0x8000;
  m.arena.write32(tlev, OP.TLEV);
  m.arena.write32(bp + 8, tlev);        // saved pc == this very TLEV
  m.regs[R.PC] = tlev;
  m.regs[R.BP] = bp;
  m.dev.pitMs = 10;                     // so boundaryChecks is reached
  check('a self-restoring TLEV is spotted', m.tlevWedged(tlev), true);
  m.boundaryChecks();
  check('and the machine halts rather than spinning', m.dev.halted, true);
  check('with a status of its own', m.dev.status, -101);
}

// ---- the healthy case must NOT trip ---------------------------------
// Every trap return in C4KE goes through the same TLEV, so "the PC is on
// a TLEV" is ordinary and must stay silent. Only the frame decides.
{
  const m = machine();
  const tlev = 0x4000, bp = 0x8000;
  m.arena.write32(tlev, OP.TLEV);
  m.arena.write32(bp + 8, 0x1234);      // saved pc points somewhere else
  m.regs[R.PC] = tlev;
  m.regs[R.BP] = bp;
  m.dev.pitMs = 10;
  check('an ordinary TLEV is left alone', m.tlevWedged(tlev), false);
  m.boundaryChecks();
  check('and the machine keeps running', m.dev.halted, false);
}

// ---- a frame pointing at some OTHER TLEV is fine --------------------
// Returning to a different TLEV is a real thing (nested traps); only
// returning to THIS one is the trap that cannot end.
{
  const m = machine();
  const tlev = 0x4000, other = 0x4100, bp = 0x8000;
  m.arena.write32(tlev, OP.TLEV);
  m.arena.write32(other, OP.TLEV);
  m.arena.write32(bp + 8, other);
  m.regs[R.PC] = tlev;
  m.regs[R.BP] = bp;
  check('returning to a DIFFERENT TLEV is fine', m.tlevWedged(tlev), false);
}

// ---- not a TLEV at all ----------------------------------------------
{
  const m = machine();
  const pc = 0x4000, bp = 0x8000;
  m.arena.write32(pc, OP.LEV);
  m.arena.write32(bp + 8, pc);          // self-referencing, but not a TLEV
  m.regs[R.PC] = pc;
  m.regs[R.BP] = bp;
  check('a self-referencing non-TLEV is not our business', m.tlevWedged(pc), false);
}

console.log(fail ? 'test-tlev-wedge: FAILED' : 'test-tlev-wedge: OK');
process.exit(fail);
