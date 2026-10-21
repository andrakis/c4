// prof-c4ix.mjs - where C4IX's cycles go (docs/c4ix-cpu.md).
//
// Boots the browser's C4IX (c4ix32.c4r on libjs, 100 MHz, a 64 MB arena),
// sets up one situation, then runs it for SECS machine-seconds in small
// chunks, charging each chunk's cycles to the kernel function its pc was
// in (by the kernel's symbols), or to user code, whose pcs are then
// charged to the functions of the images named in USERIMGS.
//
//   node libjs/tests/prof-c4ix.mjs MODE [SECS]
//   MODE  shell    the console shell at its prompt
//         nowait   the same, without waiting for the root archive
//         conspin  `spin` running at the console (PROG= to run another)
//         desk1    the desktop with its Terminal, idle
//         idle     the desktop with four windows, idle
//         spin     ... and spin running in the Terminal (PROG= another)
//   env   KERNEL= another kernel image   USERIMGS= images to break down
//         CH= chunk size (cycles)   TOP= functions listed   SCREEN=1 the text
//         on screen   TRACE=1 System Monitor's rows as they change
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { createMachine, boot } from '../boot.js';
import { STOP } from '../c4m.js';
import { GuiDevice, CMD } from '../gui-device.js';
import { parseC4r } from '../../src/c4bb/sim/loader.js';
import { parseSymbols } from '../boot.js';
import { typeBytes } from '../keys.js';
const ROOT = (process.env.IMG || new URL("../../src/c4bb/images/", import.meta.url).pathname);
const MODE = process.argv[2] || 'idle';
const SECS = +(process.argv[3] || 10);
const CH = +(process.env.CH || 97);
const SW = 800, SH = 600, TASK_H = 30;
const index = JSON.parse(readFileSync(join(ROOT, 'disk/index.json'), 'utf8'));
class LazyDisk extends Map {
  constructor(idx) { super(); this.sizes = new Map(idx); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) { if (super.has(n)) return super.get(n); if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(ROOT, 'disk', n))); super.set(n, b); return b; }
}
const files = new LazyDisk(index);
const realNow = Date.now; const base0 = realNow(); let vmref = null; if (!process.env.HOSTCLOCK) Date.now = () => base0 + (vmref ? vmref.dev.simMs() : 0);
const gui = new GuiDevice({ w: 640, h: 480 });
const out = [];
const vm = createMachine({ arenaMb: 64, mhz: 100, gui, drives: [{ files, writable: false, sink: null }], onByte: b => out.push(b) });
vmref = vm; boot(vm, new Uint8Array(readFileSync(process.env.KERNEL || join(ROOT, "c4ix32.c4r"))), ['c4ix32.c4r']);
const consoleText = () => Buffer.from(out).toString('latin1');
// The screen's strings, as test-desktop.mjs reads them: a frame without a
// CLEAR (the desktop's damage redraw) paints over part of the one before.
let frame = [], cur = [], presents = 0, gen = 0, clip = null, partial = false;
const within = (r, x, y) => x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
const pump = () => { const words = gui.drain();
  for (let i = 0; words && i < words.length; i += words[i]) { const t = words[i + 1];
    if (t === CMD.CLEAR) { cur = []; partial = false; }
    if (t === CMD.CLIP) clip = { x: words[i + 2], y: words[i + 3], w: words[i + 4], h: words[i + 5] };
    if (t === CMD.NOCLIP) clip = null;
    if (t === CMD.RECT && partial) { const r = { x: words[i + 2], y: words[i + 3], w: words[i + 4], h: words[i + 5] };
      cur = cur.filter(s => s.gen === gen || !within(r, s.x, s.y) || (clip && !within(clip, s.x, s.y))); }
    if (t === CMD.TEXT2) { const n = words[i + 8]; let s = '';
      for (let k = 0; k < n; k++) s += String.fromCharCode((words[i + 9 + (k >> 2)] >>> ((k & 3) * 8)) & 0xff);
      if (!partial || !clip || within(clip, words[i + 2], words[i + 3])) cur.push({ x: words[i + 2], y: words[i + 3], s, gen }); }
    if (t === CMD.PRESENT) { frame = cur.slice(); presents++; gen++; partial = true; } } };
const run = (c = 2e6) => { const r = vm.run(c); pump(); return r; };
const until = (cond, budget = 400) => { for (let i = 0; i < budget; i++) { if (cond()) return true; if (run() === STOP.HALT) return cond(); } return cond(); };
const settle = () => { for (let i = 0; i < 20; i++) run(); };
const has = re => typeof re === 'string' ? frame.some(t => t.s.trimEnd() === re) : frame.some(t => re.test(t.s));
const find = s => frame.filter(t => (typeof s === 'string' ? t.s.trimEnd() === s : s.test(t.s)));
const ev = e => { gui.event(e); run(3e5); };
const click = (x, y, button = 0) => { ev({ kind: 'move', x, y, buttons: 0 }); ev({ kind: 'down', x, y, button }); ev({ kind: 'up', x, y, button }); };
const clickText = s => { const all = find(s); if (!all.length) return false; const t = all[all.length - 1]; click(t.x + 5, t.y + 5); return true; };
const key = (ch, mods = 0, code) => { const named = { '\n': 13, '\b': 8 }; const c = code ?? named[ch] ?? ch.toUpperCase().charCodeAt(0);
  ev({ kind: 'keydown', code: c, char: ch === '\n' ? 10 : ch.length === 1 && c !== 8 ? ch.charCodeAt(0) : 0, mods }); };
const type = s => { for (const ch of s) key(ch); };
const start = item => { click(20, SH - TASK_H + 12); until(() => has('Power Off...')); const ok = clickText(item); settle(); return ok; };
const log = (...a) => console.log(...a);

if (!until(() => consoleText().includes('c4ix:/$'), 2000)) { log('no boot'); process.exit(1); }
if (MODE !== 'nowait') { typeBytes(vm, new TextEncoder().encode('wait 4\n')); until(() => consoleText().includes('[4] done (0)'), 3000); }
if (MODE === 'conspin') { typeBytes(vm, new TextEncoder().encode((process.env.PROG || 'c4ix-spin.c4r 1000') + '\n')); for (let i = 0; i < 20; i++) run(); }
if (MODE === 'shell' || MODE === 'nowait' || MODE === 'conspin') {
} else {
  typeBytes(vm, new TextEncoder().encode('desktop\n'));
  until(() => consoleText().includes('desktop: running on the display'));
  until(() => presents > 0 && has(/c4ix:\/\$/), 800);
  if (MODE !== 'desk1') { start('System Monitor'); start('Text Editor'); start('Calculator'); }
  if (MODE === 'spin') {
    const t = find(/^Terminal - task/).filter(t => t.y < SH - TASK_H)[0]; if (t) click(t.x + 40, t.y + 3); else log('no prompt title');
    type((process.env.PROG || 'c4ix-spin.c4r 1000') + '\n'); settle();
  }
  settle();
}
const syms = vm.symbols.slice().sort((a, b) => a.addr - b.addr);
const lo = syms[0].addr, hi = syms[syms.length - 1].addr + 4096;
const fn = pc => { if (pc < lo || pc >= hi) return '(user code)'; let a = 0, b = syms.length - 1;
  while (a < b) { const m = (a + b + 1) >> 1; if (syms[m].addr <= pc) a = m; else b = m - 1; } return syms[a].name; };
const prof = new Map(); const upc = new Map(); const traps = new Map();
const otrap = vm.trap.bind(vm);
vm.trap = (type, param, h) => { const k = type === 0 ? 'ILLOP ' + param : type === 1 ? 'HARDIRQ ' + param : 'trap' + type + ' ' + param; traps.set(k, (traps.get(k) || 0) + 1); return otrap(type, param, h); };
const pr0 = presents, gc0 = gui.stats().commands; const ms0 = vm.dev.simMs(), c0 = vm.cycle, k0 = vm.skipped, s0 = vm.dev.sleepMs;
let chunks = 0;
while (vm.dev.simMs() - ms0 < SECS * 1000) {
  const pc0 = vm.pc, cc = vm.cycle, kk = vm.skipped;
  const r = vm.run(CH); chunks++;
  const ran = (vm.cycle - cc) - (vm.skipped - kk);
  const f = fn(pc0); if (f === '(user code)') upc.set(pc0, (upc.get(pc0) || 0) + ran);
  prof.set(f, (prof.get(f) || 0) + ran);
  if ((chunks & 1023) === 0) { const p0 = presents; pump(); if (process.env.TRACE && presents !== p0) { const tx = frame.map(t => t.s.trim()); const i = tx.indexOf("idle"); const u = tx.find(x => /^CPU Usage: /.test(x)); const key = tx.slice(i, i + 30).join(" ") + " | " + u; if (key !== globalThis.lastKey) { globalThis.lastKey = key; log("TM@" + (vm.dev.simMs() - ms0) + ": " + key); } } }
  if (r === STOP.HALT) break;
}
pump();
const ms = vm.dev.simMs() - ms0, cyc = (vm.cycle - c0) - (vm.skipped - k0), slept = vm.dev.sleepMs - s0;
log(`mode ${MODE}: ${ms} machine ms, ${cyc} cycles executed (${(cyc / (ms * 100000) * 100).toFixed(1)}% of a 100 MHz machine), slept ${slept} ms, skipped ${vm.skipped - k0}`);
log(`presents ${presents - pr0}, gui commands ${gui.stats().commands - gc0}`);
let kern = 0; for (const [k, v] of prof) if (k !== '(user code)') kern += v;
log(`kernel code ${kern} (${(100 * kern / cyc).toFixed(1)}%), user code ${prof.get('(user code)') || 0} (${(100 * (prof.get('(user code)') || 0) / cyc).toFixed(1)}%)`);
[...prof].sort((a, b) => b[1] - a[1]).slice(0, +(process.env.TOP || 40)).forEach(([k, v]) => log(`  ${(100 * v / cyc).toFixed(2).padStart(6)}%  ${String(v).padStart(10)}  ${k}`));
log('traps/second:'); [...traps].sort((a, b) => b[1] - a[1]).slice(0, 25).forEach(([k, v]) => log(`  ${(v * 1000 / ms).toFixed(0).padStart(7)}  ${k}`));
const tm = frame.map(t => t.s.trim()).filter(Boolean);
if (process.env.SCREEN) log('screen:', JSON.stringify(tm.slice(0, 400)));

for (const img of (process.env.USERIMGS ?? "c4ix-desktop.c4r").split(",").filter(Boolean)) {
  const bytes = new Uint8Array(readFileSync(join(ROOT, "disk", img)));
  const c = parseC4r(bytes); const patched = new Set(c.patches.map(p => p.addr));
  let st = -1; for (let i = 0; i + 16 < c.code.length; i++) { let ok = true; for (let j = 0; j < 16; j++) if (patched.has(i + j)) { ok = false; break; } if (ok && c.code[i] !== c.code[i+1]) { st = i; break; } }
  const i32 = vm.arena.i32; let base = -1;
  outer: for (let a = 0x1000 >> 2; a < i32.length - 16; a++) { if (i32[a] !== c.code[st]) continue; for (let j = 1; j < 16; j++) if (i32[a + j] !== c.code[st + j]) continue outer; base = (a - st) * 4; break; }
  if (base < 0) { log(img + ": not found in memory"); continue; }
  const us = parseSymbols(bytes).map(s => ({ addr: base + s.off * 4, name: s.name })).sort((a, b) => a.addr - b.addr);
  const end = base + c.code.length * 4; const h = new Map(); let tot = 0;
  for (const [pc, v] of upc) { if (pc < base || pc >= end) continue; let a = 0, b = us.length - 1; while (a < b) { const m = (a + b + 1) >> 1; if (us[m].addr <= pc) a = m; else b = m - 1; } h.set(us[a].name, (h.get(us[a].name) || 0) + v); tot += v; }
  log(img + " at " + base.toString(16) + ": " + tot + " cycles (" + (100 * tot / cyc).toFixed(1) + "% of all executed)");
  [...h].sort((a, b) => b[1] - a[1]).slice(0, +(process.env.TOP || 30)).forEach(([k, v]) => log("    " + (100 * v / tot).toFixed(2).padStart(6) + "%  " + String(v).padStart(9) + "  " + k));
}
