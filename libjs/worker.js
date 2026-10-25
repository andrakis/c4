// worker.js - the machine, in a Web Worker.
//
// The page never touches the interpreter. It sends messages (boot, keys,
// signals, display events) and gets messages back (console bytes,
// status, draw commands). postMessage only, so this works from any
// static server; nothing here needs cross-origin isolation.
//
// Page -> worker
//   boot    { image: ArrayBuffer, argv, disk: [[name, ArrayBuffer]], arenaMb, mhz, unpaced, gui, snd, shared }
//           snd: true fits the sound card (snd-device.js)
//           persist: { files: [[name, Uint8Array]] } fits a writable drive 1, kept by the page
//           uploads: [[name, Uint8Array]] laid over drive 0: the disk's file of that name is hidden
//           cpus, quantum: that many processors, simulated (smp.js)
//           parallel: ... each on a worker of its own (par.js), when the page is
//           cross-origin isolated; simulated otherwise
//           shared: { ring, fb } SharedArrayBuffers from shared.js, when the page is isolated
//   key     { key, ctrlKey, altKey, metaKey }     a keydown on the terminal
//   input   { bytes: Uint8Array }                 typed programmatically
//   signal  { sig }                               2 = Ctrl-C
//   eof     {}                                    Ctrl-D
//   gui     { kind, ... }                         a display event (gui-device.js)
//   pause / resume / reset
//   reset-files                                   drive 1 emptied, then reset; answered with files-reset
//   peek    { id, addr, len }                     read guest memory
//   poke    { id, addr, bytes }                   write guest memory
// Worker -> page
//   out     { bytes }         console output, and the echo of what was typed
//   kbd     { raw }           a program opened or closed /dev/tty
//   display { frames }        draw commands (gui-device.js); not used on the shared path
//   snd     { events, now }   the sound card's events, and its clock when sent (snd-device.js)
//   persist { name, bytes }   a file the machine closed on drive 1, for the page to keep
//   files-reset {}            drive 1 is empty and the machine is booting again
//   status  { ... }           about four times a second
//   exit    { status, cycles }
//   reply   { id, ... }       for peek and poke
//   error   { message }

import { createMachine, boot } from './boot.js';
import { STOP } from './c4m.js';
import { keyDown, typeBytes } from './keys.js';
import { GuiDevice } from './gui-device.js';
import { SoundDevice } from './snd-device.js';
import { RingWriter, FbProducer } from './shared.js';

// A disk read on demand. The page sends an index of [name, size] and a
// base URL; the first get() of a name fetches that one file with a
// synchronous XHR (allowed in a worker, and the machine's OPEN is
// synchronous anyway) and keeps it. Names not in the index are absent,
// as on an eager disk, and files the machine writes are kept like any
// other. The fetch count is in stats, for the tests.
class LazyFiles extends Map {
  constructor(base, index) {
    super();
    this.base = base;
    this.sizes = new Map(index);
    this.fetched = 0;
    this.fetchedBytes = 0;
  }
  has(name) { return super.has(name) || this.sizes.has(name); }
  get(name) {
    if (super.has(name)) return super.get(name);
    if (!this.sizes.has(name)) return undefined;
    const x = new XMLHttpRequest();
    x.open('GET', this.base + name.split('/').map(encodeURIComponent).join('/'), false);
    x.responseType = 'arraybuffer';
    x.send();
    if (x.status !== 200) { this.sizes.delete(name); return undefined; }
    const b = new Uint8Array(x.response);
    this.fetched++; this.fetchedBytes += b.length;
    super.set(name, b);
    return b;
  }
  *keys() { const seen = new Set(super.keys()); yield* seen; for (const k of this.sizes.keys()) if (!seen.has(k)) yield k; }
}

// Drive 1, when the page keeps one: made once, so a reset boots with what
// the machine last wrote there, not what the page had at load.
let persistDrive = null;
let vm = null, bootMsg = null, gui = null, snd = null, ring = null, ringStalls = 0, handoffMs = 0;
// One producer per shared framebuffer for the Worker's whole life: which
// slot is ours is state the page's side depends on, and a reset must not
// forget it.
let fbProducer = null, fbSab = null;
let paused = false, halted = false, parked = false;
let out = [];
let tRun = 0, lostMs = 0, parkedAt = 0;
let busyMs = 0, lastStatus = { t: 0, cycle: 0, busy: 0 };
let lastRaw = 0, state = 'idle';

// Scheduling. A slice runs, then yields so messages can arrive. The
// zero-delay yield is a MessageChannel, because setTimeout(0) is clamped
// to 4 ms once nested; a generation number makes every earlier wake-up
// a no-op, so there is only ever one tick queued.
let gen = 0;
const chan = new MessageChannel();
chan.port1.onmessage = e => { if (e.data === gen) tick(); };
function schedule(ms) {
  const g = ++gen;
  if (ms <= 0) chan.port2.postMessage(g);
  else setTimeout(() => { if (g === gen) tick(); }, ms);
}

function flush() {
  if (!out.length) return;
  const bytes = new Uint8Array(out);
  out = [];
  postMessage({ type: 'out', bytes }, [bytes.buffer]);
}

function flushDisplay() {
  if (!gui) return;
  const frames = gui.drain();
  if (frames) {
    if (ring) {
      // Shared path: into the ring, as much as fits; the rest waits for
      // the page to catch up (a background tab draws nothing).
      const n = ring.write(frames);
      if (n < frames.length) { gui.putBack(frames.subarray(n)); ringStalls++; }
    } else postMessage({ type: 'display', frames }, [frames.buffer]);
  }
  const req = gui.takeSizeRequest();
  if (req) postMessage({ type: 'gui', w: req.w, h: req.h });
}

// The sound card's events, for the page's AudioWorklet. Its clock is
// this one, real time: the audio plays in real time whether or not the
// machine keeps up, so the stream is kept ahead of the clock the audio
// goes by (snd-device.js).
function flushSound() {
  if (!snd) return;
  const events = snd.drain();
  if (!events) return;
  const transfer = [];
  for (const e of events) if (e[5] instanceof Uint8Array) transfer.push(e[5].buffer);
  postMessage({ type: 'snd', events, now: performance.now() * 1000 }, transfer);
}

// the parallel engine's host side (par.js), when there is one
let par = null, parStuck = 0;     // since when every CPU has been parked, or 0
// While CPU 0 is not running -- paced, parked, or waiting for a key --
// the other CPUs' requests are answered as soon as they ask
// An IPI for CPU 0 ends its nap, as a message for the machine does.
function watchPar() {
  if (!par || halted) return;
  if (state === 'sleeping' && par.ipiFor0()) wake();
  par.watch(watchPar);
}

function start(msg) {
  if (par) { par.shutdown(); par = null; }
  parStuck = 0;
  bootMsg = msg;
  ring = msg.gui && msg.shared ? new RingWriter(msg.shared.ring) : null;
  if (ring && fbSab !== msg.shared.fb) { fbSab = msg.shared.fb; fbProducer = new FbProducer(fbSab); }
  gui = msg.gui ? new GuiDevice({ ...msg.gui, fbShared: ring ? fbProducer : null }) : null;
  snd = msg.snd ? new SoundDevice({ now: () => performance.now() * 1000 }) : null;
  ringStalls = 0;
  // Files arrive as Uint8Arrays or bare ArrayBuffers; the disk
  // controller wants bytes it can index.
  const files = msg.lazy ? new LazyFiles(msg.lazy.base, msg.lazy.index)
    : new Map((msg.disk || []).map(([n, b]) => [n, b instanceof Uint8Array ? b : new Uint8Array(b)]));
  for (const [n, b] of msg.uploads || []) files.set(n, b instanceof Uint8Array ? b : new Uint8Array(b));
  const drives = [{ files, writable: false, sink: null }];
  if (msg.persist) {
    if (!persistDrive) persistDrive = {
      files: new Map(msg.persist.files), writable: true,
      sink: (name, bytes) => { const b = bytes.slice(); postMessage({ type: 'persist', name, bytes: b }, [b.buffer]); },
    };
    drives.push(persistDrive);
  }
  vm = createMachine({
    arenaMb: msg.arenaMb || 64, drives, mhz: msg.mhz || 20, gui, snd, fpu: msg.fpu,
    cpus: msg.cpus | 0, quantum: msg.quantum | 0,
    parallel: !!msg.parallel, spawn: url => new Worker(url, { type: 'module' }),
    onByte: b => { out.push(b); if (out.length >= 4096) flush(); },
  });
  boot(vm, new Uint8Array(msg.image), msg.argv || ['image.c4r'], { mbox: msg.mbox | 0 });
  par = vm.smp && vm.smp.parallel ? vm.smp : null;
  watchPar();
  paused = false; halted = false; parked = false;
  tRun = performance.now(); lostMs = 0; busyMs = 0;
  lastStatus = { t: tRun, cycle: 0, busy: 0 };
  lastRaw = 0;
  state = 'running';
  schedule(0);
}

function tick() {
  if (!vm || paused || halted) return;
  if (parWaiting()) return;
  const now = performance.now();
  if (parked) { lostMs += now - parkedAt; parked = false; }
  const dev = vm.dev;
  // Paced, a slice is 16 simulated milliseconds, so the machine is never
  // more than a frame ahead of the clock. Unpaced, it is simply big.
  const slice = bootMsg.unpaced ? 5e6 : dev.cyclesPerMs * 16;
  const t = performance.now();
  let r;
  try {
    r = vm.run(slice);
  } catch (e) {
    flush();
    halted = true; state = 'halted';
    postMessage({ type: 'error', message: `libjs: ${e.message}` });
    return;
  }
  busyMs += performance.now() - t;
  flush();
  if (par && !dev.halted && r !== STOP.HALT) {
    par.service();
    if (r === STOP.PARK || r === STOP.CPUHALT || vm.cpuState !== 'run') {
      flushDisplay(); flushSound();
      // every CPU parked or halted for three seconds, none asking: nothing can wake them
      const now = performance.now();
      parStuck = par.allParked() ? parStuck || now : 0;
      if (parStuck && now - parStuck >= 3000) { par.deadlock(); flush(); }
      else if (vm.cpuState === 'halt' && par.othersDone()) { dev.halted = true; dev.status = vm.cpuStatus; }
      if (dev.halted) {
        halted = true; state = 'halted';
        postStatus(true);
        postMessage({ type: 'exit', status: dev.status, cycles: vm.cycle });
        par.shutdown();
        return;
      }
      // CPU 0 waits for its word, an IPI or another CPU, a few ms at a
      // time, so the page's messages are taken between
      state = 'parked';
      par.parkSync(8);
      schedule(0);
      return;
    }
    parStuck = 0;
  }
  const td = performance.now();
  flushDisplay();
  flushSound();
  handoffMs += performance.now() - td;
  if (dev.rawKbd !== lastRaw) { lastRaw = dev.rawKbd; postMessage({ type: 'kbd', raw: lastRaw > 0 }); }

  if (r === STOP.HALT) {
    if (par) par.shutdown();
    halted = true; state = 'halted';
    postStatus(true);
    postMessage({ type: 'exit', status: dev.status, cycles: vm.cycle });
    return;
  }
  if (r === STOP.INPUT) {
    // Waiting for a key with no timer armed: nothing happens until a
    // message arrives. With the PIT armed, come back when it is due.
    parked = true; parkedAt = performance.now(); state = 'blocked';
    if (dev.pitMs) schedule(Math.max(1, dev.pitNext - dev.hostNow()));
    return;
  }
  state = r === STOP.SLEEP ? 'sleeping' : 'running';
  if (!bootMsg.unpaced) {
    // Pace to the machine's own clock: guests time themselves against
    // TIME (cycles / MHz plus time slept), so a simulated second should
    // take a real one.
    const ahead = dev.simMs() - (performance.now() - tRun - lostMs);
    if (ahead > 2 && par && par.othersRunning()) {
      parWaitUntil = performance.now() + Math.min(ahead, 100);
      schedule(0);
      return;
    }
    if (ahead > 2) { schedule(Math.min(ahead, 100)); return; }
    // Behind. With realtime (the page's default), the machine's clock is
    // the wall's: TIME moves on by what the CPU could not get done, so a
    // host slower than the MHz asked for gives the guest fewer cycles a
    // second, not a slower second -- a game drops frames rather than
    // playing in slow motion, and the sound card's stream (its time is
    // TIME's) keeps up. Without it, the machine's own second stretches.
    if (bootMsg.realtime && ahead < -20) dev.sleepMs += -ahead;
    else if (ahead < -1000) lostMs += -ahead - 1000;   // hopelessly behind: stop trying to catch up
  } else if (par && r === STOP.SLEEP && par.othersRunning()) {
    // unpaced, CPU 0 still naps while the others work, until an IPI
    parWaitUntil = performance.now() + Math.min(dev.napMs, 100);
  }
  schedule(0);
}

// CPU 0 waits while the other CPUs work -- paced to the clock, or napping
// unpaced -- blocked in Atomics.wait, a few ms at a time, so their
// requests are answered at once (par.js sleepSync); between turns the
// worker takes its messages. An IPI ends the wait, and a nap with it.
let parWaitUntil = 0;
function parWaiting() {
  if (!parWaitUntil) return false;
  const left = parWaitUntil - performance.now();
  if (left <= 0 || !par || !par.othersRunning()) { parWaitUntil = 0; return false; }
  if (par.ipiFor0()) { parWaitUntil = 0; wake(); return true; }
  par.sleepSync(left, 8);
  schedule(0);
  return true;
}

// A message for the machine (a key, a pointer move) while it waits for
// the clock. Paced, the machine's time must not get ahead of the real
// one, however many messages come:
// - asleep (it napped, USLP), the nap is cut short at the real time --
//   the guest has not read the clock since it napped, so to it the nap
//   simply ended early, as an interrupt would end it -- and it runs now;
// - ahead after work, it does not run early: the wait already armed
//   ends within a slice, and the message is taken then.
// Running at once and keeping the whole nap gave the guest a nap's worth
// of time for every pointer move: top, the raycasters and System Monitor
// all sped up while the mouse moved (5x, measured).
function wake() {
  if (!vm || paused || halted) return;
  if (!bootMsg.unpaced) {
    const dev = vm.dev;
    const ahead = dev.simMs() - (performance.now() - tRun - lostMs);
    if (ahead > 0 && state === 'running') return;     // a wait is armed
    if (ahead > 0 && state === 'sleeping') {
      const cut = Math.min(Math.floor(ahead), dev.napMs);
      dev.sleepMs -= cut;
      dev.napMs -= cut;
    }
  }
  parWaitUntil = 0;
  schedule(0);
}

function postStatus(force) {
  if (!vm) return;
  const now = performance.now();
  const dt = now - lastStatus.t;
  if (!force && dt < 200) return;
  const ran = vm.cycle - vm.skipped;
  postMessage({
    type: 'status',
    state: paused ? 'paused' : state,
    cycle: vm.cycle,
    ips: dt > 0 ? Math.round((ran - lastStatus.cycle) / dt * 1000) : 0,
    busy: dt > 0 ? Math.min(1, (busyMs - lastStatus.busy) / dt) : 0,
    heapUsed: vm.heap.used,
    missedTraps: vm.missedTraps,
    simMs: vm.dev.simMs(),
    raw: vm.dev.rawKbd > 0,
    runMs: Math.round(busyMs),
    disk: lazyStats(),
    gui: gui ? { ...gui.stats(), shared: !!ring, ringStalls, handoffMs: Math.round(handoffMs) } : null,
    snd: snd ? snd.stats() : null,
  });
  lastStatus = { t: now, cycle: ran, busy: busyMs };
}
setInterval(() => postStatus(false), 250);

function lazyStats() {
  const f = vm && vm.dev.drives[0] && vm.dev.drives[0].files;
  return f instanceof LazyFiles ? { lazy: true, files: f.sizes.size, fetched: f.fetched, bytes: f.fetchedBytes } : { lazy: false };
}

function echo(bytes) { for (const b of bytes) out.push(b); flush(); }

self.onmessage = e => {
  const m = e.data;
  try {
    switch (m.type) {
      case 'boot': start(m); break;
      case 'reset': if (bootMsg) start(bootMsg); break;
      case 'reset-files':
        if (persistDrive) persistDrive.files = new Map();
        if (bootMsg) start(bootMsg);
        postMessage({ type: 'files-reset' });
        break;
      case 'key': if (vm) { echo(keyDown(vm, m).echo); wake(); } break;
      case 'input': if (vm) { echo(typeBytes(vm, m.bytes)); wake(); } break;
      case 'signal': if (vm) { vm.pendingSignal = m.sig | 0 || 2; wake(); } break;
      case 'eof': if (vm) { vm.dev.rxEof = true; wake(); } break;
      case 'gui': if (gui) { gui.event(m); wake(); } break;
      case 'pause':
        if (vm && !paused) { paused = true; parkedAt = performance.now(); parked = true; postStatus(true); }
        break;
      case 'resume':
        if (vm && paused) { paused = false; postStatus(true); wake(); }
        break;
      case 'peek': {
        const a = m.addr >>> 0, n = Math.max(0, m.len | 0);
        const bytes = vm ? vm.u8.slice(a, a + n) : new Uint8Array(0);
        postMessage({ type: 'reply', id: m.id, bytes }, [bytes.buffer]);
        break;
      }
      case 'poke':
        if (vm) vm.u8.set(m.bytes, m.addr >>> 0);
        postMessage({ type: 'reply', id: m.id, ok: !!vm });
        break;
    }
  } catch (err) {
    postMessage({ type: 'error', message: `libjs: ${err.message}` });
  }
};
