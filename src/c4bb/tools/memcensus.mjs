// memcensus.mjs -- how much memory an image actually needs.
//
// opscan.mjs answers the OPCODE half of "what does this program require
// of the machine". This is the other half, and until now it was not
// measured at all: the Makefile carries nine hand-picked `-m` values
// (32, 96, 128, 160, 192, 224, 512, 640) and C4IX_CELLS was found with
// a manual doubling ladder. A number arrived at by trying it until it
// stopped failing is a number nobody can defend when it later does.
//
// Two readings per image, both from a real run under the full firmware:
//
//   DATA   the highest address ever written at or below HEAP_END --
//          image, globals and heap. Image size says nothing about heap
//          appetite, which is why this is a run and not a static scan.
//
//   STACK  how far SP ever fell below the top of the arena -- the
//          MACHINE stack only. C4KE gives each task a kmalloc'd stack,
//          so task depth lands in DATA, which is the right place for
//          sizing -m. This column is the kernel's own frames.
//          Measured by
//          watching WRITES rather than sampling SP between chunks: a
//          deep call that returns before the next sample is invisible
//          to sampling and is exactly the thing that overruns a stack.
//
// The arena is deliberately large while measuring (the point is to find
// the high-water mark, not to see whether it fits), so a reading close
// to the arena size means the image wanted more than it was given and
// the number is a floor, not a peak. That case is reported.
//
// Usage:
//   node src/c4bb/tools/memcensus.mjs [-m MB] [-c cycles] [-d dir]
//                                     [-json out.json] image.c4r [...]
//
// See docs/dos-rung-fixes.md and Homeward's scripts/opcensus.ts, which
// is where the approach comes from.

import { readFileSync, writeFileSync, readdirSync, statSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { Arena, CONS_RET } from '../sim/arena.js';
import { Devices, HEAP_END } from '../sim/devices.js';
import { Machine } from '../sim/machine.js';
import { assemble } from '../sim/ucode.js';
import { boot } from '../sim/loader.js';
import { Turbo } from '../sim/turbo.js';

const ROOT = new URL('../../..', import.meta.url).pathname;
const UC = readFileSync(`${ROOT}src/c4bb/hw/microcode.uc`, 'utf8');
const FW = readFileSync(`${ROOT}src/c4bb/fw/fw.c4r`);

// cli.js's loader, same recursive walk so subdirectories (src/c4ke/c4ke.c)
// are present exactly as they are on a real medium.
function loadDisk(dir, rel = '') {
  const files = new Map();
  if (!dir) return files;
  const base = rel ? join(dir, rel) : dir;
  if (!existsSync(base)) return files;
  for (const name of readdirSync(base)) {
    const p = join(base, name);
    const key = rel ? `${rel}/${name}` : name;
    if (statSync(p).isDirectory()) {
      for (const [k, v] of loadDisk(dir, key)) files.set(k, v);
    } else {
      files.set(key, new Uint8Array(readFileSync(p)));
    }
  }
  return files;
}

function usage() {
  console.error('usage: memcensus.mjs [-m MB] [-c cycles] [-d dir] [-json out] image.c4r [...]');
  process.exit(2);
}

const opts = { mb: 256, cycles: 2_000_000_000, dir: null, json: null, args: [] };
const images = [];
for (let i = 2; i < process.argv.length; i++) {
  const a = process.argv[i];
  if (a === '-m') opts.mb = parseInt(process.argv[++i], 10);
  else if (a === '-c') opts.cycles = parseInt(process.argv[++i], 10);
  else if (a === '-d') opts.dir = process.argv[++i];
  else if (a === '-json') opts.json = process.argv[++i];
  else if (a === '--') { opts.args = process.argv.slice(i + 1); break; }
  else if (a.startsWith('-')) usage();
  else images.push(a);
}
if (!images.length) usage();

function census(path) {
  const bytes = readFileSync(path);
  const arena = new Arena(opts.mb * 1024 * 1024);
  // Devices wants a DRIVES ARRAY, not a directory -- passing {diskDir}
  // is silently ignored, and an image that boots from a disk then runs
  // with no disk, returns in a few thousand cycles and reports an
  // appetite of nothing. Same shape cli.js builds.
  const drives = [{ files: loadDisk(opts.dir), writable: false }];
  const dev = new Devices(arena, { drives });
  const machine = new Machine(arena, assemble(UC), dev, { onLog: () => {} });

  const b = boot(machine, FW, bytes, [path, ...opts.args]);

  // HEAP_END splits the two questions. Everything at or below it is
  // image/globals/heap growing UP; everything above is the stack coming
  // DOWN from stackTop. Without the split, "highest address written" is
  // always the stack and tells you nothing about the heap.
  const heapEnd = dev.read32(HEAP_END);
  let peakData = 0;
  let lowStack = arena.stackTop;
  const touch = a => {
    if (a <= heapEnd) { if (a > peakData) peakData = a; }
    else if (a < lowStack) lowStack = a;
  };
  const w32 = arena.write32.bind(arena);
  const w8 = arena.write8.bind(arena);
  const wB = arena.writeBytes.bind(arena);
  arena.write32 = (a, v) => { touch(a); return w32(a, v); };
  arena.write8 = (a, v) => { touch(a); return w8(a, v); };
  arena.writeBytes = (a, by) => {
    touch(a);
    if (a + by.length <= heapEnd && a + by.length > peakData) peakData = a + by.length;
    return wB(a, by);
  };

  const turbo = new Turbo(machine);
  const CHUNK = 500_000;
  let outcome = b && b.outcome ? b.outcome : 'ok';
  if (outcome === 'ok') {
    while (machine.cycle < opts.cycles && !dev.halted) {
      turbo.run(CHUNK, CONS_RET);
      if (machine.regs[0] === CONS_RET) break;
    }
    outcome = dev.halted ? 'exited'
            : machine.regs[0] === CONS_RET ? 'returned'
            : 'capped';
  }
  return {
    image: path,
    dataBytes: peakData,
    stackBytes: Math.max(0, arena.stackTop - lowStack),
    cycles: machine.cycle,
    outcome,
    arenaMb: opts.mb,
  };
}

// Bytes below a kilobyte: a kernel whose own frames come to 236 bytes
// should not read as "0K", which is what sent me looking for a bug in
// the instrumentation that was not there.
const K = n => n < 1024 ? n + 'B' : (n / 1024).toFixed(0) + 'K';
const rows = [];
let worst = 0;
for (const img of images) {
  let r;
  try { r = census(img); }
  catch (e) { console.log(`${img}: FAILED -- ${e.message}`); continue; }
  rows.push(r);
  const total = r.dataBytes + r.stackBytes;
  if (total > worst) worst = total;
  // A reading that comes near the arena it was measured in is a floor,
  // not a peak: the image wanted more than it was given.
  const nearCap = total > opts.mb * 1024 * 1024 * 0.9;
  console.log(
    `${img.padEnd(34)} data ${K(r.dataBytes).padStart(8)}  stack ${K(r.stackBytes).padStart(7)}` +
    `  ${String(r.outcome).padEnd(8)} ${r.cycles.toLocaleString()} cycles` +
    (nearCap ? '   *** near the arena cap: this is a FLOOR, re-run with more -m' : ''));
}
if (rows.length) {
  const mb = Math.ceil(worst / (1024 * 1024));
  console.log(`\nlargest appetite: ${K(worst)} (${mb} MB). ` +
              `A machine running these wants -m ${Math.max(2, mb * 2)} for headroom.`);
}
if (opts.json) {
  writeFileSync(opts.json, JSON.stringify(rows, null, 1) + '\n');
  console.log(`wrote ${opts.json}`);
}
