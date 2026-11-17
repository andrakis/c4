// try-osprof.mjs -- where C4IX and its desktop spend their instructions, by
// scenario, for docs/accel.md's OS accelerators: the kernel's functions by its
// symbols, and user code by the functions of the images in USERIMGS (found in
// memory, so the MMU must be off for that breakdown; MMU=1 gives totals).
//
//   node libjs/tests/try-osprof.mjs SCENARIO [SCENARIO...]
//   boot      reset to the desktop's first frame with its Terminal's prompt
//   idle      the desktop with Terminal, System Monitor, Editor, Calculator, 10 s
//   monitor   System Monitor alone in front, 10 s
//   drag      the Terminal's window dragged in 120 steps, one each 20 ms
//   term      `cat` of a 60 KB source in the Terminal, until its prompt is back
//   files     Files opened on /usr/src/c4ix/user, its list scrolled by the wheel
//   edit      Text Editor with a 60 KB source, 40 Page Downs, one each 50 ms
//   menu      the Start menu opened and closed 20 times
//   env MMU=1  TOP=n  CH=cycles a sample  NOACCEL=1
import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { createMachine, boot } from '../boot.js';
import { STOP } from '../c4m.js';
import { GuiDevice, CMD } from '../gui-device.js';
import { parseC4r } from '../../src/c4bb/sim/loader.js';
import { parseSymbols } from '../boot.js';
import { fileURLToPath } from 'node:url';
import { loadAccel, accelManifest } from '../accel.js';

const ROOT = (process.env.IMG || fileURLToPath(new URL('../../src/c4bb/images/', import.meta.url)));
const SCEN = process.argv.slice(2).length ? process.argv.slice(2) : ['boot'];
const CH = +(process.env.CH || 97), TOP = +(process.env.TOP || 30);
const MMU = !!process.env.MMU;
const SW = 800, SH = 600, TASK_H = 30;
const USERIMGS = (process.env.USERIMGS ?? 'c4ix-desktop.c4r,c4ix-libc.c4s,c4ix-libc4ix.c4s,c4ix-libgui.c4s,c4ix-sh.c4r,init.c4r,c4ix-cat.c4r,c4ix-libarc.c4s,c4ix-splash.c4r,c4cc.c4r,c4ix-cc.c4r,c4ix-persist.c4r').split(',').filter(Boolean);
const index = JSON.parse(readFileSync(join(ROOT, 'disk/index.json'), 'utf8'));
class LazyDisk extends Map {
  constructor(idx) { super(); this.sizes = new Map(idx); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) { if (super.has(n)) return super.get(n); if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(ROOT, 'disk', n))); super.set(n, b); return b; }
}
if (!process.env.NOACCEL) await loadAccel(await accelManifest());
const files = new LazyDisk(index);
const base0 = Date.now(); let vmref = null; Date.now = () => base0 + (vmref ? vmref.dev.simMs() : 0);
const gui = new GuiDevice({ w: SW, h: SH });
const out = [];
const vm = createMachine({ arenaMb: 64, mhz: 400, gui, mmu: MMU, accel: !process.env.NOACCEL,
  drives: [{ files, writable: false, sink: null }], onByte: b => out.push(b) });
vmref = vm;
boot(vm, new Uint8Array(readFileSync(process.env.KERNEL || join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r', '--desktop']);
const consoleText = () => Buffer.from(out).toString('latin1');

// the screen's strings (prof-c4ix.mjs's reading of the display)
let frame = [], cur = [], presents = 0, gen = 0, clip = null, partial = false, cmds = 0;
const cmdCount = new Map();
const within = (r, x, y) => x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
const pump = () => { const words = gui.drain();
  for (let i = 0; words && i < words.length; i += words[i]) { const t = words[i + 1]; cmds++; cmdCount.set(t, (cmdCount.get(t) || 0) + 1);
    if (t === CMD.CLEAR) { cur = []; partial = false; }
    if (t === CMD.CLIP) clip = { x: words[i + 2], y: words[i + 3], w: words[i + 4], h: words[i + 5] };
    if (t === CMD.NOCLIP) clip = null;
    if (t === CMD.RECT && partial) { const r = { x: words[i + 2], y: words[i + 3], w: words[i + 4], h: words[i + 5] };
      cur = cur.filter(s => s.gen === gen || !within(r, s.x, s.y) || (clip && !within(clip, s.x, s.y))); }
    if (t === CMD.TEXT2) { const n = words[i + 8]; let s = '';
      for (let k = 0; k < n; k++) s += String.fromCharCode((words[i + 9 + (k >> 2)] >>> ((k & 3) * 8)) & 0xff);
      if (!partial || !clip || within(clip, words[i + 2], words[i + 3])) cur.push({ x: words[i + 2], y: words[i + 3], s, gen }); }
    if (t === CMD.PRESENT) { frame = cur.slice(); presents++; gen++; partial = true; } } };

// the profile: each CH-cycle chunk charged to the function its pc was in
const ksyms = vm.symbols.slice().sort((a, b) => a.addr - b.addr);
const klo = ksyms[0].addr, khi = ksyms[ksyms.length - 1].addr + 4096;
const kfn = pc => { if (pc < klo || pc >= khi) return null; let a = 0, b = ksyms.length - 1;
  while (a < b) { const m = (a + b + 1) >> 1; if (ksyms[m].addr <= pc) a = m; else b = m - 1; } return ksyms[a].name; };
let prof = null, upc = null;
const run = (c = 2e6) => {
  if (!prof) { const r = vm.run(c); pump(); return r; }
  let r, left = c;
  while (left > 0) {
    const pc0 = vm.pc, cc = vm.cycle, kk = vm.skipped;
    r = vm.run(Math.min(CH, left));
    const ran = (vm.cycle - cc) - (vm.skipped - kk); left -= Math.max(vm.cycle - cc, 1);
    const f = kfn(pc0);
    if (f) prof.set(f, (prof.get(f) || 0) + ran); else upc.set(pc0, (upc.get(pc0) || 0) + ran);
    if (r === STOP.HALT) break;
  }
  pump(); return r;
};
const until = (cond, budget = 400) => { for (let i = 0; i < budget; i++) { if (cond()) return true; if (run() === STOP.HALT) return cond(); } return cond(); };
const settle = (n = 20) => { for (let i = 0; i < n; i++) run(); };
const has = re => typeof re === 'string' ? frame.some(t => t.s.trimEnd() === re) : frame.some(t => re.test(t.s));
const find = s => frame.filter(t => (typeof s === 'string' ? t.s.trimEnd() === s : s.test(t.s)));
const ev = (e, c = 3e5) => { gui.event(e); run(c); };
const click = (x, y, button = 0) => { ev({ kind: 'move', x, y, buttons: 0 }); ev({ kind: 'down', x, y, button }); ev({ kind: 'up', x, y, button }); };
const clickText = s => { const all = find(s); if (!all.length) return false; const t = all[all.length - 1]; click(t.x + 5, t.y + 5); return true; };
const key = (ch, mods = 0, code) => { const named = { '\n': 13, '\b': 8 }; const c = code ?? named[ch] ?? ch.toUpperCase().charCodeAt(0);
  ev({ kind: 'keydown', code: c, char: ch === '\n' ? 10 : ch.length === 1 && c !== 8 ? ch.charCodeAt(0) : 0, mods }); };
const type = s => { for (const ch of s) key(ch); };
const start = item => { click(20, SH - TASK_H + 12); until(() => has('Power Off...')); const ok = clickText(item); settle(); return ok; };
const log = (...a) => console.log(...a);
// run until machine time t (ms) passes
const runTo = ms => { while (vm.dev.simMs() < ms) if (run(4e5) === STOP.HALT) break; };

const results = {}; let conAt = 0;
function begin() { conAt = out.length; prof = new Map(); upc = new Map(); cmds = 0; cmdCount.clear();
  return { ms: vm.dev.simMs(), c: vm.cycle, k: vm.skipped, p: presents }; }
async function end(name, s0, units) {
  const pr = prof, up = upc; prof = null; upc = null;
  const ms = vm.dev.simMs() - s0.ms, cyc = (vm.cycle - s0.c) - (vm.skipped - s0.k), np = presents - s0.p;
  let kern = 0; for (const v of pr.values()) kern += v;
  let user = 0; for (const v of up.values()) user += v;
  log(`\n== ${name}${MMU ? ' (MMU)' : ''}: ${ms} machine ms, ${(cyc / 1e6).toFixed(2)} M executed, presents ${np}, gui commands ${cmds}` +
    (units ? `, ${(cyc / units.n / 1e6).toFixed(3)} M per ${units.what}` : ''));
  log(`   kernel ${(100 * kern / cyc).toFixed(1)}%, user ${(100 * user / cyc).toFixed(1)}%;  commands: ` +
    [...cmdCount].sort((a, b) => b[1] - a[1]).map(([t, n]) => `${Object.keys(CMD).find(k => CMD[k] === t) || t} ${n}`).join(', '));
  const rows = [];
  for (const [k, v] of pr) rows.push(['k ' + k, v]);
  // user pcs to the images' functions
  const imgs = [];
  for (const img of USERIMGS) {
    let bytes; try { bytes = new Uint8Array(readFileSync(join(ROOT, 'disk', img))); } catch { continue; }
    let c; try { c = parseC4r(bytes); } catch { continue; }
    const patched = new Set(c.patches.map(p => p.addr));
    let st = -1; for (let i = 0; i + 16 < c.code.length; i++) { let ok = true; for (let j = 0; j < 16; j++) if (patched.has(i + j)) { ok = false; break; } if (ok && c.code[i] !== c.code[i + 1]) { st = i; break; } }
    if (st < 0) continue;
    const i32 = vm.arena.i32; const bases = [];
    outer: for (let a = 0x1000 >> 2; a < i32.length - 16; a++) { if (i32[a] !== c.code[st]) continue; for (let j = 1; j < 16; j++) if (i32[a + j] !== c.code[st + j]) continue outer;
      // the whole image, not just a run another image may share (libc's syscall gate)
      const b0 = a - st; if (b0 < 0 || b0 + c.code.length > i32.length) continue; let bad = 0, tot = 0;
      for (let j = 0; j < c.code.length; j += 7) { if (patched.has(j)) continue; tot++; if (i32[b0 + j] !== c.code[j]) bad++; }
      if (bad * 20 <= tot) bases.push(b0 * 4); }
    const syms = parseSymbols(bytes);
    for (const base of bases) imgs.push({ img, base, end: base + c.code.length * 4, us: syms.map(s => ({ addr: base + s.off * 4, name: s.name })).sort((a, b) => a.addr - b.addr) });
  }
  const unk = { v: 0 };
  for (const [pc, v] of up) {
    const im = imgs.find(m => pc >= m.base && pc < m.end);
    if (!im) { unk.v += v; continue; }
    const us = im.us; let a = 0, b = us.length - 1; while (a < b) { const m = (a + b + 1) >> 1; if (us[m].addr <= pc) a = m; else b = m - 1; }
    rows.push([im.img.replace(/^c4ix-/, '').replace(/\.c4[rs]$/, '') + ' ' + (us[a] ? us[a].name : '?'), v]);
  }
  if (unk.v) rows.push(['(user, unknown image)', unk.v]);
  if (process.env.WHO && unk.v) {
    // which disk image holds the hottest unknown pcs
    const hot = [...up].filter(([pc]) => !imgs.find(m => pc >= m.base && pc < m.end)).sort((a, b) => b[1] - a[1]).slice(0, 5);
    const { readdirSync } = await import('node:fs');
    for (const [pc, v] of hot) {
      const w = [...vm.arena.i32.subarray(pc >> 2, (pc >> 2) + 12)];
      const hits = [];
      for (const fnm of readdirSync(join(ROOT, 'disk'))) { if (!/\.c4[rs]$/.test(fnm)) continue;
        let c; try { c = parseC4r(new Uint8Array(readFileSync(join(ROOT, 'disk', fnm)))); } catch { continue; }
        outer2: for (let i = 0; i + 12 < c.code.length; i++) { for (let j = 0; j < 12; j++) if (c.code[i + j] !== w[j]) continue outer2;
          const sy = parseSymbols(new Uint8Array(readFileSync(join(ROOT, 'disk', fnm)))).filter(q => q.off <= i).sort((a, b) => b.off - a.off)[0];
          hits.push(fnm + ':' + (sy ? sy.name : '?')); break; } }
      log(`   unknown pc ${pc.toString(16)} ${v}: ${hits.join(' ') || 'nowhere'}`);
    }
  }
  const agg = new Map(); for (const [k, v] of rows) agg.set(k, (agg.get(k) || 0) + v);
  const sorted = [...agg].sort((a, b) => b[1] - a[1]);
  sorted.slice(0, TOP).forEach(([k, v]) => log(`  ${(100 * v / cyc).toFixed(2).padStart(6)}%  ${String(v).padStart(10)}  ${k}`));
  if (process.env.CONSOLE) log('   console:', JSON.stringify(Buffer.from(out.slice(conAt)).toString('latin1').slice(0, 1500)), (out.length - conAt) + ' bytes');
  results[name] = { ms, cyc, presents: np, cmds, kern, user, rows: sorted.slice(0, 200) };
}

// ---- boot ----
let s = begin();
if (SCEN.includes('boot')) { prof = s && new Map(); upc = new Map(); }
const booted = until(() => consoleText().includes('desktop: running on the display'), 3000) && until(() => presents > 0 && has(/c4ix:\/\$/), 1500);
if (!booted) { log('no desktop'); log(consoleText().slice(-1500)); process.exit(1); }
if (SCEN.includes('boot')) await end('boot', s); else { prof = null; upc = null; }
settle(40);
const term = () => find(/^Terminal - task/).filter(t => t.y < SH - TASK_H)[0];

for (const sc of SCEN) {
  if (sc === 'boot') continue;
  if (sc === 'idle') {
    start('System Monitor'); start('Text Editor'); start('Calculator'); settle(40);
    s = begin(); runTo(vm.dev.simMs() + 10000); await end('idle', s);
  } else if (sc === 'monitor') {
    start('System Monitor'); settle(40);
    s = begin(); runTo(vm.dev.simMs() + 10000); await end('monitor', s);
  } else if (sc === 'drag') {
    const t = term(); if (!t) { log('drag: no Terminal title'); continue; }
    let x = t.x + 40, y = t.y + 3;
    ev({ kind: 'move', x, y, buttons: 0 }); ev({ kind: 'down', x, y, button: 0 });
    s = begin();
    for (let i = 0; i < 120; i++) {
      const ph = i % 40; x += ph < 20 ? 3 : -3; y += ph < 10 || ph >= 30 ? 2 : -2;
      gui.event({ kind: 'move', x, y, buttons: 1 }); runTo(vm.dev.simMs() + 20);
    }
    gui.event({ kind: 'up', x, y, button: 0 }); runTo(vm.dev.simMs() + 200);
    await end('drag', s, { n: 120, what: 'move' });
  } else if (sc === 'term') {
    const t = term(); if (t) click(t.x + 40, t.y + 3);
    type('cat /usr/src/c4ix/user/top.c >/dev/null; echo xx\n'); settle(20);
    const n0 = presents; s = begin();
    type('cat /usr/src/c4ix/user/desktop/term.c\n');
    const t0 = vm.dev.simMs();
    until(() => vm.dev.simMs() - t0 > 500 && has(/c4ix:\/\$ *$/) && find(/c4ix:\/\$ *$/).some(t => t.y > 300), 20000);
    await end('term', s);
  } else if (sc === 'files') {
    start('Files'); settle(40);
    s = begin();
    const f = find(/^Files - /)[0];
    const fx = f ? f.x + 200 : 300, fy = f ? f.y + 150 : 200;
    for (let i = 0; i < 30; i++) { gui.event({ kind: 'wheel', x: fx, y: fy, dy: i < 15 ? 1 : -1 }); runTo(vm.dev.simMs() + 50); }
    runTo(vm.dev.simMs() + 300);
    await end('files', s, { n: 30, what: 'wheel turn' });
  } else if (sc === 'edit') {
    const t = term(); if (t) click(t.x + 40, t.y + 3);
    type('edit /usr/src/c4ix/user/desktop/term.c\n'); settle(80);
    s = begin();
    for (let i = 0; i < 40; i++) { gui.event({ kind: 'keydown', code: 34, char: 0, mods: 0 }); gui.event({ kind: 'keyup', code: 34, char: 0, mods: 0 }); runTo(vm.dev.simMs() + 50); }
    runTo(vm.dev.simMs() + 300);
    await end('edit', s, { n: 40, what: 'page' });
  } else if (sc === 'cc') {
    const t = term(); if (t) click(t.x + 40, t.y + 3);
    s = begin();
    type('c4cc -o /tmp/bounce.c4r /usr/include/window.h /usr/include/window.c /usr/src/c4ix/examples/bounce.c; echo cc-done\n');
    until(() => has(/^cc-done/), 20000);
    await end('cc', s);
  } else if (sc === 'menu') {
    s = begin();
    for (let i = 0; i < 20; i++) { click(20, SH - TASK_H + 12); runTo(vm.dev.simMs() + 100); key('', 0, 27); runTo(vm.dev.simMs() + 100); }
    await end('menu', s, { n: 20, what: 'open+close' });
  }
}
if (process.env.JSON) writeFileSync(process.env.JSON, JSON.stringify(results));
if (process.env.SCREEN) log('screen:', JSON.stringify(frame.map(t => t.s.trim()).filter(Boolean).slice(0, 300)));
