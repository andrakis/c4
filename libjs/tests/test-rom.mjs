// test-rom.mjs -- the ROM words survive a wild guest. Every trap in every
// task returns through the TLEV word at 0x10, so one store there ends the
// whole machine: C4KE started as a C4IX program, under c4m, wrote 0 and
// then text over it, and every task hit "illegal opcode 541148492". libjs
// drops guest stores to 0x00-0x1F and the opcode-name ROM, and names the
// first one on the console. c4bb's firmware latches at 0x20-0x3F stay RAM.
//
// A jump into the null guard stops the machine rather than running the
// ROM: its words hold NULL_GUARD_OP, which only a fetch sees -- a guest
// reading 0..15 still gets 0.
//
// And ITH, which sets the machine's trap handler -- the kernel's -- traps
// in protected mode, as SIGH and USLP do.
import { createMachine, initRom } from '../boot.js';
import { OP, CPU_HALT_ADDR, NULL_GUARD_OP } from '../c4m.js';
import { OPNAMES } from '../../src/c4bb/sim/devices.js';

let fail = 0;
const check = (ok, what, detail) => {
  console.log(`test-rom: ${what} ${ok ? 'OK' : 'FAILED'}`);
  if (!ok) { fail = 1; if (detail !== undefined) console.log(detail); }
};

const CODE = 0x10000, DATA = 0x20000, HANDLER = 0x30000;
const machine = (files = new Map()) => {
  const out = [];
  const vm = createMachine({ arenaMb: 4, drives: [{ files, writable: false, sink: null }], onByte: b => out.push(b) });
  initRom(vm.arena);
  vm.sp = vm.bp = vm.arena.size - 4096;
  vm.pc = CODE;
  return { vm, text: () => Buffer.from(out).toString('latin1') };
};
const put = (vm, addr, words) => vm.arena.i32.set(words, addr >> 2);
const romIntact = vm => {
  const i32 = vm.arena.i32;
  const names = Buffer.from(vm.arena.u8.subarray(0x200, 0x200 + OPNAMES.length)).toString('latin1');
  return i32[0x10 >> 2] === OP.TLEV && i32[CPU_HALT_ADDR >> 2] === OP.CPUH && names === OPNAMES &&
         [0, 1, 2, 3].every(k => i32[k] === NULL_GUARD_OP);
};

// ---- stores of every kind into the ROM ----
{
  const { vm, text } = machine(new Map([['f', new TextEncoder().encode('LLA LLA LLA ')]]));
  const enc = new TextEncoder();
  vm.arena.u8.set(enc.encode('f\0'), DATA);
  vm.arena.u8.set(enc.encode('LLA text over low memory\0'), DATA + 16);
  const SI_AT = CODE + 5 * 4;
  put(vm, CODE, [
    OP.IMM, 0x10, OP.PSH, OP.IMM, 0x20414C4C, OP.SI,                // *(int *)0x10 = "LLA "
    OP.IMM, 0x18, OP.PSH, OP.IMM, 0x4C, OP.SC,                      // *(char *)0x18 (CPUH)
    OP.IMM, 0x200, OP.PSH, OP.IMM, 0x5A, OP.SC,                     // the opcode-name ROM
    OP.IMM, 0x1FE, OP.PSH, OP.IMM, -1, OP.SI,                       // a word straddling into it
    OP.IMM, 0x04, OP.PSH, OP.IMM, DATA + 16, OP.PSH, OP.IMM, 24, OP.PSH, OP.MCPY, OP.ADJ, 3,
    OP.IMM, 0x0C, OP.PSH, OP.IMM, 0x55, OP.PSH, OP.IMM, 12, OP.PSH, OP.MSET, OP.ADJ, 3,
    OP.IMM, DATA, OP.PSH, OP.IMM, 0, OP.PSH, OP.OPEN, OP.ADJ, 2,    // fd = open("f", 0)
    OP.PSH, OP.IMM, 0x08, OP.PSH, OP.IMM, 12, OP.PSH, OP.READ, OP.ADJ, 3,   // read(fd, 0x08, 12)
    OP.IMM, 0x20, OP.PSH, OP.IMM, 0x1234, OP.SI,                    // a firmware latch: RAM
    OP.IMM, 0x3FC, OP.PSH, OP.IMM, 0x5678, OP.SI,                   // past the ROM: RAM
    OP.IMM, 7, OP.PSH, OP.EXIT,
  ]);
  for (let i = 0; i < 100 && !vm.dev.halted; i++) vm.run(1e6);
  const t = text();
  check(vm.dev.halted && vm.dev.status === 7, 'a guest that stores over the ROM runs on to its EXIT', t);
  check(romIntact(vm), 'the TLEV and CPUH words, the null guard and the opcode names are untouched');
  check(vm.romStores === 4 + 24 + 12 + 12, 'every ROM store was dropped and counted', vm.romStores);
  // four single stores, then a byte each: 0x04-0x1B by MCPY, 0x0C-0x17
  // by MSET, 0x08-0x13 by READ
  check(vm.arena.i32[0x20 >> 2] === 0x1234 && vm.arena.i32[0x3FC >> 2] === 0x5678,
    'the firmware latch at 0x20 and the word after the ROM are still RAM');
  const lines = t.split('\n').filter(l => l.startsWith('libjs: store of'));
  check(lines.length === 1 && lines[0].includes('0x20414C4C') && lines[0].includes('ROM 0x00000010') &&
        lines[0].includes(`pc 0x${SI_AT.toString(16).toUpperCase().padStart(8, '0')}`),
    'the first ROM store is named once, with its value and pc', t);
}

// ---- ITH in protected mode traps; unprotected it installs ----
{
  const { vm, text } = machine();
  put(vm, HANDLER, [OP.ENT, 0, OP.IMM, 9, OP.PSH, OP.EXIT]);       // the kernel's handler: EXIT 9
  put(vm, CODE, [OP.IMM, HANDLER, OP.PSH, OP.ITH, OP.ADJ, 1,        // unprotected: installs
                 OP.IMM, 0, OP.PSH, OP.EXIT]);
  vm.run(1e5);
  check(vm.trapHandler === HANDLER && vm.dev.status === 0, 'ITH installs a handler in unprotected mode', text());
  vm.dev.halted = false;
  put(vm, CODE + 0x100, [OP.IMM, CODE + 0x200, OP.PSH, OP.ITH, OP.ADJ, 1,
                         OP.IMM, 5, OP.PSH, OP.EXIT]);
  vm.pc = CODE + 0x100; vm.mode = 1;
  vm.run(1e5);
  check(vm.dev.halted && vm.dev.status === 9 && vm.trapHandler === HANDLER,
    'ITH in protected mode traps to the kernel and leaves its handler in place',
    `status ${vm.dev.status}, handler 0x${vm.trapHandler.toString(16)}`);
}

// ---- the null guard: reads give 0, a jump there halts ----
{
  const { vm, text } = machine();
  put(vm, CODE + 0x100, [OP.IMM, 3, OP.PSH, OP.EXIT]);              // a nonzero read: EXIT 3
  const JSR_AT = CODE + 10 * 4;
  put(vm, CODE, [
    OP.IMM, 0, OP.LI, OP.PSH, OP.IMM, 1, OP.LC, OP.ADD,             // *(int *)0 + *(char *)1
    OP.BNZ, CODE + 0x100,
    OP.JSR, 0,                                                      // a call through a word left 0
    OP.IMM, 5, OP.PSH, OP.EXIT,
  ]);
  vm.run(1e5);
  const t = text();
  check(vm.dev.halted && vm.dev.status === -102, 'a call to 0 halts the machine (status -102), reads there gave 0',
    `status ${vm.dev.status}\n${t}`);
  check(t.includes('libjs: jump to 0x00000000, the null guard') &&
        t.includes(`top of stack 0x${(JSR_AT + 8).toString(16).toUpperCase().padStart(8, '0')}`),
    'and says where it went and where it came from', t);
  check(vm.cycle < 100, 'at once, not after running the ROM', vm.cycle);
  check(romIntact(vm), 'the guard still holds its marker');
}

process.exit(fail);
