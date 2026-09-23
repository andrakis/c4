// worker.js - the machine, in a Web Worker.
//
// The page never touches the interpreter. It sends messages (boot, keys,
// signals, display events) and gets messages back (console bytes,
// status, draw commands). postMessage only, so this works from any
// static server; nothing here needs cross-origin isolation.
//
// Page -> worker
//   boot    { image: ArrayBuffer, argv, disk: [[name, ArrayBuffer]], arenaMb, mhz, unpaced, gui, shared }
//           shared: { ring, fb } SharedArrayBuffers from shared.js, when the page is isolated
//   key     { key, ctrlKey, altKey, metaKey }     a keydown on the terminal
//   input   { bytes: Uint8Array }                 typed programmatically
//   signal  { sig }                               2 = Ctrl-C
//   eof     {}                                    Ctrl-D
//   gui     { kind, ... }                         a display event (gui-device.js)
//   pause / resume / reset
//   peek    { id, addr, len }                     read guest memory
//   poke    { id, addr, bytes }                   write guest memory
// Worker -> page
//   out     { bytes }         console output, and the echo of what was typed
//   kbd     { raw }           a program opened or closed /dev/tty
//   display { frames }        draw commands (gui-device.js); not used on the shared path
//   status  { ... }           about four times a second
//   exit    { status, cycles }
//   reply   { id, ... }       for peek and poke
//   error   { message }

import { createMachine, boot } from './boot.js';
import { STOP } from './c4m.js';
import { keyDown, typeBytes } from './keys.js';
import { GuiDevice } from './gui-device.js';
import { RingWriter, FbProducer } from './shared.js';

let vm = null, bootMsg = null, gui = null, ring = null, ringStalls = 0, handoffMs = 0;
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

function start(msg) {
  bootMsg = msg;
  ring = msg.gui && msg.shared ? new RingWriter(msg.shared.ring) : null;
  if (ring && fbSab !== msg.shared.fb) { fbSab = msg.shared.fb; fbProducer = new FbProducer(fbSab); }
  gui = msg.gui ? new GuiDevice({ ...msg.gui, fbShared: ring ? fbProducer : null }) : null;
  ringStalls = 0;
  // Files arrive as Uint8Arrays or bare ArrayBuffers; the disk
  // controller wants bytes it can index.
  const files = new Map((msg.disk || []).map(([n, b]) => [n, b instanceof Uint8Array ? b : new Uint8Array(b)]));
  const drives = [{ files, writable: false, sink: null }];
  vm = createMachine({
    arenaMb: msg.arenaMb || 64, drives, mhz: msg.mhz || 20, gui,
    onByte: b => { out.push(b); if (out.length >= 4096) flush(); },
  });
  boot(vm, new Uint8Array(msg.image), msg.argv || ['image.c4r'], { mbox: msg.mbox | 0 });
  paused = false; halted = false; parked = false;
  tRun = performance.now(); lostMs = 0; busyMs = 0;
  lastStatus = { t: tRun, cycle: 0, busy: 0 };
  lastRaw = 0;
  state = 'running';
  schedule(0);
}

function tick() {
  if (!vm || paused || halted) return;
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
  const td = performance.now();
  flushDisplay();
  handoffMs += performance.now() - td;
  if (dev.rawKbd !== lastRaw) { lastRaw = dev.rawKbd; postMessage({ type: 'kbd', raw: lastRaw > 0 }); }

  if (r === STOP.HALT) {
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
    if (ahead > 2) { schedule(Math.min(ahead, 100)); return; }
    if (ahead < -1000) lostMs += -ahead - 1000;   // hopelessly behind: stop trying to catch up
  }
  schedule(0);
}

function wake() {
  if (!vm || paused || halted) return;
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
    gui: gui ? { ...gui.stats(), shared: !!ring, ringStalls, handoffMs: Math.round(handoffMs) } : null,
  });
  lastStatus = { t: now, cycle: ran, busy: busyMs };
}
setInterval(() => postStatus(false), 250);

function echo(bytes) { for (const b of bytes) out.push(b); flush(); }

self.onmessage = e => {
  const m = e.data;
  try {
    switch (m.type) {
      case 'boot': start(m); break;
      case 'reset': if (bootMsg) start(bootMsg); break;
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
