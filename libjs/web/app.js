// app.js - the c4m.js page: pick a system, boot it, wire the panes.
//
// URL parameters:
//   ?system=NAME                 which machine to boot (default c4ix): c4ix, c4ke,
//                                c4dos, wolf3d, wolf3d-c4ix, doom, tyrian, rott, gui-demo, fb-demo,
//                                snd-demo, raycast -- SYSTEMS below
//   ?image=URL&disk=URL          any other image and disk instead
//   ?args=A+B                    more arguments for the system's program, after its
//                                own: ?system=wolf3d&args=--secrets+--nodeath
//   ?mhz=N                       how fast the machine runs, paced (default 100)
//   ?snd=0                       no sound card
//   ?cpus=N                      N processors, simulated (smp.js, docs/c4ix-smp.md)
//   ?parallel                    ... each on a worker of its own (par.js), when the
//                                page is cross-origin isolated (serve.mjs --isolate)
//   ?persist=0 | reset           C4IX's files are not kept between visits / are forgotten first
//   ?fast                        run as fast as the host can, unpaced
//   ?realtime=0                  when the host cannot keep up, the machine's clock
//                                slows with it, rather than keeping the wall's
//                                (worker.js: a game then plays in slow motion)
//   ?fpu=0                       no FPU opcode: floats in software (F3, docs/c4-float.md)
//   ?mem=MB                      arena size (default 64)
//   ?shared=0                    use the message display path even when the page
//                                is cross-origin isolated (serve.mjs --isolate)
//
// The Configuration button shows them all as a form (CONFIG below).
//
// The running machine is window.c4m (page.js's C4M), so the console or a
// test can drive it: c4m.write('ps\n'), c4m.status, await c4m.peek(a, n).

import { C4M } from '../page.js';

const IMAGES = '../../src/c4bb/images';
const SYSTEMS = {
  c4ix: { label: 'C4IX', image: `${IMAGES}/c4ix32.c4r`, disk: `${IMAGES}/disk`, argv: ['c4ix32.c4r', '--desktop'], persist: 'c4ix' },
  c4ke: { label: 'C4KE', image: `${IMAGES}/c4ke32.c4r`, disk: `${IMAGES}/disk`, argv: ['c4ke32.c4r'] },
  // C4DOS on the games' disk (build-images.sh): Wolfenstein 3D unpacked
  // Doom, Tyrian and Rise of the Triad, beside C4DOS's own files, so WOLF3D, DOOM, TYRIAN or ROTT at the A> prompt
  // plays them.
  c4dos: { label: 'C4DOS', image: `${IMAGES}/c4dos32.c4r`, disk: `${IMAGES}/games`, argv: ['c4dos32.c4r'], mhz: 400 },
  // Wolfenstein 3D with no operating system under it (games/wolf3d/plat-bare.c),
  // and on C4IX drawing on the display itself rather than in a window
  // (wolf3d --direct, like raycast -G). The game times itself against the
  // machine's clock, so these are paced, at a clock it can keep up with.
  wolf3d: { label: 'Wolfenstein 3D (bare machine)', image: `${IMAGES}/games/wolf3d.c4r`, disk: `${IMAGES}/games`,
            argv: ['wolf3d.c4r'], mhz: 400 },
  doom: { label: 'Doom (bare machine)', image: `${IMAGES}/games/doom.c4r`, disk: `${IMAGES}/games`,
          argv: ['doom.c4r'], mhz: 400 },
  // OpenTyrian (games/tyrian), where Tyrian's data was there to put on
  // the disk (games/tyrian/data/README.md)
  tyrian: { label: 'Tyrian (bare machine)', image: `${IMAGES}/games/tyrian.c4r`, disk: `${IMAGES}/games`,
            argv: ['tyrian.c4r'], mhz: 400 },
  // Rise of the Triad (games/rott), the shareware episode
  rott: { label: 'Rise of the Triad (bare machine)', image: `${IMAGES}/games/rott.c4r`, disk: `${IMAGES}/games`,
          argv: ['rott.c4r'], mhz: 400 },
  'wolf3d-c4ix': { label: 'Wolfenstein 3D (C4IX, direct)', image: `${IMAGES}/c4ix32.c4r`, disk: `${IMAGES}/disk`,
                   argv: ['c4ix32.c4r', '-e', 'persist -d 60 &', '-e', 'wolf3d --direct'], persist: 'c4ix', mhz: 400 },
  'gui-demo': { label: 'GUI demo (bare machine)', image: `${IMAGES}/gui-demo.c4r`, disk: null, argv: ['gui-demo.c4r'] },
  // Computes every pixel itself, so it claims a faster clock than c4bb's 20 MHz.
  'fb-demo': { label: 'Framebuffer demo (bare machine)', image: `${IMAGES}/fb-demo.c4r`, disk: null, argv: ['fb-demo.c4r'], mhz: 100 },
  'snd-demo': { label: 'Sound card demo (bare machine)', image: `${IMAGES}/snd-demo.c4r`, disk: null, argv: ['snd-demo.c4r'] },
  // The raycaster drawing 640x480 on the display, as fast as the host can:
  // unpaced, so the frame rate is the machine's and the display path's.
  raycast: { label: 'Raycast 640x480 (bare machine)', image: `${IMAGES}/disk/raycast.c4r`, disk: null,
             argv: ['raycast.c4r', '31x31', '-G', '640x480'], unpaced: true },
};

const $ = id => document.getElementById(id);
const params = new URLSearchParams(location.search);
const which = params.get('system') || 'c4ix';

const sel = $('system');
for (const [k, s] of Object.entries(SYSTEMS)) sel.add(new Option(s.label, k, false, k === which));
if (params.get('image')) sel.add(new Option(params.get('image').split('/').pop(), 'custom', true, true));
sel.addEventListener('change', () => {
  const p = new URLSearchParams(location.search);
  p.delete('image'); p.delete('disk');
  p.set('system', sel.value);
  location.search = p.toString();
});

const sys = params.get('image')
  ? { image: params.get('image'), disk: params.get('disk'), argv: [params.get('image').split('/').pop()] }
  : SYSTEMS[which] || SYSTEMS.c4ix;

const status = $('status');
const kbd = $('kbd');
const term = $('terminal');
const canvas = $('display');
const info = $('display-info');

function fmtRate(ips) {
  return ips >= 1e6 ? (ips / 1e6).toFixed(1) + 'M' : ips >= 1e3 ? (ips / 1e3).toFixed(0) + 'k' : String(ips);
}

const extraArgs = (params.get('args') || '').split(/\s+/).filter(Boolean);
const opts = {
  ...sys,
  argv: sys.argv.concat(extraArgs),
  terminal: term,
  display: canvas,
  // 100 MHz: the interpreter manages well over that on a desktop, an idle
  // machine sleeps whatever the figure, and at c4bb's 20 a full-screen
  // program in the C4IX desktop managed a few frames a second.
  mhz: Number(params.get('mhz')) || sys.mhz || 100,
  unpaced: params.has('fast') || !!sys.unpaced,
  realtime: params.get('realtime') !== '0',
  fpu: params.get('fpu') !== '0',
  arenaMb: Number(params.get('mem')) || 64,
  shared: params.get('shared') !== '0',
  snd: params.get('snd') !== '0',
  cpus: Number(params.get('cpus')) || 0,
  parallel: params.has('parallel'),
  persist: params.get('persist') === '0' ? null : sys.persist || null,
  persistReset: params.get('persist') === 'reset',
};
// Configuration: the URL's parameters (above) as a form. OK writes the ones
// that differ from their defaults into the URL, keeps any it does not know,
// and so boots the machine again with them. A text box takes any value and
// suggests the usual ones.
const CONFIG = [
  { key: 'system', label: 'System', kind: 'select', def: 'c4ix',
    options: Object.entries(SYSTEMS).map(([k, s]) => [k, s.label]) },
  { key: 'mhz', label: 'Speed (MHz)', kind: 'text', options: ['20', '50', '100', '200', '400', '800'],
    help: `paced to this clock; empty is the system's own (${sys.mhz || 100})` },
  { key: 'cpus', label: 'Processors', kind: 'text', options: ['1', '2', '4', '8'],
    help: 'empty or 1 is one; more are simulated, taking turns' },
  { key: 'parallel', label: 'A thread each', kind: 'flag',
    help: 'several processors at once, each on a worker (needs the page cross-origin isolated)' },
  { key: 'mem', label: 'Memory (MB)', kind: 'text', options: ['32', '64', '128', '256'],
    help: 'the arena; empty is 64' },
  { key: 'snd', label: 'Sound card', kind: 'onoff', def: true },
  { key: 'fpu', label: 'FPU', kind: 'onoff', def: true, help: 'off: floats in software' },
  { key: 'realtime', label: 'Keep the wall clock', kind: 'onoff', def: true,
    help: 'off: when the host cannot keep up, the machine slows down instead of dropping frames' },
  { key: 'fast', label: 'Unpaced', kind: 'flag', help: 'as fast as the host can, whatever the MHz' },
  { key: 'shared', label: 'Shared display path', kind: 'onoff', def: true,
    help: 'off: the message path even when the page is cross-origin isolated' },
  { key: 'persist', label: "C4IX's files", kind: 'select', def: '',
    options: [['', 'kept between visits'], ['0', 'not kept'], ['reset', 'forgotten, then kept']] },
  { key: 'args', label: 'Arguments', kind: 'text', options: [],
    help: "more for the system's program, after its own (wolf3d: --secrets --nodeath)" },
  { key: 'image', label: 'Image URL', kind: 'text', options: [], help: 'any other image instead of the system' },
  { key: 'disk', label: 'Disk URL', kind: 'text', options: [], help: 'its disk (a directory with an index.json)' },
];

const cfgDialog = $('config');
const cfgFields = $('config-fields');
const cfgInputs = {};
for (const c of CONFIG) {
  const id = `cfg-${c.key}`;
  const label = document.createElement('label');
  label.htmlFor = id;
  label.textContent = c.label;
  let input;
  if (c.kind === 'select') {
    input = document.createElement('select');
    for (const [v, t] of c.options) input.add(new Option(t, v));
  } else if (c.kind === 'flag' || c.kind === 'onoff') {
    input = document.createElement('input');
    input.type = 'checkbox';
  } else {
    input = document.createElement('input');
    input.type = 'text';
    input.autocomplete = 'off';
    input.spellcheck = false;
    if (c.options.length) {
      const list = document.createElement('datalist');
      list.id = `${id}-list`;
      for (const v of c.options) list.append(new Option(v));
      cfgFields.append(list);
      input.setAttribute('list', list.id);
    }
  }
  input.id = id;
  cfgInputs[c.key] = input;
  cfgFields.append(label, input);
  if (c.help) {
    const help = document.createElement('div');
    help.className = 'help';
    help.textContent = c.help;
    cfgFields.append(help);
  }
}

// the form from a URL's parameters
function cfgLoad(p) {
  for (const c of CONFIG) {
    const input = cfgInputs[c.key];
    if (c.kind === 'flag') input.checked = p.has(c.key);
    else if (c.kind === 'onoff') input.checked = p.get(c.key) !== '0';
    else if (c.kind === 'select') input.value = p.get(c.key) ?? c.def;
    else input.value = p.get(c.key) ?? '';
  }
  if (p.get('image')) cfgInputs.system.value = which;
}

// the URL's parameters from the form: the defaults left out, others kept
function cfgParams() {
  const p = new URLSearchParams(location.search);
  for (const c of CONFIG) {
    const input = cfgInputs[c.key];
    p.delete(c.key);
    if (c.kind === 'flag') { if (input.checked) p.set(c.key, ''); }
    else if (c.kind === 'onoff') { if (!input.checked) p.set(c.key, '0'); }
    else if (c.kind === 'select') { if (input.value !== c.def) p.set(c.key, input.value); }
    else if (input.value.trim() !== '') p.set(c.key, input.value.trim());
  }
  // a flag is there or not: ?fast, not ?fast=
  return p.toString().replace(/=(&|$)/g, '$1');
}

$('configure').addEventListener('click', () => {
  cfgLoad(new URLSearchParams(location.search));
  cfgDialog.showModal();
});
$('config-cancel').addEventListener('click', () => cfgDialog.close());
$('config-defaults').addEventListener('click', () => cfgLoad(new URLSearchParams()));
$('config-form').addEventListener('submit', e => {
  e.preventDefault();
  const q = cfgParams();
  cfgDialog.close();
  if (q !== location.search.replace(/^\?/, '')) location.search = q;
});

const vm = new C4M(opts);
window.c4m = vm;

vm.on('loading', ({ n, of }) => { status.textContent = `loading disk ${n}/${of}`; });
vm.on('status', s => {
  if (s.state === 'loading') return;
  const parts = [s.state, `${fmtRate(s.ips)} inst/s`, `cpu ${Math.round((s.busy || 0) * 100)}%`,
                 `t ${((s.simMs || 0) / 1000).toFixed(1)}s`];
  status.textContent = parts.join(' · ');
  if (s.gui && s.gui.attached)
    info.textContent = `${s.gui.w}×${s.gui.h} ×${canvas.dataset.scale || '1'} · ${s.gui.presents + s.gui.flips} frames${s.gui.shared ? ' · shared' : ''}`;
});
vm.on('kbd', ({ raw }) => updateKbd());
vm.on('exit', e => { status.textContent = `halted, status ${e.status} after ${e.cycles} cycles`; });
vm.on('error', e => { status.textContent = e.message; });

function updateKbd() {
  const focused = document.activeElement === term;
  kbd.classList.toggle('live', focused && !vm.raw);
  kbd.classList.toggle('raw', focused && vm.raw);
  kbd.textContent = !focused ? 'click to type' : vm.raw ? 'typing · raw' : 'typing';
}
term.addEventListener('focus', updateKbd);
term.addEventListener('blur', updateKbd);
term.focus();

// ---- the splitter, and the display scaled to its pane ----------------------
// The terminal's width is dragged; the display takes the rest, and the
// canvas is scaled to fit it, keeping its shape. When a whole-number scale
// (1x, 2x, ...) is within 15% of the best fit, it snaps to that and draws
// with sharp pixels; otherwise it scales smoothly.
const panes = $('panes'), splitter = $('splitter'), wrap = $('display-wrap');
const TERM_W_KEY = 'c4m.js:term-w';
const setTermW = px => {
  const max = panes.clientWidth - 260;
  const w = Math.max(240, Math.min(max, Math.round(px)));
  panes.style.setProperty('--term-w', w + 'px');
  return w;
};
try { const saved = Number(localStorage.getItem(TERM_W_KEY)); if (saved) setTermW(saved); } catch { /* no storage */ }
splitter.addEventListener('pointerdown', e => {
  splitter.setPointerCapture(e.pointerId);
  splitter.classList.add('dragging');
  const x0 = e.clientX, w0 = $('term-pane').getBoundingClientRect().width;
  const move = ev => { setTermW(w0 + ev.clientX - x0); fitDisplay(); };
  const up = () => {
    splitter.removeEventListener('pointermove', move);
    splitter.removeEventListener('pointerup', up);
    splitter.classList.remove('dragging');
    try { localStorage.setItem(TERM_W_KEY, String(Math.round($('term-pane').getBoundingClientRect().width))); } catch { /* no storage */ }
  };
  splitter.addEventListener('pointermove', move);
  splitter.addEventListener('pointerup', up);
  e.preventDefault();
});
splitter.addEventListener('dblclick', () => {
  panes.style.removeProperty('--term-w');
  try { localStorage.removeItem(TERM_W_KEY); } catch { /* no storage */ }
  fitDisplay();
});

function fitDisplay() {
  const r = wrap.getBoundingClientRect();
  const aw = r.width - 16, ah = r.height - 16;
  if (aw <= 0 || ah <= 0 || !canvas.width) return;
  const fit = Math.min(aw / canvas.width, ah / canvas.height);
  const whole = Math.floor(fit);
  const scale = whole >= 1 && whole >= fit * 0.85 ? whole : fit;
  canvas.style.width = Math.floor(canvas.width * scale) + 'px';
  canvas.style.height = Math.floor(canvas.height * scale) + 'px';
  canvas.classList.toggle('crisp', Number.isInteger(scale));
  canvas.dataset.scale = scale.toFixed(2);
}
new ResizeObserver(fitDisplay).observe(wrap);
vm.on('resize', () => requestAnimationFrame(fitDisplay));
fitDisplay();

try {
  await vm.start(opts);
} catch (e) {
  status.textContent = `could not boot: ${e.message}`;
  throw e;
}

$('reset').addEventListener('click', () => { vm.reset(); term.focus(); });
$('ctrlc').addEventListener('click', () => { vm.signal(2); term.focus(); });
// Reset Files: only where the page keeps C4IX's files (?persist=0 has none)
const filesBtn = $('resetfiles');
if (vm.store) filesBtn.style.display = '';
filesBtn.addEventListener('click', async () => {
  if (!confirm("Forget every change kept to C4IX's files -- what was saved, made and deleted -- " +
               'and boot again from the disk as it ships?')) return;
  await vm.resetFiles();
  term.focus();
});
const pauseBtn = $('pause');
let paused = false;
pauseBtn.addEventListener('click', () => {
  paused = !paused;
  if (paused) vm.pause(); else vm.resume();
  pauseBtn.textContent = paused ? 'Resume' : 'Pause';
  pauseBtn.classList.toggle('active', paused);
});
