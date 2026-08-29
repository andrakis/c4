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

import { readFileSync, readdirSync, statSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
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
              diskDir: null, drives: [], interactive: false,
              mhz: 20, unpaced: false };
  let i = 0;
  while (i < argv.length && argv[i].startsWith('-')) {
    if (argv[i] === '-m') o.arenaMb = parseInt(argv[++i], 10);
    else if (argv[i] === '-c') o.maxCycles = parseInt(argv[++i], 10);
    else if (argv[i] === '-s') o.stats = true;
    // -d attaches a read-only medium, -w a writable one, in the order
    // given: the first is drive 0, the next drive 1, and a guest reaches
    // another one by name ("1:c4ke.c4r") or by the DISK_DRIVE register.
    // docs/c4bb-storage.md. -d alone still means what it always did.
    else if (argv[i] === '-d') { o.diskDir = argv[++i]; o.drives.push({ dir: o.diskDir, writable: false }); }
    else if (argv[i] === '-w') { const dir = argv[++i]; o.drives.push({ dir, writable: true }); }
    else if (argv[i] === '-i') o.interactive = true;
    // How fast the machine claims to be, in MHz. It matters because
    // guests time themselves against it: C4KE sizes its preemption
    // interval from a measured instructions-per-second, and top
    // refreshes once a "second".
    else if (argv[i] === '-hz') o.mhz = parseFloat(argv[++i]);
    // Let it run as fast as the host can rather than pacing to the
    // machine's own clock. Faster, and time inside stops meaning
    // anything -- which is what batch runs want.
    else if (argv[i] === '--fast') o.unpaced = true;
    else if (argv[i] === '--step') o.useStep = true;
    else { console.error(`c4bb: unknown option ${argv[i]}`); process.exit(1); }
    i++;
  }
  // No program: the firmware boots on its own, the way a machine with
  // nothing but a BIOS does -- it looks for a medium. Which is the
  // whole point of having drives (docs/c4bb-storage.md M10).
  if (i >= argv.length) {
    if (!o.drives.length && !o.diskDir) {
      console.error('usage: cli.js [-m MB] [-c cycles] [-s] [-d dir] [-w dir] [-i] [-hz MHz] [--fast] [--step] [program.c4r [args...]]');
      console.error('       with no program, the firmware boots from the first medium that has one');
      process.exit(1);
    }
    o.progPath = null;
    o.progArgs = ['bios'];
    return o;
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
  // An empty drive is a real thing -- `-w mydisk` on a machine that has
  // never written one is a BLANK medium, not an error, and that is
  // exactly the case the whole install story starts from.
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

async function main(argv) {
  const o = parseArgs(argv);
  const ucSource = readFileSync(join(here, '..', 'hw', 'microcode.uc'), 'utf8');
  const fwBytes = new Uint8Array(readFileSync(join(here, '..', 'fw', 'fw.c4r')));
  let progBytes = o.progPath ? new Uint8Array(readFileSync(o.progPath)) : null;

  const arena = new Arena(o.arenaMb * 1024 * 1024);
  const out = [];
  const flush = () => { if (out.length) { process.stdout.write(Buffer.from(out)); out.length = 0; } };
  // A writable drive is a host directory, and closing a file there
  // writes it through -- so a medium really does survive the machine
  // being switched off, which is the whole point of having one.
  // A writable drive gets its directory made now rather than on the
  // first write, so that a blank medium exists on the host as soon as
  // it is in the machine -- and so `ls mydisk` says something.
  for (const d of o.drives) if (d.writable && d.dir) mkdirSync(d.dir, { recursive: true });
  const drives = (o.drives.length ? o.drives : [{ dir: o.diskDir, writable: false }])
    .map(d => ({
      files: loadDisk(d.dir),
      writable: d.writable,
      dir: d.dir,
      sink: d.writable && d.dir
        ? (name, bytes) => {
            const full = join(d.dir, name);
            mkdirSync(dirname(full), { recursive: true });
            writeFileSync(full, bytes);
          }
        : null,
    }));
  const dev = new Devices(arena, {
    drives,
    cyclesPerMs: Math.max(1, Math.round(o.mhz * 1000)),
    // A disk put into the machine while the BIOS is waiting for one.
    // Rescan is the machine asking to look again, which is what
    // putting a disk in looks like from inside -- so it also undoes an
    // eject.
    onRescan: n => {
      if (drives[n] && drives[n].dir) {
        drives[n].files = loadDisk(drives[n].dir);
        drives[n].ejected = false;
      }
    },
    onByte: b => { out.push(b); if (out.length >= 4096 || b === 10) flush(); },
  });
  const machine = new Machine(arena, assemble(ucSource), dev);

  // -i streams stdin as it arrives, whether stdin is a terminal or a
  // pipe. That distinction matters because C4KE hands the console to
  // the FOCUSED task: bytes that arrive while a task is running go to
  // that task, and a shell command queued behind a five-minute compile
  // is simply lost. Prefeeding the whole pipe (the branch below) is
  // right for a batch session and wrong for a scripted interactive one,
  // which is why the walkthrough in docs/climbing-the-ladder.md can be
  // driven by a shell script with sleeps in it.
  if (o.interactive) {
    if (process.stdin.isTTY) process.stdin.setRawMode(true);
    process.stdin.resume();
    process.stdin.on('end', () => { dev.rxEof = true; });
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
          //
          // Backspace is the line discipline's job and always was --
          // devices.js holds a line in rxFifo until Enter precisely so
          // that not-yet-committed characters can still be erased, and
          // then nothing ever erased them. DEL and BS both, because
          // terminals disagree about which one the key sends.
          if (b === 127 || b === 8) {
            const n = dev.rxFifo.length;
            if (n && dev.rxFifo[n - 1] !== 10) {
              dev.rxFifo.pop();
              process.stdout.write('\b \b');
            }
            continue;
          }
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
  // A soft reset re-runs everything below from a clean machine, which
  // is what a reset button does: the arena is zeroed, the firmware is
  // placed again, and the BIOS looks at the drives as if the power had
  // just come on. The drives themselves survive, because they are the
  // media -- that is the whole point of resetting rather than exiting.
  let progImg, turbo, status;
  // The reset comes back HERE. Everything in the loop is what happens
  // when the power comes on; everything above it is the machine itself,
  // which a reset does not rebuild -- the media in the drives least of
  // all, since a disk that was just written is the reason to reset.
  for (;;) {
  ({ progImg } = boot(machine, fwBytes, progBytes, o.progArgs));
  turbo = o.useStep ? null : new Turbo(machine);
  const tRun = Date.now();
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
        // Pace to the machine's own clock. Without this the guest's
        // idea of a second and the person's are different by a factor
        // of twenty, so `top` redraws twenty times a second and a
        // sleep() does not sleep. Deterministic: nothing about WHAT
        // runs changes, only when the host lets it. --fast opts out,
        // which is what batch runs want.
        if (!o.unpaced) {
          const ahead = dev.simMs() - (Date.now() - tRun);
          if (ahead > 2) await new Promise(r => setTimeout(r, Math.min(ahead, 100)));
          else await new Promise(r => setImmediate(r));
        } else await new Promise(r => setImmediate(r));
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
  }
  if (!dev.resetRequested) break;
  flush();
  console.error('c4bb: soft reset');
  arena.u8.fill(0);
  dev.resetRequested = false;
  dev.halted = false;
  dev.fds = new Map();
  dev.nextFd = 3;
  dev.drive = 0;
  machine.reset();
  // Re-read the media -- but not one that was taken OUT. Restarting a
  // machine does not put a disk back in it, and the whole climb turns
  // on that: install a boot medium in drive 1, eject drive 0, reset,
  // and the BIOS must find drive 1 rather than the disk it just came
  // from. (`d.dir` is the host directory; a drive with none was empty
  // to begin with.)
  for (const d of drives) if (d.dir && !d.ejected) d.files = loadDisk(d.dir);
  // A reset boots the BIOS, not whatever image was named at start: the
  // disk that has just been written is the one that should come up.
  progBytes = null;
  }
  try { } finally {
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
