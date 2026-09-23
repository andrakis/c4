#!/usr/bin/env node
// cli.js - run a .c4r on the libjs c4m from the command line.
//
//   node libjs/cli.js [-m MB] [-c cycles] [-s] [-d dir] [-w dir] [-i]
//                     [-hz MHz] [--fast] [--mbox BYTES] program.c4r [args...]
//
//   -m MB        arena size in megabytes (default 32)
//   -c cycles    cycle budget (default unlimited); running out is status 9
//   -s           print machine stats to stderr on exit
//   -d dir       a read-only drive: every file under dir, by relative path
//   -w dir       a writable drive: files the guest writes land in dir
//   -i           interactive: stdin streams to the keyboard as it arrives
//   -hz MHz      how fast the machine claims to be (default 20)
//   --fast       interactive runs go as fast as the host can instead of
//                pacing to the machine's own clock
//   --mbox N     fit a mailbox of N bytes (c4bb's MBOX_* registers)
//
// The flags are c4bb's (src/c4bb/sim/cli.js), so a test can swap one
// runner for the other. Piped stdin without -i is fed to the keyboard up
// front with end-of-file set, which is how scripted OS sessions run:
//   printf 'ps\nexit\n' | node libjs/cli.js -d src/c4bb/images/disk src/c4bb/images/c4ix32.c4r

import { readFileSync, readdirSync, statSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { createMachine, boot } from './boot.js';
import { STOP } from './c4m.js';

function usage() {
  console.error('usage: cli.js [-m MB] [-c cycles] [-s] [-d dir] [-w dir] [-i] [-hz MHz] [--fast] [--mbox BYTES] program.c4r [args...]');
  process.exit(1);
}

function parseArgs(argv) {
  const o = { arenaMb: 32, maxCycles: Infinity, stats: false, drives: [],
              interactive: false, mhz: 20, unpaced: false, mbox: 0 };
  let i = 0;
  while (i < argv.length && argv[i].startsWith('-')) {
    const f = argv[i];
    if (f === '-m') o.arenaMb = parseInt(argv[++i], 10);
    else if (f === '-c') o.maxCycles = parseInt(argv[++i], 10);
    else if (f === '-s') o.stats = true;
    else if (f === '-d') o.drives.push({ dir: argv[++i], writable: false });
    else if (f === '-w') o.drives.push({ dir: argv[++i], writable: true });
    else if (f === '-i') o.interactive = true;
    else if (f === '-hz') o.mhz = parseFloat(argv[++i]);
    else if (f === '--fast') o.unpaced = true;
    else if (f === '--mbox') o.mbox = parseInt(argv[++i], 10);
    else if (f === '--') { i++; break; }
    else { console.error(`libjs: unknown option ${f}`); usage(); }
    i++;
  }
  if (i >= argv.length) usage();
  o.progPath = argv[i];
  o.progArgs = argv.slice(i);            // argv[0] = the image path, like c4l
  return o;
}

// Every file under dir, keyed by its '/'-joined relative path: some
// guests open nested paths verbatim (c4bb's cli.js does the same).
function loadDisk(dir, rel = '') {
  const files = new Map();
  if (!dir) return files;
  const base = rel ? join(dir, rel) : dir;
  if (!existsSync(base)) return files;
  for (const name of readdirSync(base)) {
    const p = join(base, name);
    const key = rel ? `${rel}/${name}` : name;
    if (statSync(p).isDirectory()) for (const [k, v] of loadDisk(dir, key)) files.set(k, v);
    else files.set(key, new Uint8Array(readFileSync(p)));
  }
  return files;
}

async function main(argv) {
  const o = parseArgs(argv);
  const image = new Uint8Array(readFileSync(o.progPath));

  const out = [];
  const flush = () => { if (out.length) { process.stdout.write(Buffer.from(out)); out.length = 0; } };
  for (const d of o.drives) if (d.writable) mkdirSync(d.dir, { recursive: true });
  const drives = (o.drives.length ? o.drives : [{ dir: null, writable: false }]).map(d => ({
    files: loadDisk(d.dir),
    writable: d.writable,
    sink: d.writable ? (name, bytes) => {
      const full = join(d.dir, name);
      mkdirSync(dirname(full), { recursive: true });
      writeFileSync(full, bytes);
    } : null,
  }));

  const machine = createMachine({
    arenaMb: o.arenaMb, drives, mhz: o.mhz,
    onByte: b => { out.push(b); if (out.length >= 4096 || b === 10) flush(); },
  });
  const dev = machine.dev;

  // Input. -i streams it; otherwise piped stdin is fed up front with EOF.
  let wake = null;                       // resolves a parked run loop
  const poke = () => { if (wake) { const w = wake; wake = null; w(); } };
  if (o.interactive) {
    if (process.stdin.isTTY) process.stdin.setRawMode(true);
    process.stdin.resume();
    process.stdin.on('end', () => { dev.rxEof = true; poke(); });
    process.stdin.on('data', buf => {
      for (const b of buf) {
        if (b === 3) {                                   // Ctrl-C
          if (machine.signalHandlers.get(2)) machine.pendingSignal = 2;
          else { flush(); process.exit(130); }
        } else if (b === 4) dev.rxEof = true;          // Ctrl-D
        else if (dev.rawKbd > 0) dev.rxFifo.push(b);    // a program owns the tty
        else if (b === 127 || b === 8) {                 // cooked: erase
          const n = dev.rxFifo.length;
          if (n && dev.rxFifo[n - 1] !== 10) { dev.rxFifo.pop(); process.stdout.write('\b \b'); }
        } else {
          const c = b === 13 ? 10 : b;
          dev.rxFifo.push(c);
          process.stdout.write(String.fromCharCode(c));  // local echo, as a tty does
        }
      }
      poke();
    });
  } else if (!process.stdin.isTTY) {
    try { for (const b of readFileSync(0)) dev.rxFifo.push(b); } catch { /* no stdin */ }
    dev.rxEof = true;
  } else {
    dev.rxEof = true;
  }

  boot(machine, image, o.progArgs, { mbox: o.mbox });

  const t0 = Date.now();
  const limit = o.maxCycles;
  let status, parkedMs = 0;
  for (;;) {
    const left = limit - machine.cycle;
    if (left <= 0) {
      flush();
      console.error(`libjs: stopped at cycle budget (${machine.cycle} cycles)`);
      status = 9;
      break;
    }
    // Interactive and paced: slices of 16 simulated milliseconds, so the
    // pacing below never lets the machine get far ahead of the clock.
    const slice = o.interactive && !o.unpaced ? dev.cyclesPerMs * 16 : 50e6;
    const r = machine.run(Math.min(left, slice));
    flush();
    if (r === STOP.HALT) { status = dev.status; break; }
    if (r === STOP.INPUT) {
      if (!o.interactive) {
        // Batch: stdin is already at EOF, so a read cannot block. If one
        // does, nothing will ever arrive.
        console.error('libjs: blocked on input with nothing left to read');
        status = 8;
        break;
      }
      // Parked until a key, or until the PIT is next due.
      const t = Date.now();
      await new Promise(res => {
        wake = res;
        if (dev.pitMs) setTimeout(poke, Math.max(1, dev.pitNext - dev.hostNow()));
      });
      parkedMs += Date.now() - t;
      continue;
    }
    if (o.interactive) {
      if (!o.unpaced) {
        // Pace to the machine's own clock (cli.js in c4bb does the same):
        // a simulated second should take a real one, or `top` redraws
        // twenty times a second.
        const ahead = dev.simMs() - (Date.now() - t0 - parkedMs);
        if (ahead > 2) {
          await new Promise(res => { wake = res; setTimeout(poke, Math.min(ahead, 100)); });
          continue;
        }
      }
      await new Promise(res => setImmediate(res));
    }
  }
  flush();
  if (process.stdin.isTTY && o.interactive) process.stdin.setRawMode(false);
  if (o.stats) {
    const dt = (Date.now() - t0) / 1000;
    const ran = machine.cycle - machine.skipped;
    console.error(`libjs: ${ran} cycles in ${dt.toFixed(2)}s (${Math.round(ran / dt / 1000)}k inst/s), ` +
      `${machine.missedTraps} missed traps, status ${status}`);
  }
  return status & 0xff;
}

main(process.argv.slice(2)).then(s => process.exit(s), e => { console.error(e); process.exit(70); });
