// loader.js - .c4r image loader for the c4bb machine.
//
// A direct port of c4l.c (the cleanest loader in the repo) for 32-bit
// images and a flat arena: code and data are placed at a base address,
// and the patch table rebases every address unambiguously, which is
// what makes loading into an arena trivial.
//
// The load sequence mirrors what native c4m + load-c4r.c do:
//   fw.c4r first (its constructor fills the firmware vector latches),
//   then the program, then all constructors, then main via the boot
//   frame; destructors run only if main returns normally (exit()
//   halts the machine through the POWER device and skips them).

import { MEM_BASE, CONS_RET, OPNAMES_ROM, TLEV_ADDR } from './arena.js';
import { HEAP_BASE, HEAP_END, OPNAMES } from './devices.js';
import { R, OP } from './machine.js';
import { FETCH } from './ucode.js';

const PATCH_CODE = -1, PATCH_DATA = -2, PATCH_DCODE = -3, PATCH_DDATA = -4;

export function parseC4r(bytes) {
  if (bytes.length < 13 || bytes[0] !== 0x43 || bytes[1] !== 0x34 || bytes[2] !== 0x52)
    throw new Error('not a .c4r image');
  const version = bytes[3], wordBits = bytes[4];
  if (wordBits !== 32) throw new Error(`c4bb is a 32-bit machine; image is ${wordBits}-bit`);
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let p = 13;
  const w = () => { const v = dv.getInt32(p, true); p += 4; return v; };
  const entry = w(), codeLen = w(), dataLen = w(), patchLen = w(),
        symbolsLen = w(), consLen = w(), desLen = w();
  // Format v3 puts the data segment's in-memory size in the word at
  // byte 5, where v2 wrote filler (c4r.lisp, load-c4r.c:64). Anything
  // past DATALEN is BSS: uninitialized globals that occupy no image
  // bytes and must be zero at entry. Ignoring it is not merely a
  // missing feature -- the next thing placed in the arena (the heap,
  // or another image) lands ON TOP of those globals, so a c4lc-built
  // program's uninitialized globals get overwritten by its own
  // malloc'd memory. The arena is already zero-filled, so reserving
  // the space is all that is needed.
  const memsz = Math.max(dataLen, version >= 3 ? dv.getInt32(5, true) : dataLen);

  w();                                       // 'C' marker
  const code = new Int32Array(codeLen);
  for (let i = 0; i < codeLen; i++) code[i] = w();

  w();                                       // 'D' marker
  const data = bytes.slice(p, p + dataLen); p += dataLen;

  w();                                       // 'P' marker
  const patches = [];
  for (let i = 0; i < patchLen; i++) patches.push({ type: w(), addr: w(), val: w() });

  w();                                       // 'c' marker
  const cons = []; for (let i = 0; i < consLen; i++) cons.push(w());
  w();                                       // 'd' marker
  const des = []; for (let i = 0; i < desLen; i++) des.push(w());
  // 'S' symbols follow; not needed to run

  return { version, entry, code, data, memsz, patches, cons, des, symbolsLen };
}

// Place an image in the arena. codeBase word-aligned; data follows.
// Returns { entryAddr, codeBase, dataBase, top, cons, des } with all
// addresses in bytes.
export function loadImage(arena, img, base) {
  const codeBase = (base + 3) & ~3;
  const dataBase = (codeBase + img.code.length * 4 + 7) & ~3;
  for (let i = 0; i < img.code.length; i++)
    arena.i32[(codeBase >> 2) + i] = img.code[i];
  arena.writeBytes(dataBase, img.data);

  for (const pt of img.patches) {
    if (pt.type === PATCH_CODE)
      arena.write32(codeBase + pt.addr * 4, codeBase + pt.val * 4);
    else if (pt.type === PATCH_DATA)
      arena.write32(codeBase + pt.addr * 4, dataBase + pt.val);
    else if (pt.type === PATCH_DCODE)
      arena.write32(dataBase + pt.addr, codeBase + pt.val * 4);
    else if (pt.type === PATCH_DDATA)
      arena.write32(dataBase + pt.addr, dataBase + pt.val);
    // positive types: linker symbol references, skipped (c4l.c:164)
  }

  return {
    entryAddr: codeBase + img.entry * 4,
    codeBase, dataBase,
    top: (dataBase + Math.max(img.data.length, img.memsz | 0) + 7) & ~3,
    cons: img.cons.map(o => codeBase + o * 4),
    des: img.des.map(o => codeBase + o * 4),
  };
}

// Write the fixed low-memory ROM contents.
export function initRom(arena) {
  arena.write32(TLEV_ADDR, OP.TLEV);
  const enc = new TextEncoder().encode(OPNAMES);
  arena.writeBytes(OPNAMES_ROM, enc);
}

// Call a loaded function synchronously: fabricate the frame JSR would
// have made (args pushed left to right, return pc = CONS_RET) and run
// the machine until it comes back. This is the JS-side equivalent of
// c4l.c's invoke stub.
export function callFunction(machine, addr, args, maxCycles = 2e9) {
  const arena = machine.arena, regs = machine.regs;
  let sp = regs[R.SP];
  for (const a of args) { sp -= 4; arena.write32(sp, a | 0); }
  sp -= 4; arena.write32(sp, CONS_RET);
  regs[R.SP] = sp;
  regs[R.PC] = addr;
  machine.upc = FETCH;
  if (!machine.runUntilPc(CONS_RET, maxCycles))
    throw new Error('c4bb: function call did not return');
  regs[R.SP] = (regs[R.SP] + 4 * args.length) | 0;   // caller cleanup (ADJ)
  return regs[R.A];
}

// Full boot: firmware + program + argv + constructors. Afterwards the
// machine is ready to run; when PC reaches CONS_RET, main returned
// (run destructors, status in A). The POWER device handles exit().
// opts.mbox: bytes to carve out below the stack reserve for the mailbox
// (devices.js MBOX_*); the firmware sees a heap that stops short of it,
// so nothing the guest allocates can land on the rings. Zero = not fitted.
// opts.ctorBudget: cycles ONE constructor may take before boot gives up. The
// default is effectively no limit, which is right for an image the machine can
// read; an image it cannot -- a corrupt one, or one built for a different opcode
// ROM -- has no constructors, only whatever the garbage does, and that does not
// return. Lower it when running an image that may be garbage is the point.
export function boot(machine, fwBytes, progBytes, argv, opts = {}) {
  const arena = machine.arena;
  const mboxLen = (opts.mbox | 0) > 0 ? ((opts.mbox | 0) + 4095) & ~4095 : 0;
  initRom(arena);

  const fwImg = loadImage(arena, parseC4r(fwBytes), MEM_BASE);
  // No program image: the FIRMWARE is the program. That is the machine
  // switched on with nothing in ROM but its BIOS, which then goes
  // looking for a medium to boot -- docs/c4bb-storage.md M10.
  const progImg = progBytes ? loadImage(arena, parseC4r(progBytes), fwImg.top + 16)
                            : fwImg;

  // argv strings + pointer table in the reserved area above the stack
  let argPtr = arena.stackTop;
  const argvAddrs = [];
  const enc = new TextEncoder();
  for (const s of argv) {
    const bytes = enc.encode(s);
    arena.writeBytes(argPtr, bytes);
    arena.write8(argPtr + bytes.length, 0);
    argvAddrs.push(argPtr);
    argPtr += bytes.length + 1;
  }
  argPtr = (argPtr + 3) & ~3;
  const argvBase = argPtr;
  for (const a of argvAddrs) { arena.write32(argPtr, a); argPtr += 4; }

  // heap bounds for the firmware allocator (1 MB stack reserve)
  // (opts.reserve: host-owned bytes below the mailbox, outside the heap --
  // where a host parks something the guest must see but never allocate over)
  const reserveLen = (opts.reserve | 0) > 0 ? ((opts.reserve | 0) + 4095) & ~4095 : 0;
  const heapBase = (progImg.top + 4096) & ~4095;
  const heapEnd = ((arena.stackTop - 1024 * 1024) & ~4095) - mboxLen - reserveLen;
  machine.dev.write32(HEAP_BASE, heapBase);
  machine.dev.write32(HEAP_END, heapEnd);
  const reserve = reserveLen ? { base: heapEnd, len: reserveLen } : null;
  const mbox = mboxLen ? { base: heapEnd + reserveLen, len: mboxLen } : null;
  if (mbox) { machine.dev.mbox = mbox; arena.u8.fill(0, mbox.base, mbox.base + mbox.len); }

  // initial stack
  machine.regs[R.SP] = machine.regs[R.BP] = arena.stackTop;

  // constructors: firmware first (fills the vector latches), then the
  // program's, each called with a single argument (c4l.c:188 passes 0)
  const ctorBudget = opts.ctorBudget ?? 2e9;
  for (const c of fwImg.cons) callFunction(machine, c, [0], ctorBudget);
  if (progImg !== fwImg) for (const c of progImg.cons) callFunction(machine, c, [0], ctorBudget);

  // boot frame for main(argc, argv), pushed like c4m.c:1466 but with
  // the CONS_RET sentinel so destructors can run on normal return
  let sp = machine.regs[R.SP];
  sp -= 4; arena.write32(sp, argv.length);        // argc
  sp -= 4; arena.write32(sp, argvBase);           // argv
  sp -= 4; arena.write32(sp, CONS_RET);           // return pc sentinel
  machine.regs[R.SP] = sp;
  machine.regs[R.PC] = progImg.entryAddr;
  machine.upc = FETCH;

  return { fwImg, progImg, argvBase, heapBase, heapEnd, mbox, reserve };
}

// Run to completion after boot(); returns the exit status. When a
// Turbo engine is supplied the main body runs compiled; constructors,
// destructors and the sentinel handling are identical either way.
export function runToExit(machine, progImg, maxCycles = Infinity, turbo = null) {
  const limit = machine.cycle + maxCycles;
  while (!machine.dev.halted && machine.cycle < limit) {
    if (machine.upc === FETCH && machine.regs[R.PC] === CONS_RET) {
      // main returned normally: run destructors, then done
      const status = machine.regs[R.A];
      machine.regs[R.SP] = (machine.regs[R.SP] + 8) | 0;  // pop argc/argv
      for (const d of progImg.des) callFunction(machine, d, []);
      return status;
    }
    if (turbo) {
      turbo.run(limit - machine.cycle, CONS_RET);
      if (!machine.dev.halted && machine.regs[R.PC] !== CONS_RET) break;
    } else if (!machine.step()) break;
  }
  if (machine.dev.halted) return machine.dev.status;
  if (machine.regs[R.PC] === CONS_RET) {
    const status = machine.regs[R.A];
    machine.regs[R.SP] = (machine.regs[R.SP] + 8) | 0;
    for (const d of progImg.des) callFunction(machine, d, []);
    return status;
  }
  throw new Error('c4bb: cycle budget exhausted');
}
