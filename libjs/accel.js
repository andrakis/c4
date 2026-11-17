// accel.js -- accelerators: a game's heaviest loops run by the host, in the
// game's own JavaScript, one instruction however much they do (docs/accel.md).
//
// A game that wants them ships games/NAME/c4/accel.mjs, whose default export
// is given this file's kit and returns its operations, an array of functions
// of the machine. libjs knows no game: build-images.sh lists the modules it
// finds in src/c4bb/images/accel.json, and the machine loads those before it
// starts (loadAccel), from the site's own games/ folder and nowhere else, in
// the list's order, the same on every CPU (ap.js is handed the names). A
// program can name a module, never supply one.
//
// The numbers 0x1000 to 0x1fff (ACC_BASE, ACC_END in c4m.js) are these,
// outside the instruction set, C4KE's services (128 up) and C4IX's syscalls
// (200 up), so no compiler and no other machine has to know them. INFO bit
// 0x100000 (C4I_ACCEL) says the machine has the loader. A program asks once
// for its game's (libjs/guest/accel.h):
//   base = __c4_opcode("quake", ACC_FIND);    // 0x1000: 0 if there is none
// and the k-th module's op i is then base + i, base being ACC_BASE + 0x100 *
// (k + 1); a number nobody has is a custom opcode as before. Everywhere else
// -- native c4m, c4bb, C4DOS, libjs with accel off -- the bit is clear and
// the program keeps its own loops.
//
// An operation reads its arguments with kit.args (sp+4 upward, the last one
// nearest), guest memory only through kit's readers and writers, and returns
// what the program gets back (a). Under the MMU those go through the task's
// pages, and an address it may not use faults as the program's own load or
// store would: the instruction then runs again from the start once the
// kernel has seen to it, so an operation must give the same result run
// again after a partial run (what it writes it may read back only where
// writing the same again is what the loop would do), or first make sure
// with kit.probe that nothing it writes can fault. It costs one cycle.
//
// An operating system ships its own the same way, src/NAME/accel.mjs, listed
// as "os/NAME" (C4IX's: src/c4ix/accel.mjs, its libarc's CRC-32 and
// inflater). Those have a switch of their own, the machine's option osAccel
// (cli.js --no-osaccel, the page's &osaccel=0), which the guest may read and
// turn (ACC_OS, 0x1001: the desktop's Settings): while it is off, ACC_FIND
// gives 0 for an OS module and its programs run their own loops. One word,
// shared with the CPUs on threads (ap.js adopts it), so every CPU sees a turn.
import { STOP, ACC_BASE, ACC_END } from './c4m.js';
import { FAULT, FK } from './mmu.js';

export const ACC_FIND = ACC_BASE, ACC_OS = ACC_BASE + 1, ACC_SLOT = 0x100;
const MAX_MODS = (ACC_END - ACC_BASE) / ACC_SLOT - 1;
const NAME = /^[a-z0-9_-]{1,32}$/;
const mods = [], index = new Map();

// The OS modules' switch: 1 on, 0 off
let osSwitch = new Int32Array(typeof SharedArrayBuffer !== 'undefined' ? new SharedArrayBuffer(4) : new ArrayBuffer(4));
osSwitch[0] = 1;
export const accelSwitch = () => osSwitch.buffer;
export const adoptAccelSwitch = buf => { if (buf) osSwitch = new Int32Array(buf); };
export const osAccelOn = () => Atomics.load(osSwitch, 0) !== 0;
export const setOsAccel = on => { Atomics.store(osSwitch, 0, on ? 1 : 0); };

// A byte reader, a byte writer, a word reader and a word writer for one
// instruction: plain memory as runPlain's own loads take it, a task's pages
// under the MMU, or the machine's ld and st for anything else (the user
// window, the boot stack's guard, the low page, off the arena's end). Each
// keeps its own page, so two streams read in turn do not evict each other.
function reader(m) {
  const u8 = m.u8;
  if (m.vm) {
    const mmu = m.mmu;
    let vp = -1, base = 0;
    return x => {
      const v = x >>> 12;
      if (v !== vp) {
        if (v === 0) return m.ld8(x) & 255;
        const p = mmu.page(x, FK.READ);
        if (p < 0) throw FAULT;
        vp = v; base = p;
      }
      return u8[base | (x & 4095)];
    };
  }
  if (!m.uwin) {
    const hi = Math.min(m.guardLo(), u8.length);
    return x => (x >= 0x1000 && x < hi ? u8[x] : m.ld8(x) & 255);
  }
  return x => m.ld8(x) & 255;
}
function writer(m) {
  const u8 = m.u8;
  if (m.vm) {
    const mmu = m.mmu;
    let vp = -1, base = 0;
    return (x, b) => {
      const v = x >>> 12;
      if (v !== vp) {
        if (v === 0) { m.st8(x, b); return; }
        const p = mmu.page(x, FK.WRITE);
        if (p < 0) throw FAULT;
        vp = v; base = p;
      }
      u8[base | (x & 4095)] = b;
    };
  }
  if (!m.uwin) {
    const hi = Math.min(m.guardLo(), u8.length);
    return (x, b) => { if (x >= 0x1000 && x < hi) u8[x] = b; else m.st8(x, b); };
  }
  return (x, b) => m.st8(x, b);
}
function reader32(m) {
  const i32 = m.i32;
  if (!m.vm && !m.uwin) {
    const hi = Math.min(m.guardLo(), m.u8.length) - 3;
    return x => ((x & 3) === 0 && x >= 0x1000 && x < hi ? i32[x >> 2] | 0 : m.ld32(x) | 0);
  }
  return x => m.ld32(x) | 0;
}
function writer32(m) {
  const i32 = m.i32;
  if (!m.vm && !m.uwin) {
    const hi = Math.min(m.guardLo(), m.u8.length) - 3;
    return (x, v) => { if ((x & 3) === 0 && x >= 0x1000 && x < hi) i32[x >> 2] = v; else m.st32(x, v); };
  }
  return (x, v) => m.st32(x, v);
}
// That the task may use [addr, addr + len): before an operation writes
// what it cannot write twice (a sum, a move), so that a page that is not
// there faults before anything is written and the instruction runs again
// as if it had not begun. Nothing to look at without the MMU.
function probe(m, addr, len, write) {
  if (!m.vm || len <= 0) return;
  const mmu = m.mmu, kind = write ? FK.WRITE : FK.READ, last = (addr + len - 1) | 0;
  for (let x = addr & ~4095; ; x = (x + 4096) | 0) {
    if ((x >>> 12) !== 0 && mmu.page(x, kind) < 0) throw FAULT;
    if (((x ^ last) & ~4095) === 0) break;
  }
}
// [addr, addr + len) as a view of the machine's own bytes, when they are
// one run of them: plain memory as runPlain's loads take it, or under the
// MMU a stretch within one page the task may read (write: and write). Else
// null, and the operation goes byte by byte with reader and writer. A page
// that is not there faults, as reader's and writer's do.
function span(m, addr, len, write) {
  const u8 = m.u8;
  if (len <= 0) return null;
  if (m.vm) {
    if ((addr >>> 12) === 0 || ((addr ^ (addr + len - 1)) & ~4095) !== 0) return null;
    const p = m.mmu.page(addr, write ? FK.WRITE : FK.READ);
    if (p < 0) throw FAULT;
    const at = p | (addr & 4095);
    return u8.subarray(at, at + len);
  }
  if (m.uwin) return null;
  if (addr >= 0x1000 && addr + len <= Math.min(m.guardLo(), u8.length)) return u8.subarray(addr, addr + len);
  return null;
}
// the n arguments, in the order the program wrote them
function args(m, n) {
  const sp = m.sp, a = new Array(n);
  for (let i = 0; i < n; i++) a[i] = m.ld32(sp + 4 * (n - i)) | 0;
  return a;
}
// a float's bits as the float, and back; a single's arithmetic is
// Math.fround of the double's, as c4's FPU makes it; a conversion to int
// that does not fit gives the most negative int, as c4's FPU and SSE do
const F = new Float32Array(1), FI = new Int32Array(F.buffer);
const fval = u => { FI[0] = u; return F[0]; };
const fbits = x => { F[0] = x; return FI[0]; };
const toInt = v => (v >= -2147483648 && v < 2147483648) ? Math.trunc(v) | 0 : (0x80000000 | 0);

export const kit = Object.freeze({ reader, writer, reader32, writer32, probe, span, args, fval, fbits, toInt, fround: Math.fround });

// Load the named modules, each once, in order: before a machine starts. A
// game's is "NAME" (games/NAME/c4/accel.mjs), an operating system's
// "os/NAME" (src/NAME/accel.mjs); either is asked for as NAME.
export async function loadAccel(names) {
  for (const entry of names || []) {
    if (typeof entry !== 'string') continue;
    const os = entry.startsWith('os/'), name = os ? entry.slice(3) : entry;
    if (!NAME.test(name) || index.has(name) || mods.length >= MAX_MODS) continue;
    try {
      const mod = await import(new URL(os ? `../src/${name}/accel.mjs` : `../games/${name}/c4/accel.mjs`, import.meta.url));
      const ops = mod.default(kit);
      if (!Array.isArray(ops)) throw new Error('its default export gave no array of operations');
      index.set(name, mods.length);
      mods.push({ entry, name, os, ops });
    } catch (e) {
      console.error(`libjs: the accelerators of ${name}: ${e.message}`);
    }
  }
}
export const accelNames = () => mods.map(x => x.entry);

// The build's list (build-images.sh): the page fetches it, node reads it.
// None there, none loaded: the games draw with their own loops.
export async function accelManifest() {
  const url = new URL('../src/c4bb/images/accel.json', import.meta.url);
  try {
    if (url.protocol === 'file:') {
      const { readFileSync } = await import('node:fs');
      return JSON.parse(readFileSync(url, 'utf8'));
    }
    const r = await fetch(url);
    return r.ok ? await r.json() : [];
  } catch { return []; }
}

// The machine's accel (c4m.js sys): ACC_FIND, ACC_OS, or a module's
// operation; -1 for a number nobody has, which is then a custom opcode like
// any other
export function accel(m, op) {
  if (op === ACC_FIND) {
    const name = new TextDecoder().decode(m.cstring(m.ld32(m.sp + 4) | 0, 33));
    const k = index.get(name);
    m.a = k === undefined || (mods[k].os && !osAccelOn()) ? 0 : ACC_BASE + ACC_SLOT * (k + 1);
    return STOP.BUDGET;
  }
  if (op === ACC_OS) {
    // __c4_opcode(v, ACC_OS): the OS modules' switch as it was, 1 or 0, and
    // turned on (v > 0) or off (v == 0); v < 0 only asks. 2 more where the
    // machine has an OS module (so a program can tell "off" from "none").
    const v = m.ld32(m.sp + 4) | 0, was = osAccelOn() ? 1 : 0;
    if (v >= 0) setOsAccel(v > 0);
    m.a = was | (mods.some(x => x.os) ? 2 : 0);
    return STOP.BUDGET;
  }
  const r = op - ACC_BASE - ACC_SLOT;
  const mod = r >= 0 ? mods[r >> 8] : undefined;
  const f = mod && mod.ops[r & 255];
  if (typeof f !== 'function') return -1;
  m.a = f(m) | 0;
  return STOP.BUDGET;
}
