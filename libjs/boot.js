// boot.js - put an image in the machine and point the CPU at it.
//
// The .c4r parser and relocator are c4bb's (src/c4bb/sim/loader.js:
// parseC4r, loadImage), used as they are: the four patch types, BSS from
// the v3 header, 32-bit images only.
//
// What differs from c4bb is how main gets called. c4bb's firmware
// cannot call guest code from the host, so it fakes a return frame and
// watches for the PC to land on a sentinel. This machine does what c4mp
// does (src/c4mp/main.c:1-26) and assembles a few real instructions
// after the image:
//
//      IMM 0; PSH; JSR ctor; ADJ 1      each constructor, in order
//      IMM argc; PSH; IMM argv; PSH
//      JSR entry; ADJ 2                 main(argc, argv)
//      PSH                              main's result waits for EXIT
//      JSR dtor                         each destructor, in reverse
//      EXIT                             takes the result off the stack
//
// Constructors get one argument, 0, as they do on c4bb (c4l.c passes
// 0 too). The destructors cannot lose the exit code: each JSR/LEV pair
// leaves the stack where it found it.

import { parseC4r, loadImage } from '../src/c4bb/sim/loader.js';
import { Arena, MEM_BASE, TLEV_ADDR, OPNAMES_ROM } from '../src/c4bb/sim/arena.js';
import { Devices, OPNAMES, HEAP_BASE, HEAP_END, CYCLES_PER_MS } from '../src/c4bb/sim/devices.js';
import { OP, Machine } from './c4m.js';
import { Bus } from './bus.js';
import { Heap } from './heap.js';

// The whole machine, empty. Both hosts (cli.js, worker.js) build it
// here so they cannot drift apart.
//   arenaMb   memory in megabytes (default 32)
//   drives    [{ files: Map<name, Uint8Array>, writable, sink }] (c4bb's drives)
//   onByte    console sink, one byte at a time
//   mhz       how fast the machine claims to be; TIME is cycles / (mhz * 1000)
//   gui       a GuiDevice to fit, or null
export function createMachine(opts = {}) {
  const arena = new Arena((opts.arenaMb || 32) * 1024 * 1024);
  const dev = new Devices(arena, {
    drives: opts.drives,
    onByte: opts.onByte,
    cyclesPerMs: opts.mhz ? Math.max(1, Math.round(opts.mhz * 1000)) : CYCLES_PER_MS,
    hostNow: opts.hostNow,
  });
  const bus = new Bus(arena, opts.gui || null);
  const machine = new Machine(arena, dev, bus, new Heap(0, 0));
  if (opts.gui) opts.gui.attach(machine);
  return machine;
}

export function initRom(arena) {
  arena.i32[TLEV_ADDR >> 2] = OP.TLEV;
  arena.writeBytes(OPNAMES_ROM, new TextEncoder().encode(OPNAMES));
}

// The function symbols of an image, for stack traces: [{ off, name }]
// with off a code word offset, sorted. parseC4r stops before the symbol
// section ('S', src/c4cc/asm-c4r.c), so this walks to it from the
// header's lengths. Class 129 is a function (c4m.c's Fun).
export function parseSymbols(bytes) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const w = at => dv.getInt32(at, true);
  const codeLen = w(17), dataLen = w(21), patchLen = w(25), symbolsLen = w(29), consLen = w(33), desLen = w(37);
  let p = 41;
  p += 4 + codeLen * 4;
  p += 4 + dataLen;
  p += 4 + patchLen * 12;
  p += 4 + consLen * 4;
  p += 4 + desLen * 4;
  p += 4;                                        // 'S'
  const out = [];
  for (let i = 0; i < symbolsLen && p + 17 <= bytes.length; i++) {
    const cls = w(p + 8);
    const n = bytes[p + 16];
    const name = new TextDecoder('latin1').decode(bytes.subarray(p + 17, p + 17 + n));
    const val = w(p + 17 + n);
    p += 17 + n + 4;
    if (cls === 129) out.push({ off: val, name });
  }
  return out.sort((a, b) => a.off - b.off);
}

// opts.mbox: bytes of mailbox below the stack reserve (c4bb's MBOX_*);
// opts.reserve: host-owned bytes below the mailbox, outside the heap.
export function boot(machine, imageBytes, argv, opts = {}) {
  const arena = machine.arena, i32 = arena.i32;
  initRom(arena);
  const img = loadImage(arena, parseC4r(imageBytes), MEM_BASE);

  // argv strings and pointer table in the reserved area above the stack
  // (c4bb's layout, loader.js:141-154)
  let p = arena.stackTop;
  const enc = new TextEncoder();
  const ptrs = [];
  for (const s of argv) {
    const b = enc.encode(s);
    arena.writeBytes(p, b);
    arena.u8[p + b.length] = 0;
    ptrs.push(p);
    p += b.length + 1;
  }
  p = (p + 3) & ~3;
  const argvBase = p;
  for (const q of ptrs) { i32[p >> 2] = q; p += 4; }
  if (p > arena.size) throw new Error('libjs: argv does not fit in the 4 KB above the stack');

  // the trampoline
  const tramp = (img.top + 3) & ~3;
  let w = tramp >> 2;
  for (const c of img.cons) {
    i32[w++] = OP.IMM; i32[w++] = 0; i32[w++] = OP.PSH;
    i32[w++] = OP.JSR; i32[w++] = c; i32[w++] = OP.ADJ; i32[w++] = 1;
  }
  i32[w++] = OP.IMM; i32[w++] = argv.length; i32[w++] = OP.PSH;
  i32[w++] = OP.IMM; i32[w++] = argvBase; i32[w++] = OP.PSH;
  i32[w++] = OP.JSR; i32[w++] = img.entryAddr; i32[w++] = OP.ADJ; i32[w++] = 2;
  i32[w++] = OP.PSH;
  for (let k = img.des.length - 1; k >= 0; k--) { i32[w++] = OP.JSR; i32[w++] = img.des[k]; }
  i32[w++] = OP.EXIT;
  const top = w << 2;

  // The heap: everything between the image and a 1 MB stack reserve,
  // less whatever the host carves out for itself (loader.js:156-165).
  const mboxLen = (opts.mbox | 0) > 0 ? ((opts.mbox | 0) + 4095) & ~4095 : 0;
  const reserveLen = (opts.reserve | 0) > 0 ? ((opts.reserve | 0) + 4095) & ~4095 : 0;
  const heapBase = (top + 4096) & ~4095;
  const heapEnd = ((arena.stackTop - 1024 * 1024) & ~4095) - mboxLen - reserveLen;
  if (heapEnd <= heapBase) throw new Error('libjs: the image does not leave room for a heap; use a bigger arena');
  machine.heap.reset(heapBase, heapEnd);
  machine.dev.write32(HEAP_BASE, heapBase);
  machine.dev.write32(HEAP_END, heapEnd);
  const reserve = reserveLen ? { base: heapEnd, len: reserveLen } : null;
  const mbox = mboxLen ? { base: heapEnd + reserveLen, len: mboxLen } : null;
  if (mbox) { machine.dev.mbox = mbox; arena.u8.fill(0, mbox.base, mbox.base + mbox.len); }

  try {
    machine.symbols = parseSymbols(imageBytes).map(s => ({ addr: img.codeBase + s.off * 4, name: s.name }));
  } catch { machine.symbols = []; }
  machine.sp = machine.bp = arena.stackTop;
  machine.pc = tramp;
  machine.a = 0;
  return { img, tramp, argvBase, heapBase, heapEnd, mbox, reserve };
}
