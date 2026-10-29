#!/usr/bin/env node
// cli.js - run a .c4r on the libjs c4m from the command line.
//
//   node libjs/cli.js [-m MB] [-c cycles] [-s] [-d dir] [-w dir] [-i]
//                     [-hz MHz] [--fast] [--mbox BYTES] [--wav FILE] program.c4r [args...]
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
//   --cpus N     N processors (smp.js, c4mp's instruction set), simulated:
//                each runs -q instructions in turn (default 1000)
//   -q N         instructions a CPU runs before the next one's turn
//   --parallel   ... each CPU on a thread of its own (par.js), for real
//   --no-mmu     no MMU (docs/c4ix-mmu.md). One is fitted by default:
//                C4IX turns it on, and then user tasks run in address
//                spaces of their own, so a wild write is caught where it
//                happens; nothing else uses it. (--mmu: the default)
//   --wav FILE   fit a sound card (snd-device.js) and write what it played,
//                on the machine's own clock, to FILE: 44.1 kHz 16-bit stereo.
//                The same session makes the same file. Without it there is
//                no sound card, and programs run silent
//
// The flags are c4bb's (src/c4bb/sim/cli.js), so a test can swap one
// runner for the other. Piped stdin without -i is fed to the keyboard up
// front with end-of-file set, which is how scripted OS sessions run:
//   printf 'ps\nexit\n' | node libjs/cli.js -d src/c4bb/images/disk src/c4bb/images/c4ix32.c4r

import { readFileSync, readdirSync, statSync, writeFileSync, mkdirSync, existsSync, rmSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { spawn } from 'node:child_process';
import { constants } from 'node:os';
import { createMachine, boot } from './boot.js';
import { Worker } from 'node:worker_threads';
import { STOP } from './c4m.js';
import { SoundDevice } from './snd-device.js';
import { renderWav } from './snd-mix.js';

function usage() {
  console.error('usage: cli.js [-m MB] [-c cycles] [-s] [-d dir] [-w dir] [-i] [-hz MHz] [--fast] [--mbox BYTES] [--wav FILE] program.c4r [args...]');
  process.exit(1);
}

function parseArgs(argv) {
  const o = { arenaMb: 32, maxCycles: Infinity, stats: false, drives: [], mmu: true,
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
    else if (f === '--no-fpu') o.fpu = false;
    else if (f === '--mbox') o.mbox = parseInt(argv[++i], 10);
    else if (f === '--wav') o.wav = argv[++i];
    else if (f === '--cpus') o.cpus = parseInt(argv[++i], 10);
    else if (f === '-q') o.quantum = parseInt(argv[++i], 10);
    else if (f === '--parallel') o.parallel = true;
    else if (f === '--mmu') o.mmu = true;
    else if (f === '--no-mmu') o.mmu = false;
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
    remove: d.writable ? name => rmSync(join(d.dir, name), { force: true }) : null,
  }));

  const snd = o.wav ? new SoundDevice() : null;
  const played = [];
  const machine = createMachine({
    arenaMb: o.arenaMb, drives, mhz: o.mhz, fpu: o.fpu, snd, cpus: o.cpus, quantum: o.quantum,
    parallel: o.parallel, spawn: url => new Worker(url), mmu: o.mmu,
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
  // the machine's time: with several CPUs, the round robin's, not CPU 0's
  const clock = () => (machine.smp ? machine.smp.cycle : machine.cycle);
  // the parallel engine (par.js): CPU 0 answers the others between runs,
  // and waits for them, without blocking, when it is parked or halted
  const par = machine.smp && machine.smp.parallel ? machine.smp : null;
  let stuck = 0;
  for (;;) {
    const left = limit - clock();
    if (left <= 0) {
      flush();
      console.error(`libjs: stopped at cycle budget (${clock()} cycles)`);
      status = 9;
      break;
    }
    // Interactive and paced: slices of 16 simulated milliseconds, so the
    // pacing below never lets the machine get far ahead of the clock.
    const slice = o.interactive && !o.unpaced ? dev.cyclesPerMs * 16 : 50e6;
    const r = machine.run(Math.min(left, slice));
    flush();
    if (par && !dev.halted) {
      par.service();
      // CPU 0 naps while the others work: for real, until an IPI. The
      // machine's clock gets only what it slept.
      if (r === STOP.SLEEP && par.othersRunning()) {
        const nap = dev.napMs;
        let slept = 0;
        while (slept < nap && !par.ipiFor0() && !dev.halted) {
          slept += par.sleepSync(nap - slept);
          flush();
          await new Promise(res => setImmediate(res));   // stdin, timers
        }
        if (slept < nap) dev.sleepMs -= nap - slept;
        continue;
      }
      if (r === STOP.PARK || r === STOP.CPUHALT || machine.cpuState !== 'run') {
        if (machine.cpuState === 'halt' && par.othersDone()) { status = machine.cpuStatus; break; }
        // parked with every other CPU parked or halted, three seconds running: nothing can wake them
        const now = Date.now();
        stuck = par.allParked() ? stuck || now : 0;
        if (stuck && now - stuck >= 3000) { par.deadlock(); flush(); status = dev.status; break; }
        par.parkSync(10);
        flush();
        await new Promise(res => setImmediate(res));   // stdin, timers
        continue;
      }
      stuck = 0;
    }
    if (snd) { const e = snd.drain(); if (e) for (const x of e) played.push(x); }
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
  if (par) par.shutdown();
  if (process.stdin.isTTY && o.interactive) process.stdin.setRawMode(false);
  if (snd) {
    const e = snd.drain();
    if (e) for (const x of e) played.push(x);
    writeFileSync(o.wav, renderWav(played, 44100, Math.max(snd.now(), snd.cursor)));
  }
  if (o.stats) {
    const dt = (Date.now() - t0) / 1000;
    const ran = machine.cycle - machine.skipped;
    console.error(`libjs: ${ran} cycles in ${dt.toFixed(2)}s (${Math.round(ran / dt / 1000)}k inst/s), ` +
      `${machine.missedTraps} missed traps, status ${status}`);
    if (snd) { const st = snd.stats(); console.error(`libjs: sound: ${st.writes} OPL writes, ${st.plays} samples played`); }
  }
  return status & 0xff;
}

// Node 26 can hang in process exit: about one run in twenty (more under
// load) the main thread waits forever on a concurrent TurboFan job, after
// the program has finished and its output is out. So the machine runs in
// a child, which tells this process its status just before it exits; if
// it has not gone a few seconds later, it is stuck there and is killed.
// (--no-concurrent-recompilation avoids the hang too, at a third of the
// speed.)
if (process.send && process.env.LIBJS_CHILD) {
  const done = s => process.send({ status: s }, () => process.exit(s));
  main(process.argv.slice(2)).then(done, e => { console.error(e); done(70); });
} else {
  const child = spawn(process.execPath, [...process.execArgv, ...process.argv.slice(1)],
    { stdio: ['inherit', 'inherit', 'inherit', 'ipc'], env: { ...process.env, LIBJS_CHILD: '1' } });
  let status = null;
  child.on('message', m => {
    status = m.status;
    setTimeout(() => child.kill('SIGKILL'), 3000).unref();
  });
  child.on('exit', (code, sig) => process.exit(status !== null ? status : code !== null ? code : 128 + (constants.signals[sig] || 9)));
  // the child has the terminal's Ctrl-C to itself
  process.on('SIGINT', () => {});
}
