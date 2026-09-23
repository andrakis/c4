// app.js - the c4m.js page: pick a system, boot it, wire the panes.
//
// URL parameters:
//   ?system=c4ix|c4ke|gui-demo|fb-demo   which machine to boot (default c4ix)
//   ?image=URL&disk=URL          any other image and disk instead
//   ?mhz=N                       how fast the machine claims to be (default 20)
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
  mhz: Number(params.get('mhz')) || sys.mhz || 20,
  unpaced: params.has('fast'),
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
    info.textContent = `${s.gui.w}×${s.gui.h} · ${s.gui.presents + s.gui.flips} frames${s.gui.shared ? ' · shared' : ''}`;
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
