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

const $ = id => document.getElementById(id);

// Most images print and exit; they never call read(), so typing into
// the terminal does nothing for them - that's not a bug, there's just
// no one listening. Only the two interactive OS images read a
// keyboard, so they're flagged and labeled for it.
const PROGRAMS = [
  'c4ix32', 'c4ke32', 'c4dos32', 'hello32', 'factorial', 'test_basic', 'mandel',
  'tests', 'test_malloc', 'test_printloop', 'cycles', 'test_float',
  'bb_customop', 'bb_preempt', 'bb_pm',
];
const INTERACTIVE = new Set(['c4ix32', 'c4ke32', 'c4dos32']);

// Which disk directory an image boots against. `disk` is the shared
// one where all three systems live together; the derived disks
// (build-images.sh) are curated per system, which is what you want the
// moment you are BUILDING on one rather than demonstrating it. Anything
// not named here gets the shared disk.
const DISKS = {
  c4dos32: 'dos-recovery',   // DOS, a compiler, and the kernel sources
};
const DEFAULT_DISK = 'disk';

let ucSource, boardDef, fwBytes, ucode;
let machine, turbo, progImg, renderer;
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
  fwBytes = await fetchBin('../fw/fw.c4r');
  ucode = assemble(ucSource);
  renderer = new BoardRenderer($('board'), boardDef);
  ucodePanel = new UcodePanel($('ucode'), $('ucode-title'), ucode);

  const sel = $('program');
  for (const p of PROGRAMS) {
    const o = document.createElement('option');
    o.value = p;
    o.textContent = INTERACTIVE.has(p) ? `${p} (interactive)` : p;
    sel.appendChild(o);
  }
  sel.onchange = () => reset();
  await reset();
  requestAnimationFrame(frame);
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

async function reset() {
  const prog = $('program').value || PROGRAMS[0];
  const interactive = INTERACTIVE.has(prog);
  const progBytes = await fetchBin(`../images/${prog}.c4r`);
  const arena = new Arena(32 * 1024 * 1024);
  const dev = new Devices(arena, {
    files: await loadDisk(DISKS[prog] || DEFAULT_DISK),
    onByte: b => terminal.write(b),
  });
  machine = new Machine(arena, ucode, dev, { onLog: s => terminal.writeString(s) });
  machine.onEvent = e => { lastEvent = e; };
  terminal.clear();
  $('terminal').dataset.placeholder = interactive
    ? 'click here, then type - this OS reads a real keyboard'
    : `${prog}.c4r just prints and exits - it never reads input, so there's nothing to type into`;
  updateKbdStatus(interactive);
  ({ progImg } = boot(machine, fwBytes, progBytes, [prog + '.c4r']));
  turbo = new Turbo(machine);
  lastEvent = null;
  finished = false;
  mode = 'pause';
  setStatus(`${prog}.c4r loaded - ready`);
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

function checkDone() {
  if (finished) return true;
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
