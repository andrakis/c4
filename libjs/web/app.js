// app.js - the c4m.js page: pick a system, boot it, wire the panes.
//
// URL parameters:
//   ?system=c4ix|c4ke|gui-demo|fb-demo|raycast   which machine to boot (default c4ix)
//   ?image=URL&disk=URL          any other image and disk instead
//   ?mhz=N                       how fast the machine runs, paced (default 100)
//   ?fast                        run as fast as the host can, unpaced
//   ?mem=MB                      arena size (default 64)
//   ?shared=0                    use the message display path even when the page
//                                is cross-origin isolated (serve.mjs --isolate)
//
// The running machine is window.c4m (page.js's C4M), so the console or a
// test can drive it: c4m.write('ps\n'), c4m.status, await c4m.peek(a, n).

import { C4M } from '../page.js';

const IMAGES = '../../src/c4bb/images';
const SYSTEMS = {
  c4ix: { label: 'C4IX', image: `${IMAGES}/c4ix32.c4r`, disk: `${IMAGES}/disk`, argv: ['c4ix32.c4r'] },
  c4ke: { label: 'C4KE', image: `${IMAGES}/c4ke32.c4r`, disk: `${IMAGES}/disk`, argv: ['c4ke32.c4r'] },
  'gui-demo': { label: 'GUI demo (bare machine)', image: `${IMAGES}/gui-demo.c4r`, disk: null, argv: ['gui-demo.c4r'] },
  // Computes every pixel itself, so it claims a faster clock than c4bb's 20 MHz.
  'fb-demo': { label: 'Framebuffer demo (bare machine)', image: `${IMAGES}/fb-demo.c4r`, disk: null, argv: ['fb-demo.c4r'], mhz: 100 },
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

const opts = {
  ...sys,
  terminal: term,
  display: canvas,
  // 100 MHz: the interpreter manages well over that on a desktop, an idle
  // machine sleeps whatever the figure, and at c4bb's 20 a full-screen
  // program in the C4IX desktop managed a few frames a second.
  mhz: Number(params.get('mhz')) || sys.mhz || 100,
  unpaced: params.has('fast') || !!sys.unpaced,
  arenaMb: Number(params.get('mem')) || 64,
  shared: params.get('shared') !== '0',
};
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
const pauseBtn = $('pause');
let paused = false;
pauseBtn.addEventListener('click', () => {
  paused = !paused;
  if (paused) vm.pause(); else vm.resume();
  pauseBtn.textContent = paused ? 'Resume' : 'Pause';
  pauseBtn.classList.toggle('active', paused);
});
