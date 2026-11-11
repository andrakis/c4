// app.js - the c4m.js page: pick a system, boot it, wire the panes.
//
// URL parameters:
//   ?system=NAME                 which machine to boot (default c4ix): c4ix, c4ke,
//                                c4dos, c4dos-ladder, wolf3d, wolf3d6, wolf3d-c4ix, doom, doom-full,
//                                doom1-full, doom2-full, tnt-full, plutonia-full, tyrian, rott,
//                                rottreg, quake, quake-full, duke3d, duke3d-full, gui-demo, fb-demo,
//                                snd-demo, raycast -- SYSTEMS below
//   ?image=URL&disk=URL          any other image and disk instead
//   ?args=A+B                    more arguments for the system's program, after its
//                                own: ?system=wolf3d&args=--secrets+--nodeath
//   ?mhz=N                       how fast the machine runs, paced (default 100)
//   ?snd=0                       no sound card
//   ?cpus=N                      N processors, simulated (smp.js, docs/c4ix-smp.md)
//   ?parallel                    ... each on a worker of its own (par.js), when the
//                                page is cross-origin isolated (serve.mjs --isolate)
//   ?mmu=1 | 0                   fit an MMU (mmu.js, docs/c4ix-mmu.md), or not: C4IX
//                                turns it on, and its user tasks each get an address
//                                space of their own; nothing else uses it. Off
//                                unless ?mmu=1 (or the Configuration dialog says
//                                so), for what it costs; the browser checks fit one
//   ?persist=0 | reset           the system's saved files (its store, SYSTEMS' `persist`) are
//                                not kept between visits / are forgotten first
//   ?uploads=0                   the Uploads (your own files) are not put on the disk
//   ?fast                        run as fast as the host can, unpaced
//   ?realtime=0                  when the host cannot keep up, the machine's clock
//                                slows with it, rather than keeping the wall's
//                                (worker.js: a game then plays in slow motion)
//   ?fpu=0                       no FPU opcode: floats in software (F3, docs/c4-float.md)
//   ?mem=MB                      arena size (default 64)
//   ?shared=0                    use the message display path even when the page
//                                is cross-origin isolated (serve.mjs --isolate)
//   ?coi=0                       no service worker to isolate the page (../coi.js)
//
// The Configuration button shows them all as a form (CONFIG below).
//
// The running machine is window.c4m (page.js's C4M), so the console or a
// test can drive it: c4m.write('ps\n'), c4m.status, await c4m.peek(a, n).

import { C4M } from '../page.js';
import { SITE } from './site.js';
import { MediaStore, indexedDbBackend } from '../../src/c4bb/web/store.js';
import { FOLDERS, uploadName, uploadParts } from '../uploads.js';
import { keptFiles, keptFolders, folderTar } from '../kept.js';

// On a host that cannot isolate the page, coi.js reloads it once with a
// service worker that does: nothing boots before that
await globalThis.c4coi?.ready;

const IMAGES = '../../src/c4bb/images';
const LJIMAGES = '../images';          // the page's own disks (build-images.sh)
// `persist` names the system's store: drive 1 (B:), kept in this browser's
// IndexedDB under that name and given back at the next boot (page.js).
// Every system has its own -- C4DOS's saves are not the bare games', and
// the ladder system has `ladder` -- so Reset Files and
// ?persist= touch the current system's alone (docs/grin-plan.md 3.8).
const SYSTEMS = {
  c4ix: { label: 'C4IX', image: `${IMAGES}/c4ix32.c4r`, disk: `${IMAGES}/disk`, argv: ['c4ix32.c4r', '--desktop'], persist: 'c4ix', mmu: true },
  c4ke: { label: 'C4KE', image: `${IMAGES}/c4ke32.c4r`, disk: `${IMAGES}/disk`, argv: ['c4ke32.c4r'] },
  // C4DOS on the page's own disk (build-images.sh, libjs/images/dos):
  // its AUTOEXEC.BAT starts grin, the graphical shell (docs/grin-plan.md),
  // whose File > Exit grin comes back to the prompt. The games are on it
  // too, each in its directory (\GAMES\DOOM: CD there, then DOOM). Its CONFIG.SYS
  // says DRIVES.SYS HOME=B:, so what is saved goes to B:, the store.
  c4dos: { label: 'C4DOS', image: `${LJIMAGES}/dos/c4dos32.c4r`, disk: `${LJIMAGES}/dos`, argv: ['c4dos32.c4r'], mhz: 400,
           persist: 'c4dos' },
  // The Build Ladder (build-images.sh, libjs/images/ladder;
  // docs/c4dos-ladder.md): C4DOS with the games, and what it takes to
  // climb in one sitting -- BUILD compiles C4KE, dosload boots it, C4KE
  // builds C4IX with b4ke and kboot boots that. 128 MB, because C4KE
  // keeps what its tasks allocate until it stops. Drive 1 is a store of
  // its own, `ladder` (docs/grin-plan.md 3.8): bbsave, save and C4IX's
  // persist write there.
  'c4dos-ladder': { label: 'C4DOS Build Ladder', image: `${LJIMAGES}/ladder/c4dos32.c4r`, disk: `${LJIMAGES}/ladder`,
                    argv: ['c4dos32.c4r'], mhz: 400, mem: 128, persist: 'ladder' },
  // The games with no operating system under them, from the games disk
  // (build-images.sh, src/c4bb/images/games), which stays flat: a bare
  // game opens its files from the root, where the page's C4DOS disk has
  // directories. libc's bare.c writes their saves and settings to drive
  // 1, each game's own store, and reads it before the disk.
  // Wolfenstein 3D with no operating system under it (games/wolf3d/plat-bare.c),
  // and on C4IX drawing on the display itself rather than in a window
  // (wolf3d --direct, like raycast -G). The game times itself against the
  // machine's clock, so these are paced, at a clock it can keep up with.
  wolf3d: { label: 'Wolfenstein 3D (bare machine)', image: `${IMAGES}/games/wolf3d.c4r`, disk: `${IMAGES}/games`,
            argv: ['wolf3d.c4r'], mhz: 400, persist: 'wolf3d' },
  // ... and the registered game (games/wolf3d, wolf3d6), whose files are
  // the person's own: Uploads put them on the disk. Its saves (.WL6) sit
  // beside the shareware's (.WL1) in the one Wolfenstein store.
  wolf3d6: { label: 'Wolfenstein 3D, registered (bare machine)', image: `${IMAGES}/games/wolf3d6.c4r`,
             disk: `${IMAGES}/games`, argv: ['wolf3d6.c4r'], mhz: 400, persist: 'wolf3d' },
  doom: { label: 'Doom (bare machine)', image: `${IMAGES}/games/doom.c4r`, disk: `${IMAGES}/games`,
          argv: ['doom.c4r'], mhz: 400, persist: 'doom' },
  // ... and the person's own IWAD (doom.wad, doom2.wad, plutonia.wad,
  // tnt.wad), which the Uploads put in doom/ or at the root (doom -home,
  // games/doom/README.md): read whole when it opens, Doom II's 14 MB
  'doom-full': { label: 'Doom, your files (bare machine)', image: `${IMAGES}/games/doom.c4r`, disk: `${IMAGES}/games`,
                 argv: ['doom.c4r', '-home'], mhz: 400, mem: 128, persist: 'doom' },
  // ... or one of them by name (-iwad), when there are several
  ...Object.fromEntries([['doom1', 'The Ultimate Doom', 'doom.wad'], ['doom2', 'Doom II', 'doom2.wad'],
                         ['tnt', 'Final Doom: TNT', 'tnt.wad'], ['plutonia', 'Final Doom: Plutonia', 'plutonia.wad']]
    .map(([k, what, wad]) => [`${k}-full`, { label: `Doom, your files: ${what} (bare machine)`, image: `${IMAGES}/games/doom.c4r`,
      disk: `${IMAGES}/games`, argv: ['doom.c4r', '-home', '-iwad', wad], mhz: 400, mem: 128, persist: 'doom' }])),
  // OpenTyrian (games/tyrian), where Tyrian's data was there to put on
  // the disk (games/tyrian/data/README.md)
  tyrian: { label: 'Tyrian (bare machine)', image: `${IMAGES}/games/tyrian.c4r`, disk: `${IMAGES}/games`,
            argv: ['tyrian.c4r'], mhz: 400, persist: 'tyrian' },
  // Rise of the Triad (games/rott), the shareware episode
  rott: { label: 'Rise of the Triad (bare machine)', image: `${IMAGES}/games/rott.c4r`, disk: `${IMAGES}/games`,
          argv: ['rott.c4r'], mhz: 400, persist: 'rott' },
  // ... and the registered game, rottreg, from the person's own DARKWAR
  // files, which the Uploads put in rott/ or at the root (games/rott/README.md).
  // A store of its own: its saved games are not the shareware's.
  rottreg: { label: 'Rise of the Triad, your files (bare machine)', image: `${IMAGES}/games/rottreg.c4r`,
             disk: `${IMAGES}/games`, argv: ['rottreg.c4r'], mhz: 400, persist: 'rottreg' },
  // Quake (games/quake), the shareware episode. The bare libc holds a file in
  // memory whole, and its pak is 18 MB.
  quake: { label: 'Quake (bare machine)', image: `${IMAGES}/games/quake.c4r`, disk: `${IMAGES}/games`,
           argv: ['quake.c4r'], mhz: 400, mem: 128, persist: 'quake' },
  // ... and the registered game from the person's own pak1.pak (and
  // pak0.pak), which the Uploads put in quake/ or at the root (quake -home,
  // games/quake/README.c4.md): 18 and 34 MB, each read whole when it opens
  'quake-full': { label: 'Quake, your files (bare machine)', image: `${IMAGES}/games/quake.c4r`, disk: `${IMAGES}/games`,
                  argv: ['quake.c4r', '-home'], mhz: 400, mem: 256, persist: 'quake' },
  // Duke Nukem 3D (games/duke3d), the shareware episode; and the full game
  // from the person's own DUKE3D.GRP, which the Uploads put on the disk
  // (duke3d -home). The bare libc holds a file in memory whole, and the
  // Atomic Edition's GRP is 44 MB: read into a buffer that doubles as it
  // fills, 96 MB at once at its peak, which 128 MB did not leave room for.
  duke3d: { label: 'Duke Nukem 3D (bare machine)', image: `${IMAGES}/games/duke3d.c4r`, disk: `${IMAGES}/games`,
            argv: ['duke3d.c4r'], mhz: 400, mem: 64, persist: 'duke3d' },
  'duke3d-full': { label: 'Duke Nukem 3D, your files (bare machine)', image: `${IMAGES}/games/duke3d.c4r`,
                   disk: `${IMAGES}/games`, argv: ['duke3d.c4r', '-home'], mhz: 400, mem: 192, persist: 'duke3d' },
  'wolf3d-c4ix': { label: 'Wolfenstein 3D (C4IX, direct)', image: `${IMAGES}/c4ix32.c4r`, disk: `${IMAGES}/disk`,
                   argv: ['c4ix32.c4r', '-e', 'persist -d 60 &', '-e', 'wolf3d --direct'], persist: 'c4ix', mhz: 400, mmu: true },
  'gui-demo': { label: 'GUI demo (bare machine)', image: `${IMAGES}/gui-demo.c4r`, disk: null, argv: ['gui-demo.c4r'] },
  // Computes every pixel itself, so it claims a faster clock than c4bb's 20 MHz.
  'fb-demo': { label: 'Framebuffer demo (bare machine)', image: `${IMAGES}/fb-demo.c4r`, disk: null, argv: ['fb-demo.c4r'], mhz: 100 },
  'snd-demo': { label: 'Sound card demo (bare machine)', image: `${IMAGES}/snd-demo.c4r`, disk: null, argv: ['snd-demo.c4r'] },
  // The raycaster drawing 640x480 on the display, as fast as the host can:
  // unpaced, so the frame rate is the machine's and the display path's.
  raycast: { label: 'Raycast 640x480 (bare machine)', image: `${IMAGES}/disk/raycast.c4r`, disk: null,
             argv: ['raycast.c4r', '31x31', '-G', '640x480'], unpaced: true },
};

// The games are optional (games/README.md): one not built here is not on
// the games disk, and its system is left off the menu
const onDisk = async d => {
  try {
    const r = await fetch(`${d}/index.json`);
    return r.ok ? new Set((await r.json()).map(e => e[0])) : new Set();
  } catch { return new Set(); }
};
const GAMES = await onDisk(`${IMAGES}/games`);
for (const [k, s] of Object.entries(SYSTEMS))
  if (s.disk === `${IMAGES}/games` && !GAMES.has(s.image.split('/').pop())) delete SYSTEMS[k];

const $ = id => document.getElementById(id);
const UPLOADS = 'uploads';   // the IndexedDB medium the Uploads are kept in (page.js)
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

// The MMU: off unless asked for (?mmu=1), since what it costs is what the
// games feel (docs/c4ix-mmu.md 12); a site may fit one by default
// (site.js), on a system that has one. The tests ask for it.
const mmuDefault = !!sys.mmu && SITE.mmu === true;

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
  arenaMb: Number(params.get('mem')) || sys.mem || 64,
  shared: params.get('shared') !== '0',
  snd: params.get('snd') !== '0',
  cpus: Number(params.get('cpus')) || 0,
  parallel: params.has('parallel'),
  mmu: (params.get('mmu') ?? (mmuDefault ? '1' : '0')) === '1',
  persist: params.get('persist') === '0' ? null : sys.persist || null,
  persistReset: params.get('persist') === 'reset',
  uploads: params.get('uploads') === '0' ? null : UPLOADS,
};
// Configuration: the URL's parameters (above) as a form. OK writes the ones
// that differ from their defaults into the URL, keeps any it does not know,
// and so boots the machine again with them. A text box takes any value and
// suggests the usual ones. Two tabs: Machine (what it is) and Other
// (everything else: the ones an entry has no tab for).
const CONFIG = [
  { key: 'system', label: 'System', kind: 'select', def: 'c4ix', tab: 'machine',
    options: Object.entries(SYSTEMS).map(([k, s]) => [k, s.label]) },
  { key: 'mhz', label: 'Speed (MHz)', kind: 'text', tab: 'machine', options: ['20', '50', '100', '200', '400', '800'],
    help: `paced to this clock; empty is the system's own (${sys.mhz || 100})` },
  { key: 'cpus', label: 'Processors', kind: 'text', tab: 'machine', options: ['1', '2', '4', '8'],
    help: 'empty or 1 is one; more are simulated, taking turns' },
  { key: 'parallel', label: 'A thread each', kind: 'flag', tab: 'machine',
    help: 'several processors at once, each on a worker (needs the page cross-origin isolated)' },
  { key: 'mmu', label: 'MMU', kind: 'select', tab: 'machine', def: mmuDefault ? '1' : '0',
    options: [['0', 'off'], ['1', 'on: C4IX gives each program its own memory']],
    help: 'paged memory with protection: a program that writes where it should not is stopped, and nothing else is hurt (C4IX only)' },
  { key: 'mem', label: 'Memory (MB)', kind: 'text', tab: 'machine', options: ['32', '64', '128', '256'],
    help: `the arena; empty is the system's own (${sys.mem || 64})` },
  { key: 'snd', label: 'Sound card', kind: 'onoff', def: true },
  { key: 'fpu', label: 'FPU', kind: 'onoff', def: true, help: 'off: floats in software' },
  { key: 'realtime', label: 'Keep the wall clock', kind: 'onoff', def: true,
    help: 'off: when the host cannot keep up, the machine slows down instead of dropping frames' },
  { key: 'fast', label: 'Unpaced', kind: 'flag', help: 'as fast as the host can, whatever the MHz' },
  { key: 'shared', label: 'Shared display path', kind: 'onoff', def: true,
    help: 'off: the message path even when the page is cross-origin isolated' },
  { key: 'persist', label: 'Saved files', kind: 'select', def: '',
    options: [['', 'kept between visits'], ['0', 'not kept'], ['reset', 'forgotten, then kept']] },
  { key: 'args', label: 'Arguments', kind: 'text', options: [],
    help: "more for the system's program, after its own (wolf3d: --secrets --nodeath)" },
  { key: 'image', label: 'Image URL', kind: 'text', options: [], help: 'any other image instead of the system' },
  { key: 'disk', label: 'Disk URL', kind: 'text', options: [], help: 'its disk (a directory with an index.json)' },
];

// Presets on the Machine tab: the processors and how they run. Each
// fills those two fields in, and OK boots with them as with any other.
const PRESETS = [
  { label: 'Single Core', cpus: '', parallel: false, help: 'one processor' },
  { label: 'Dual CPU', cpus: '2', parallel: true, help: 'two processors, a thread each' },
  { label: 'Quad CPU', cpus: '4', parallel: true, help: 'four processors, a thread each' },
  { label: 'Octo CPU', cpus: '8', parallel: true, help: 'eight processors, a thread each' },
];
const TABS = [['machine', 'Machine'], ['other', 'Other']];

const cfgDialog = $('config');
const cfgFields = $('config-fields');
const cfgInputs = {};
const cfgTabs = document.createElement('div');
cfgTabs.className = 'cfg-tabs';
cfgTabs.setAttribute('role', 'tablist');
const cfgPanels = {};
for (const [t, name] of TABS) {
  const b = document.createElement('button');
  b.type = 'button';
  b.textContent = name;
  b.setAttribute('role', 'tab');
  b.dataset.tab = t;
  b.addEventListener('click', () => cfgTab(t));
  cfgTabs.append(b);
  const panel = document.createElement('div');
  panel.className = 'cfg-panel';
  panel.setAttribute('role', 'tabpanel');
  cfgPanels[t] = panel;
}
cfgFields.append(cfgTabs);
// the presets, first on the Machine tab
const cfgPresets = document.createElement('div');
cfgPresets.className = 'cfg-presets';
for (const pr of PRESETS) {
  const b = document.createElement('button');
  b.type = 'button';
  b.textContent = pr.label;
  b.title = pr.help;
  b.addEventListener('click', () => {
    cfgInputs.cpus.value = pr.cpus;
    cfgInputs.parallel.checked = pr.parallel;
    cfgPresetShow();
  });
  pr.button = b;
  cfgPresets.append(b);
}
const presetLabel = document.createElement('label');
presetLabel.textContent = 'Preset';
cfgPanels.machine.append(presetLabel, cfgPresets);
for (const t of Object.keys(cfgPanels)) cfgFields.append(cfgPanels[t]);
function cfgTab(t) {
  for (const b of cfgTabs.children) b.setAttribute('aria-selected', String(b.dataset.tab === t));
  for (const [k, panel] of Object.entries(cfgPanels)) panel.hidden = k !== t;
}
// the preset the two fields now say, marked
function cfgPresetShow() {
  const cpus = cfgInputs.cpus.value.trim() === '1' ? '' : cfgInputs.cpus.value.trim();
  for (const pr of PRESETS)
    pr.button.classList.toggle('on', pr.cpus === cpus && (pr.parallel === cfgInputs.parallel.checked || !pr.cpus));
}
for (const c of CONFIG) {
  const panel = cfgPanels[c.tab || 'other'];
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
      panel.append(list);
      input.setAttribute('list', list.id);
    }
  }
  input.id = id;
  cfgInputs[c.key] = input;
  panel.append(label, input);
  if (c.help) {
    const help = document.createElement('div');
    help.className = 'help';
    help.textContent = c.help;
    panel.append(help);
  }
}
cfgInputs.cpus.addEventListener('input', cfgPresetShow);
cfgInputs.parallel.addEventListener('change', cfgPresetShow);

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
  cfgPresetShow();
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
  cfgTab('machine');
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
// (1x, 2x, ...) is within 5% of the best fit, it snaps to that and draws
// with sharp pixels; otherwise it scales smoothly. (Within 15% it was:
// 1024x768 in a pane that fits it 1.13 times was left at 1x, with a
// border of an eighth all round.)
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
  const scale = whole >= 1 && whole >= fit * 0.95 ? whole : fit;
  canvas.style.width = Math.floor(canvas.width * scale) + 'px';
  canvas.style.height = Math.floor(canvas.height * scale) + 'px';
  canvas.classList.toggle('crisp', Number.isInteger(scale));
  canvas.dataset.scale = scale.toFixed(2);
}
new ResizeObserver(fitDisplay).observe(wrap);
vm.on('resize', () => requestAnimationFrame(fitDisplay));
// and whenever the canvas is sized at all: the worker says so ('resize')
// for the size registers, a program's attach, but a SIZE command in the
// shared ring -- the desktop's Settings, wk_screen -- sizes it in
// display.js without a word to the page, and the canvas kept the scale
// of the size before
new MutationObserver(() => requestAnimationFrame(fitDisplay))
  .observe(canvas, { attributes: true, attributeFilter: ['width', 'height'] });
fitDisplay();

try {
  await vm.start(opts);
} catch (e) {
  status.textContent = `could not boot: ${e.message}`;
  throw e;
}

$('reset').addEventListener('click', () => { vm.reset(); term.focus(); });
$('ctrlc').addEventListener('click', () => { vm.signal(2); term.focus(); });
// Reset Files: only where the page keeps the system's files (?persist=0
// has none), and only this system's store
const filesBtn = $('resetfiles');
const keptBy = sys.label || 'this system';
if (vm.persistName) {
  filesBtn.style.display = '';
  filesBtn.title = `forget every file ${keptBy} saved in this browser, and boot again from the disk as it ships`;
}
filesBtn.addEventListener('click', async () => {
  if (!confirm(`Forget every change kept to ${keptBy}'s files -- what was saved, made and deleted -- ` +
               'and boot again from the disk as it ships?')) return;
  await vm.resetFiles();
  term.focus();
});

// ---- Uploads: the person's own files, on drive 0 (page.js, opts.uploads) ----
// Kept in IndexedDB, and given to the machine when it boots: adding or
// forgetting one changes nothing until Boot with them, which reloads the
// page (the system's saved files then are as the page's Reload leaves them).
//
// One medium, `uploads`, for every system, unlike the saved files: these
// are what the person brings (Wolfenstein's registered data, a WAD),
// read-only on drive 0 where every system reads -- the bare games, which
// look on drive 1 and then drive 0; C4DOS, which finds them on A: and
// lists them there (uploads.js adds them to A:'s c4dos.dir); C4IX, which
// lists them in /home (uploads.js again, and the manifest's hostdir) -- so
// one upload serves wolf3d6 on the bare machine and WOLF3D under C4DOS.
// Nothing a system writes goes here, and Reset Files leaves them alone:
// Uploads' own Forget and Forget all do that.
//
// Each upload goes in a folder, picked before the files are (FOLDERS,
// uploads.js): /home, or a game's own, /home/doom and the like, where
// that game looks first. Its name in the store is the folder's and the
// file's, "doom/doom2.wad"; one in /home is the file's alone, as before.
const upDialog = $('uploads'), upList = $('uploads-list'), upInput = $('uploads-input');
const upFolder = $('uploads-folder');
const upStore = () => vm.store || (vm.store = new MediaStore(indexedDbBackend()));
const fmtSize = n => n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB' : n >= 1024 ? Math.round(n / 1024) + ' KB' : n + ' bytes';
for (const [f, label, what] of FOLDERS) {
  const o = document.createElement('option');
  o.value = f; o.textContent = label; if (what) o.title = what;
  upFolder.append(o);
}
// the folder last picked, for this browser (a convenience: /home without it)
try { const f = localStorage.getItem('c4m.uploadFolder'); if (FOLDERS.some(([k]) => k === f)) upFolder.value = f; } catch { /* no storage */ }
const folderHint = () => { $('uploads-folder-what').textContent = (FOLDERS.find(([f]) => f === upFolder.value) || [])[2] || ''; };
upFolder.addEventListener('change', () => {
  folderHint();
  try { localStorage.setItem('c4m.uploadFolder', upFolder.value); } catch { /* no storage */ }
});
folderHint();
async function upFiles() {
  // by folder, /home's first, then by name
  const key = n => { const [f, file] = uploadParts(n); return [f, file]; };
  try {
    return [...(await upStore().load(UPLOADS))].sort(([a], [b]) => {
      const [fa, na] = key(a), [fb, nb] = key(b);
      return fa !== fb ? (fa < fb ? -1 : 1) : na < nb ? -1 : na > nb ? 1 : 0;
    });
  } catch { return []; }
}
async function upShow() {
  const files = await upFiles();
  upList.replaceChildren();
  for (const [name, data] of files) {
    const [folder, file] = uploadParts(name);
    const li = document.createElement('li');
    const n = document.createElement('span'); n.className = 'name';
    const d = document.createElement('span'); d.className = 'dim'; d.textContent = `/home/${folder ? folder + '/' : ''}`;
    n.append(d, file);
    const z = document.createElement('span'); z.className = 'dim'; z.textContent = fmtSize(data.length);
    const x = document.createElement('button'); x.type = 'button'; x.className = 'small'; x.textContent = 'Forget';
    x.title = `forget ${file}, in /home/${folder ? folder + '/' : ''}`;
    x.addEventListener('click', () => upForget(name));
    li.append(n, z, x);
    upList.append(li);
  }
  if (!files.length) {
    const li = document.createElement('li'); li.className = 'none'; li.textContent = 'none yet';
    upList.append(li);
  }
  $('uploadsbtn').textContent = files.length ? `Uploads (${files.length})` : 'Uploads';
}
async function upAdd(list) {
  try {
    for (const f of list) await upStore().backend.putFile(UPLOADS, uploadName(upFolder.value, f.name), new Uint8Array(await f.arrayBuffer()));
  } catch (e) { alert(`Could not keep the files in this browser: ${e.message || e}`); }
  await upShow();
}
async function upForget(name) {
  try { await upStore().backend.deleteFile(UPLOADS, name); } catch { /* not kept */ }
  await upShow();
}
$('uploadsbtn').addEventListener('click', async () => { await upShow(); upDialog.showModal(); });
$('uploads-add').addEventListener('click', () => upInput.click());
upInput.addEventListener('change', async () => { await upAdd([...upInput.files]); upInput.value = ''; });
$('uploads-clear').addEventListener('click', async () => {
  if (!confirm('Forget every uploaded file?')) return;
  try { await upStore().erase(UPLOADS); } catch { /* nothing kept */ }
  await upShow();
});
$('uploads-boot').addEventListener('click', () => location.reload());
// files dropped anywhere on the page are uploads too
addEventListener('dragover', e => {
  if (!e.dataTransfer.types.includes('Files')) return;
  e.preventDefault();
  document.body.classList.add('dropping');
});
addEventListener('dragleave', e => { if (!e.relatedTarget) document.body.classList.remove('dropping'); });
addEventListener('drop', async e => {
  document.body.classList.remove('dropping');
  if (!e.dataTransfer.files.length) return;
  e.preventDefault();
  await upAdd([...e.dataTransfer.files]);
  if (!upDialog.open) upDialog.showModal();
});
upShow();

// ---- Downloads: files of the machine's, saved where the browser keeps them ----
// Two ways, for two needs (libjs/README.md, "Downloads"):
// - C4IX hands a file over as it is now: `download PATH` in a shell, or
//   Files' File > Download, a folder as a tar (src/c4ix/user/persist.c).
//   It reaches the page as the worker's `download` (devices.js
//   "download:NAME"), saved here at once.
// - Saved Files lists what the system keeps in this browser, its store
//   (drive 1, SYSTEMS' `persist`), read from IndexedDB without the
//   machine: every system's -- the bare games' saves, C4DOS's B:, C4IX's
//   changes as its last save kept them (kept.js) -- with a file, a
//   folder or the lot to download.
function saveAs(name, bytes) {
  const url = URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' }));
  const a = document.createElement('a');
  a.href = url; a.download = name.split('/').pop() || 'download';
  document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 60000);
}
vm.on('download', ({ name, bytes }) => saveAs(name, bytes));

const svDialog = $('saved'), svList = $('saved-list');
async function svFiles() {
  try { return keptFiles(await upStore().load(vm.persistName)); } catch { return []; }
}
async function svShow() {
  const files = await svFiles();
  const isC4ix = files.some(([p]) => p.startsWith('/'));
  $('saved-what').textContent = `What ${keptBy} keeps in this browser (its drive 1, the store "${vm.persistName}"), ` +
    (isC4ix ? 'as its last save kept it: the desktop saves a minute after a change, and at Shut Down. ' +
              'In C4IX, Files\' File > Download (or download in a shell) hands over a file as it is now.'
            : 'as the system last wrote it.');
  svList.replaceChildren();
  const row = (cls, label, size, button, title, onclick) => {
    const li = document.createElement('li'); li.className = cls;
    const n = document.createElement('span'); n.className = 'name'; n.textContent = label;
    const z = document.createElement('span'); z.className = 'dim'; z.textContent = size;
    const x = document.createElement('button'); x.type = 'button'; x.className = 'small'; x.textContent = button; x.title = title;
    x.addEventListener('click', onclick);
    li.append(n, z, x);
    svList.append(li);
  };
  const dirOf = p => p.slice(0, Math.max(0, p.lastIndexOf('/')));
  for (const [folder, all] of keptFolders(files)) {
    const here = files.filter(([p]) => dirOf(p) === folder);
    if (!here.length && folder) continue;   // a folder with only folders in it: its own row is noise
    const shown = isC4ix ? (folder || '/') : (folder ? folder + '/' : '(the top)');
    row('folder', shown, `${all.length} file${all.length === 1 ? '' : 's'}`, '.tar', `download ${shown} and all in it, as a tar`,
        () => { const t = folderTar(files, folder, vm.persistName); saveAs(t.name, t.bytes); });
    for (const [p, b] of here)
      row('file', p.slice(folder ? folder.length + 1 : 0), fmtSize(b.length), 'Download', `download ${p}`, () => saveAs(p, b));
  }
  if (!files.length) {
    const li = document.createElement('li'); li.className = 'none'; li.textContent = 'nothing kept yet';
    svList.append(li);
  }
  $('saved-all').disabled = !files.length;
}
if (vm.persistName) $('savedbtn').style.display = '';
$('savedbtn').addEventListener('click', async () => { await svShow(); svDialog.showModal(); });
$('saved-refresh').addEventListener('click', svShow);
$('saved-all').addEventListener('click', async () => {
  const t = folderTar(await svFiles(), '', vm.persistName);
  saveAs(t.name, t.bytes);
});
// what the machine keeps while the list is open
vm.on('persist', () => { if (svDialog.open) svShow(); });

const pauseBtn = $('pause');
let paused = false;
pauseBtn.addEventListener('click', () => {
  paused = !paused;
  if (paused) vm.pause(); else vm.resume();
  pauseBtn.textContent = paused ? 'Resume' : 'Pause';
  pauseBtn.classList.toggle('active', paused);
});
