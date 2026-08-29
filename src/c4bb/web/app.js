// app.js - the interactive c4bb machine in the browser.
//
// Wires the simulator core (../sim) to the board renderer and panels.
// Execution modes:
//   uStep - one microstep, every control line visible
//   Step  - one whole instruction
//   Run   - continuous animation at the speed slider's rate
//   Turbo - the compiled engine, board in heat mode (~millions/s)

import { Arena, CONS_RET } from '../sim/arena.js';
import { Devices } from '../sim/devices.js';
import { Machine, R } from '../sim/machine.js';
import { assemble, FETCH } from '../sim/ucode.js';
import { parseBoard } from '../sim/netlist.js';
import { boot, callFunction } from '../sim/loader.js';
import { Turbo } from '../sim/turbo.js';
import { BoardRenderer } from './board.js';
import { RegsPanel, UcodePanel, Terminal } from './panels.js';
import { DriveSet, DrivePanel, EMPTY, humanBytes } from './drives.js';
import { MediaStore, indexedDbBackend } from './store.js';

const $ = id => document.getElementById(id);

// Most images print and exit; they never call read(), so typing into
// the terminal does nothing for them - that's not a bug, there's just
// no one listening. Only the two interactive OS images read a
// keyboard, so they're flagged and labeled for it.
// BIOS is first because it is the machine, and everything after it is
// a shortcut past it. Choosing it hands the firmware the drives and
// lets it find something to boot -- which is what makes the drive panel
// a control rather than a display, and what makes the climb (install a
// medium, eject, restart) possible here and not only in the CLI.
const BIOS = '(BIOS)';
const PROGRAMS = [
  BIOS,
  'c4ix32', 'c4ke32', 'c4dos32', 'hello32', 'factorial', 'test_basic', 'mandel',
  'tests', 'test_malloc', 'test_printloop', 'cycles', 'test_float',
  'bb_customop', 'bb_preempt', 'bb_pm',
];
const INTERACTIVE = new Set([BIOS, 'c4ix32', 'c4ke32', 'c4dos32']);

// Which disk directory an image boots against. `disk` is the shared
// one where all three systems live together; the derived disks
// (build-images.sh) are curated per system, which is what you want the
// moment you are BUILDING on one rather than demonstrating it. Anything
// not named here gets the shared disk.
const DISKS = {
  c4dos32: 'dos-recovery',   // DOS, a compiler, and the kernel sources
  [BIOS]:  'climb',          // the whole ladder: LADDER, install, b4ke
};
const DEFAULT_DISK = 'disk';

// The media that ship with the machine. Read-only, fetched the first
// time one is put in a drive rather than at startup -- the climb disk
// alone is four megabytes, and most sessions never look at it.
const ROM_DISKS = ['climb', 'dos-recovery', 'disk', 'c4ke-root'];
const ROM_LABEL = {
  climb:          'climb disk (C4DOS + the whole ladder)',
  'dos-recovery': 'recovery disk (C4DOS + sources)',
  disk:           'shared disk (all three systems)',
  'c4ke-root':    'C4KE root (its full userland)',
};
const romId = dir => `rom:${dir}`;

// The firmware, in the stages the player builds it in
// (docs/c4bb-storage.md M6). Each is the same source with a piece
// missing, and the piece really is missing -- which is what makes
// "you cannot boot that yet" a fact about the machine rather than a
// line in a design document.
const FIRMWARES = [
  ['fw-hello.c4r',  'fw: banner only'],
  ['fw-ram.c4r',    'fw: + memory probe'],
  ['fw-drives.c4r', 'fw: + drive probe'],
  ['fw.c4r',        'fw: the BIOS'],
];
const DEFAULT_FW = 'fw.c4r';

let ucSource, boardDef, fwBytes, ucode;
let machine, turbo, progImg, renderer;
let store, driveSet, drivePanel;
let lastEvent = null;
let mode = 'pause';          // pause | run | turbo
let finished = false;

const terminal = new Terminal($('terminal'));
const regsPanel = new RegsPanel($('regs'));
let ucodePanel;

async function fetchBin(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${url}: ${r.status}`);
  return new Uint8Array(await r.arrayBuffer());
}

async function init() {
  ucSource = await (await fetch('../hw/microcode.uc')).text();
  boardDef = parseBoard(await (await fetch('../hw/board.hwd')).text());
  fwBytes = await fetchBin(`../fw/${DEFAULT_FW}`);
  ucode = assemble(ucSource);
  renderer = new BoardRenderer($('board'), boardDef);
  ucodePanel = new UcodePanel($('ucode'), $('ucode-title'), ucode);

  const sel = $('program');
  for (const p of PROGRAMS) {
    const o = document.createElement('option');
    o.value = p;
    o.textContent = p === BIOS ? '(BIOS) - boot from a drive'
                  : INTERACTIVE.has(p) ? `${p} (interactive)` : p;
    sel.appendChild(o);
  }
  sel.onchange = () => reset();

  const fws = $('firmware');
  for (const [file, label] of FIRMWARES) {
    const o = document.createElement('option');
    o.value = file;
    o.textContent = label;
    fws.appendChild(o);
  }
  fws.value = DEFAULT_FW;
  fws.onchange = () => reset();

  await initMedia();
  await reset();
  requestAnimationFrame(frame);
}

// ---- media and drives -----------------------------------------------

async function initMedia() {
  driveSet = new DriveSet(3);
  for (const dir of ROM_DISKS)
    driveSet.define({ id: romId(dir), label: ROM_LABEL[dir] || dir, files: new Map(), sink: null, dir });

  // Whatever the player made last time. Loaded in full, because a
  // medium they made is a medium they are about to use, and the
  // alternative is a drive that reports 0 files until you touch it.
  store = new MediaStore(indexedDbBackend());
  try {
    for (const m of await store.list())
      driveSet.define({ id: m.id, label: m.label, files: await store.load(m.id), sink: store.sinkFor(m.id) });
  } catch (e) {
    // Private mode, or storage refused. The machine still runs; it
    // just cannot keep anything, and saying so is better than a drive
    // panel that silently loses a boot disk.
    setStatus('no persistent storage in this browser - media will not survive a reload');
  }

  drivePanel = new DrivePanel($('drives'), driveSet, { onchange: () => drivesChanged() });
  $('newmedium').onclick = () => newMedium();
  $('erasemedium').onclick = () => eraseMedium();
  $('dropmedium').onclick = () => dropMedium();
}

// A read-only medium's files are fetched the first time it is actually
// put in a drive.
async function ensureLoaded() {
  await Promise.all([...driveSet.media.values()].map(async m => {
    if (!m.dir || m.loaded) return;
    if (!driveSet.slots.includes(m.id)) return;
    m.files = await loadDisk(m.dir);
    m.loaded = true;
  }));
}

// A disk swapped while the machine is running is a disk swapped while
// the machine is running: hand it the new list and let it find out on
// its next open(), which is what a real drive does.
async function drivesChanged() {
  await ensureLoaded();
  if (machine) machine.dev.drives = driveSet.toDevices();
  renderDrives();
}

function userMedia() { return [...driveSet.media.values()].filter(m => !m.dir); }

function renderDrives() {
  drivePanel.render(machine ? machine.dev.drive : -1);
  const sel = $('usermedium');
  const was = sel.value;
  sel.textContent = '';
  for (const m of userMedia()) {
    const o = document.createElement('option');
    o.value = m.id;
    o.textContent = `${m.label} (${m.files.size} files, ${humanBytes([...m.files.values()].reduce((a, b) => a + b.length, 0))})`;
    sel.appendChild(o);
  }
  if ([...sel.options].some(o => o.value === was)) sel.value = was;
  const none = userMedia().length === 0;
  $('erasemedium').disabled = none;
  $('dropmedium').disabled = none;
  $('drive-note').textContent = none
    ? 'New makes a blank writable medium. It is kept in this browser and survives a reload.'
    : 'Media you make are kept in this browser. Pick (BIOS) above to boot from a drive rather than past it.';
}

async function newMedium() {
  if (!store) return;
  const n = userMedia().length + 1;
  const m = await store.create(`medium ${n}`);
  driveSet.define({ id: m.id, label: m.label, files: new Map(), sink: store.sinkFor(m.id) });
  // Into the first free drive, because making one and then having to
  // put it somewhere is two steps where the player meant one.
  const free = driveSet.slots.indexOf(EMPTY);
  if (free >= 0) driveSet.insert(free, m.id);
  await drivesChanged();
}

async function eraseMedium() {
  const id = $('usermedium').value;
  const m = driveSet.media.get(id);
  if (!m || !store) return;
  if (!confirm(`Erase "${m.label}"? Every file on it goes.`)) return;
  await store.erase(id);
  m.files = new Map();
  await drivesChanged();
}

async function dropMedium() {
  const id = $('usermedium').value;
  const m = driveSet.media.get(id);
  if (!m || !store) return;
  if (!confirm(`Delete "${m.label}" for good?`)) return;
  await store.remove(id);
  driveSet.forget(id);
  await drivesChanged();
}

// One cache entry per disk directory: switching programs must not
// re-fetch a disk already in memory, and must not hand one program the
// other's files.
const diskCache = new Map();
async function loadDisk(dir) {
  if (diskCache.has(dir)) return diskCache.get(dir);
  const files = new Map();
  diskCache.set(dir, files);
  try {
    const names = await (await fetch(`../images/${dir}/manifest.json`)).json();
    await Promise.all(names.map(async n =>
      files.set(n, await fetchBin(`../images/${dir}/${n}`))));
  } catch { /* no disk built; programs that open files will get -1 */ }
  return files;
}

// Build the machine. `drives` is the drive list to hand it -- on a
// player-initiated reset that is whatever the panel says, and on a
// soft reset it is what survived the eject.
async function build(prog, drives) {
  const interactive = INTERACTIVE.has(prog);
  // (BIOS) has no image of its own: the firmware runs, probes the
  // drives and boots the first one with something on it, exactly as
  // the CLI does when handed no program.
  const progBytes = prog === BIOS ? null : await fetchBin(`../images/${prog}.c4r`);
  // 128 MB, not 32. The recovery disk can now build C4IX as well as
  // C4KE, and C4DOS does not return a transient's memory when it exits
  // -- the image and whatever it allocated stay held -- so twelve
  // compiler runs in one session accumulate. 128 MB is what the whole
  // climb needs with c4sc at a 200,000-cell arena, measured; the
  // largest single module fits in 48.
  const arena = new Arena(128 * 1024 * 1024);
  const dev = new Devices(arena, {
    drives,
    onByte: b => terminal.write(b),
    // A guest that takes a disk out has just made the panel wrong.
    onEject: d => { driveSet.ejectedByGuest(d); renderDrives(); },
    onRescan: () => renderDrives(),
  });
  machine = new Machine(arena, ucode, dev, { onLog: s => terminal.writeString(s) });
  machine.onEvent = e => { lastEvent = e; };
  $('terminal').dataset.placeholder = interactive
    ? 'click here, then type - this OS reads a real keyboard'
    : `${prog}.c4r just prints and exits - it never reads input, so there's nothing to type into`;
  updateKbdStatus(interactive);
  ({ progImg } = boot(machine, fwBytes, progBytes, progBytes ? [prog + '.c4r'] : []));
  turbo = new Turbo(machine);
  lastEvent = null;
  finished = false;
  renderDrives();
}

async function reset() {
  const prog = $('program').value || PROGRAMS[0];
  fwBytes = await fetchBin(`../fw/${$('firmware').value || DEFAULT_FW}`);
  // Choosing a program is choosing a machine, and a machine comes with
  // the medium that program expects in drive 0. Everything after that
  // is the player's to move.
  const want = romId(DISKS[prog] || DEFAULT_DISK);
  if (driveSet.media.has(want)) driveSet.insert(0, want);
  await ensureLoaded();

  terminal.clear();
  await build(prog, driveSet.toDevices());
  mode = 'pause';
  setStatus(prog === BIOS ? 'BIOS - press Turbo to boot from a drive'
                          : `${prog}.c4r loaded - ready`);
  draw();
}

// The machine asking to start again (`RUN reboot.c4r 0`). The arena is
// zeroed, the media come back MINUS anything that was ejected, and the
// BIOS runs rather than whatever image was named at the start -- the
// disk that was just written is the one that should come up. cli.js
// does exactly this; this is the browser half of M10's soft reset.
async function softReset() {
  machine.dev.resetRequested = false;
  terminal.writeString('\nc4bb: soft reset\n\n');
  await build(BIOS, driveSet.survivesReset());
  setStatus('soft reset - BIOS');
  draw();
}

function updateKbdStatus(interactive) {
  const el = $('kbd-status');
  const focused = document.activeElement === $('terminal');
  el.classList.toggle('listening', focused);
  el.classList.toggle('inert', !interactive);
  el.textContent = !interactive
    ? 'this program does not read keyboard input'
    : focused ? 'listening - type now' : 'click the terminal below to type';
}

function setStatus(s) { $('status').textContent = s; }

// A reset is not a halt: the machine wants to be rebuilt, not stopped.
// Handled before the halt check because a reset sets both.
let resetting = false;
function checkReset() {
  if (resetting || !machine.dev.resetRequested) return false;
  resetting = true;
  const wasMode = mode;
  mode = 'pause';
  softReset().then(() => { resetting = false; mode = wasMode; });
  return true;
}

function checkDone() {
  if (finished) return true;
  if (checkReset()) return true;
  if (machine.dev.halted) {
    finished = true;
    mode = 'pause';
    setStatus(`exit(${machine.dev.status}) after ${machine.cycle} cycles`);
    return true;
  }
  if (machine.upc === FETCH && machine.regs[R.PC] === CONS_RET) {
    finished = true;
    mode = 'pause';
    const status = machine.regs[R.A];
    machine.regs[R.SP] = (machine.regs[R.SP] + 8) | 0;
    for (const d of progImg.des) callFunction(machine, d, []);
    setStatus(`main returned ${status} after ${machine.cycle} cycles`);
    return true;
  }
  return false;
}

function uStep() { if (!checkDone()) { machine.step(); checkDone(); draw(); } }

function iStep() {
  if (checkDone()) return;
  machine.step();
  while (machine.upc !== FETCH && !machine.dev.halted) machine.step();
  checkDone();
  draw();
}

// speed slider: 0..100 -> ~1..80000 microsteps per frame (log scale)
function stepsPerFrame() {
  return Math.max(1, Math.round(Math.pow(10, 0.05 * $('speed').value)));
}

function frame() {
  if (mode === 'run') {
    const n = stepsPerFrame();
    for (let i = 0; i < n; i++) {
      if (checkDone()) break;
      machine.step();
    }
    checkDone();
    draw();
  } else if (mode === 'turbo') {
    if (!checkDone()) {
      turbo.run(300000, CONS_RET);
      checkDone();
    }
    drawHeat();
  }
  requestAnimationFrame(frame);
}

function draw() {
  const step = lastEvent ? lastEvent.step : null;
  renderer.render(machine, { step, mode: 'step', busValue: lastEvent ? lastEvent.bus : 0 });
  regsPanel.update(machine);
  ucodePanel.update(machine, lastEvent);
  if (!finished && mode === 'run')
    setStatus(`cycle ${machine.cycle} - ${stepsPerFrame()} µsteps/frame`);
}

function drawHeat() {
  renderer.render(machine, { mode: 'heat' });
  regsPanel.update(machine);
  if (!finished) setStatus(`TURBO - cycle ${machine.cycle}`);
}

function setMode(m, btn) {
  mode = m;
  for (const b of ['run', 'turbo', 'pause']) $(b).classList.toggle('active', b === btn);
}

$('reset').onclick = () => reset();
$('ustep').onclick = () => { setMode('pause', 'pause'); uStep(); };
$('istep').onclick = () => { setMode('pause', 'pause'); iStep(); };
$('run').onclick = () => setMode('run', 'run');
$('turbo').onclick = () => {
  // the compiled engine starts at an instruction boundary: finish any
  // half-executed instruction on the step engine first
  while (machine.upc !== FETCH && !machine.dev.halted) machine.step();
  setMode('turbo', 'turbo');
};
$('pause').onclick = () => { setMode('pause', 'pause'); draw(); };

// Keyboard input feeds the keyboard device, in one of two modes.
//
// COOKED, which is what a shell wants. Neither kernel's shell ever
// disables terminal echo (c4sh.c's own char-reader has its putchar(c)
// commented out - it was written expecting the host tty to echo,
// exactly like a real unmodified Linux terminal in canonical mode
// always does regardless of which fd a program reads from). So the
// host - us - echoes and does local backspace line editing.
//
// RAW, which is what a full-screen program wants, and which is exactly
// what dev.rawKbd has been counting all along: it goes up when a
// program opens /dev/tty (devices.js:112) and down when it closes it.
// A program that has done that owns the screen, so we must NOT echo -
// a local echo lands wherever the cursor happens to be and corrupts
// whatever it was painting - and we must send the keys it cannot
// otherwise get. Arrows, function keys, Home/End and Alt-letter are
// how a DOS-style UI is driven, and e.key for all of them is a WORD,
// so the old length === 1 test dropped every one.
const KEYSEQ = {
  ArrowUp: '\x1b[A', ArrowDown: '\x1b[B', ArrowRight: '\x1b[C', ArrowLeft: '\x1b[D',
  Home: '\x1b[H', End: '\x1b[F', PageUp: '\x1b[5~', PageDown: '\x1b[6~',
  Insert: '\x1b[2~', Delete: '\x1b[3~',
  F1: '\x1bOP', F2: '\x1bOQ', F3: '\x1bOR', F4: '\x1bOS',
  F5: '\x1b[15~', F6: '\x1b[17~', F7: '\x1b[18~', F8: '\x1b[19~',
  F9: '\x1b[20~', F10: '\x1b[21~', F11: '\x1b[23~', F12: '\x1b[24~',
  Tab: '\t', Escape: '\x1b', Enter: '\r', Backspace: '\x7f',
};

function sendKeys(dev, s) { for (let i = 0; i < s.length; ++i) dev.rxFifo.push(s.charCodeAt(i)); }
$('terminal').addEventListener('focus', () => updateKbdStatus(INTERACTIVE.has($('program').value)));
$('terminal').addEventListener('blur', () => updateKbdStatus(INTERACTIVE.has($('program').value)));
$('terminal').addEventListener('keydown', e => {
  const dev = machine.dev;
  if (e.ctrlKey && e.key === 'c') {
    machine.pendingSignal = 2;                        // SIGINT
  } else if (e.ctrlKey && e.key === 'd') {
    dev.rxEof = true;
  } else if (dev.rawKbd > 0) {
    // Raw: send, never echo. The program is painting.
    if (e.altKey && e.key.length === 1) {
      sendKeys(dev, '\x1b' + e.key);                  // ESC-prefixed: Alt-F
    } else if (KEYSEQ[e.key] !== undefined) {
      sendKeys(dev, KEYSEQ[e.key]);
    } else if (e.ctrlKey && e.key.length === 1) {
      const c = e.key.toLowerCase().charCodeAt(0);
      if (c >= 97 && c <= 122) dev.rxFifo.push(c - 96);   // Ctrl-A is 1
      else return;
    } else if (e.key.length === 1 && !e.metaKey) {
      dev.rxFifo.push(e.key.charCodeAt(0));
    } else return;
  } else if (e.key === 'Enter') {
    dev.rxFifo.push(10);
    terminal.write(10);
  } else if (e.key === 'Backspace') {
    if (dev.rxFifo.length && dev.rxFifo[dev.rxFifo.length - 1] !== 10) {
      dev.rxFifo.pop();
      terminal.backspace();
    }
  } else if (e.key.length === 1 && !e.ctrlKey && !e.metaKey) {
    dev.rxFifo.push(e.key.charCodeAt(0));
    terminal.write(e.key.charCodeAt(0));
  } else return;
  e.preventDefault();
});

init().catch(e => setStatus('ERROR: ' + e.message));
