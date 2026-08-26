#!/usr/bin/env node
// cli.js - headless runner for the c4bb machine.
//
//   node src/c4bb/sim/cli.js [-m MB] [-c maxcycles] [-s] [-d dir] [-i]
//                            program.c4r [args...]
//
//   -m MB        arena size in megabytes (default 32)
//   -c cycles    cycle budget (default unlimited)
//   -s           print machine stats to stderr on exit
//   -d dir       serve every file in dir through the disk controller
//   -i           interactive: raw stdin feeds the keyboard device
//   --step       use the microstep engine instead of turbo
//
// Non-interactive runs with piped stdin prefeed it to the keyboard
// FIFO with EOF set, so scripted OS sessions work:
//   echo 'ps' | node cli.js -d disk/ c4ke32.c4r
//
// Firmware is loaded from src/c4bb/fw/fw.c4r next to this script.

import { readFileSync, readdirSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join, basename } from 'node:path';
import { Arena, CONS_RET } from './arena.js';
import { Devices } from './devices.js';
import { Machine, R } from './machine.js';
import { assemble } from './ucode.js';
import { boot, runToExit, callFunction } from './loader.js';
import { Turbo } from './turbo.js';

const here = dirname(fileURLToPath(import.meta.url));

function parseArgs(argv) {
  const o = { arenaMb: 32, maxCycles: Infinity, stats: false, useStep: false,
              diskDir: null, interactive: false };
  let i = 0;
  while (i < argv.length && argv[i].startsWith('-')) {
    if (argv[i] === '-m') o.arenaMb = parseInt(argv[++i], 10);
    else if (argv[i] === '-c') o.maxCycles = parseInt(argv[++i], 10);
    else if (argv[i] === '-s') o.stats = true;
    else if (argv[i] === '-d') o.diskDir = argv[++i];
    else if (argv[i] === '-i') o.interactive = true;
    else if (argv[i] === '--step') o.useStep = true;
    else { console.error(`c4bb: unknown option ${argv[i]}`); process.exit(1); }
    i++;
  }
  if (i >= argv.length) {
    console.error('usage: cli.js [-m MB] [-c cycles] [-s] [-d dir] [-i] [--step] program.c4r [args...]');
    process.exit(1);
  }
  o.progPath = argv[i];
  o.progArgs = argv.slice(i);           // argv[0] = program path, like c4l
  return o;
}

// Recursive: some disk entries are opened by a path with real slashes
// (innerbench's nested c4/c4m opens "src/c4ke/c4ke.c" directly, not
// through the vfs.txt manifest), and a filesystem is the only way to
// have a file literally named that - so subdirectories under diskDir
// are walked and exposed with their '/'-joined relative path as the
// disk key, matching what the guest's OPEN asks for verbatim.
function loadDisk(dir, rel = '') {
  const files = new Map();
  if (!dir) return files;
  const base = rel ? join(dir, rel) : dir;
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

async function main(argv) {
  const o = parseArgs(argv);
  const ucSource = readFileSync(join(here, '..', 'hw', 'microcode.uc'), 'utf8');
  const fwBytes = new Uint8Array(readFileSync(join(here, '..', 'fw', 'fw.c4r')));
  const progBytes = new Uint8Array(readFileSync(o.progPath));

  const arena = new Arena(o.arenaMb * 1024 * 1024);
  const out = [];
  const flush = () => { if (out.length) { process.stdout.write(Buffer.from(out)); out.length = 0; } };
  const dev = new Devices(arena, {
    files: loadDisk(o.diskDir),
    onByte: b => { out.push(b); if (out.length >= 4096 || b === 10) flush(); },
  });
  const machine = new Machine(arena, assemble(ucSource), dev);

  if (o.interactive && process.stdin.isTTY) {
    process.stdin.setRawMode(true);
    process.stdin.resume();
    process.stdin.on('data', buf => {
      for (const b of buf) {
        if (b === 3) {
          if (machine.signalHandlers.get(2)) machine.pendingSignal = 2;
          else { flush(); process.exit(130); }        // no handler: die
        } else if (b === 4) dev.rxEof = true;
        else if (dev.rawKbd > 0) {
          // A program has /dev/tty open, so it owns the screen: forward
          // the byte exactly as the tty sent it -- escape sequences and
          // all, which setRawMode already gives us -- and do NOT echo.
          // An echo here lands wherever the cursor happens to be and
          // corrupts whatever the program was painting.
          dev.rxFifo.push(b);
        } else {
          // Cooked: neither kernel's shell disables terminal echo
          // (c4sh.c's own char-reader has its putchar(c) commented out,
          // written expecting the host tty to do it, like a real
          // unmodified terminal always does).
          const c = b === 13 ? 10 : b;                // CR -> LF
          dev.rxFifo.push(c);
          process.stdout.write(String.fromCharCode(c));
        }
      }
    });
  } else if (!process.stdin.isTTY) {
    // scripted session: prefeed piped stdin, mark EOF
    try {
      const data = readFileSync(0);
      for (const b of data) dev.rxFifo.push(b);
    } catch { /* no stdin */ }
    dev.rxEof = true;
  }

  const t0 = Date.now();
  const { progImg } = boot(machine, fwBytes, progBytes, o.progArgs);
  const turbo = o.useStep ? null : new Turbo(machine);
  let status;
  try {
    if (o.interactive) {
      // chunked async loop so stdin events can arrive
      const limit = machine.cycle + o.maxCycles;
      for (;;) {
        if (machine.dev.halted) { status = machine.dev.status; break; }
        if (machine.regs[R.PC] === CONS_RET) {
          status = machine.regs[R.A];
          machine.regs[R.SP] = (machine.regs[R.SP] + 8) | 0;
          for (const d of progImg.des) callFunction(machine, d, []);
          break;
        }
        if (machine.cycle >= limit) throw new Error('c4bb: cycle budget exhausted');
        if (turbo) turbo.run(Math.min(2e6, limit - machine.cycle), CONS_RET);
        else { let n = 2e5; while (n-- && machine.step() &&
               machine.regs[R.PC] !== CONS_RET) ; }
        flush();
        await new Promise(r => setImmediate(r));
      }
    } else {
      status = runToExit(machine, progImg, o.maxCycles, turbo);
    }
  } catch (e) {
    if (String(e.message).includes('cycle budget exhausted')) {
      // an OS run with -c simply stops here; output up to now stands
      console.error(`c4bb: stopped at cycle budget (${machine.cycle} cycles)`);
      status = 9;
    } else throw e;
  } finally {
    flush();
    if (process.stdin.isTTY && o.interactive) process.stdin.setRawMode(false);
  }
  if (o.stats) {
    const dt = (Date.now() - t0) / 1000;
    console.error(`c4bb: ${machine.cycle} cycles in ${dt.toFixed(2)}s ` +
      `(${Math.round(machine.cycle / dt / 1000)}k inst/s), ` +
      `${machine.missedTraps | 0} missed traps, status ${status}`);
  }
  return status & 0xff;
}

main(process.argv.slice(2)).then(s => process.exit(s));
