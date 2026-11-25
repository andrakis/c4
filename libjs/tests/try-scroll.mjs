// try-scroll.mjs - what a turn of the wheel costs in the C4IX desktop's
// IDE and Text Editor, down and up, headless: the instructions executed
// (cycles less those skipped while the machine slept) from the wheel
// event until the desktop is idle again, its frame drawn. And whether the
// wheel reaches the first and the last line, wrapped or not.
//
//   ~/bin/c4-lock node libjs/tests/try-scroll.mjs [FILE] [STEPS]
//
// FILE is a long file on the disk (default the IDE's own source, edit.c).

import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { createMachine, boot } from '../boot.js';
import { STOP } from '../c4m.js';
import { GuiDevice, CMD } from '../gui-device.js';
import { typeBytes } from '../keys.js';
import { fileURLToPath } from 'node:url';

const ROOT = fileURLToPath(new URL('../../src/c4bb/images/', import.meta.url));
const FILE = process.argv[2] || '/usr/src/c4ix/user/desktop/edit.c';
const STEPS = +(process.argv[3] || 30);
const SW = 800, SH = 600, TASK_H = 30;
// the file's lines, from the tree's copy (/usr/src is the repository's src)
const LINES = readFileSync(fileURLToPath(new URL('../../' + FILE.replace(/^\/usr\//, ''), import.meta.url)), 'latin1').split('\n').length - 1;

const index = JSON.parse(readFileSync(join(ROOT, 'disk/index.json'), 'utf8'));
class LazyDisk extends Map {
  constructor(idx) { super(); this.sizes = new Map(idx); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) {
    if (super.has(n)) return super.get(n);
    if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(ROOT, 'disk', n)));
    super.set(n, b);
    return b;
  }
}
const files = new LazyDisk(index);
const gui = new GuiDevice({ w: 640, h: 480 });
const out = [];
const vm = createMachine({ arenaMb: 64, gui, onByte: b => out.push(b),
                          drives: [{ files, writable: false, sink: null },
                                   { files: new Map(), writable: true, sink: () => {}, remove: () => {} }] });
boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r']);
const consoleText = () => Buffer.from(out).toString('latin1');

let frame = [], cur = [], presents = 0;
const pump = () => {
  const words = gui.drain();
  let sized = null;
  for (let i = 0; words && i < words.length; i += words[i]) {
    const t = words[i + 1];
    if (t === CMD.CLEAR) cur = [];
    if (t === CMD.TEXT2) {
      const n = words[i + 8];
      let s = '';
      for (let k = 0; k < n; k++) s += String.fromCharCode((words[i + 9 + (k >> 2)] >>> ((k & 3) * 8)) & 0xff);
      cur.push({ x: words[i + 2], y: words[i + 3], s, adv: words[i + 7] });
    }
    if (t === CMD.PRESENT) { frame = cur.slice(); presents++; }
    if (t === CMD.SIZE) sized = { w: words[i + 2], h: words[i + 3] };
  }
  if (sized && (sized.w !== gui.w || sized.h !== gui.h)) gui.event({ kind: 'resize', ...sized });
};
const run = (cycles = 2e6) => { const r = vm.run(cycles); pump(); return r; };
const until = (cond, budget = 400) => { for (let i = 0; i < budget; i++) { if (cond()) return true; if (run() === STOP.HALT) return cond(); } return cond(); };
const settle = () => { for (let i = 0; i < 20; i++) run(); };
const has = re => typeof re === 'string' ? frame.some(t => t.s.trimEnd() === re) : frame.some(t => re.test(t.s));
const find = s => frame.filter(t => (typeof s === 'string' ? t.s.trimEnd() === s : s.test(t.s)));
const ev = e => { gui.event(e); run(3e5); };
const click = (x, y) => { ev({ kind: 'move', x, y, buttons: 0 }); ev({ kind: 'down', x, y, button: 0 }); ev({ kind: 'up', x, y, button: 0 }); };
const clickText = s => { const all = find(s); if (!all.length) return false; const t = all[all.length - 1]; click(t.x + 5, t.y + 5); return true; };
const key = (ch, mods = 0, code) => {
  const named = { '\n': 13, ESC: 27 };
  const c = code ?? named[ch] ?? ch.toUpperCase().charCodeAt(0);
  ev({ kind: 'keydown', code: c, char: ch === '\n' ? 10 : ch.length === 1 ? ch.charCodeAt(0) : 0, mods });
};
const type = s => { for (const ch of s) key(ch); };
const start = item => { click(20, SH - TASK_H + 12); until(() => has('Power Off...')); clickText(item); settle(); };
const executed = () => vm.cycle - vm.skipped;

until(() => consoleText().includes('c4ix:/$'), 2000);
typeBytes(vm, new TextEncoder().encode('wait 4\n'));
until(() => consoleText().includes('[4] done (0)'), 3000);
typeBytes(vm, new TextEncoder().encode('mkdir /etc\n'));
settle();
typeBytes(vm, new TextEncoder().encode('echo scheme 0 bg -1 wall 0 saver 0 wait 60 > /etc/desktop.cfg\n'));
settle();
typeBytes(vm, new TextEncoder().encode('desktop\n'));
until(() => consoleText().includes('desktop: running on the display'));
until(() => gui.w === SW && presents > 0);
until(() => has(/c4ix:\/\$/), 800);

// one wheel event, and the instructions until the desktop is idle with its frame shown
const wheel = (x, y, dy) => {
  settle();
  const p0 = presents, c0 = executed();
  gui.event({ kind: 'wheel', x, y, dy });
  for (let i = 0; i < 200; i++) { const r = run(2e6); if (presents > p0 && r !== STOP.CYCLES) break; }
  settle();
  return executed() - c0;
};
// the cost of an idle stretch as long as settle(), to take off
const idle = () => { settle(); const c0 = executed(); settle(); return executed() - c0; };

const report = (what, xs) => {
  xs = xs.slice().sort((a, b) => a - b);
  const mean = Math.round(xs.reduce((a, b) => a + b, 0) / xs.length);
  console.log(`try-scroll: ${what}: median ${xs[xs.length >> 1]}, mean ${mean}, min ${xs[0]}, max ${xs[xs.length - 1]} instructions per wheel event (${xs.length} events)`);
  return xs[xs.length >> 1];
};

// ---- the IDE ----
start('IDE');
until(() => has('Untitled - IDE'));
// Traditional: Esc then a command
key('ESC'); type(`e ${FILE}\n`);
const base = FILE.split('/').pop();
until(() => has(`${base} - IDE`), 800);
settle();
const edTitle = () => frame.find(t => / - IDE$/.test(t.s));
const t0 = edTitle();
const ex = t0.x - 27, ey = t0.y - 7;
// the line numbers in the gutter, right of the file pane
const gutter = () => frame.filter(t => t.adv > 0 && /^ *\d+$/.test(t.s) && t.x > ex + 184 && t.x < ex + 216 && t.y > ey + 40 && t.y < ey + 470).map(t => +t.s.trim());
const firstLine = () => Math.min(...gutter());
const lastLine = () => Math.max(...gutter());
const tx = ex + 450, ty = ey + 200;
const ib = idle();
console.log(`try-scroll: idle stretch ${ib} instructions (taken off below)`);
for (const wrap of [1, 0]) {
  if (!wrap) { key('ESC'); type('set nowrap\n'); settle(); }
  // from the middle of the file, both ways
  key('ESC'); type(`${Math.floor(LINES / 2)}\n`); settle();
  const before = firstLine();
  const down = [], upw = [];
  for (let i = 0; i < STEPS; i++) down.push(wheel(tx, ty, 1) - ib);
  const after = firstLine();
  for (let i = 0; i < STEPS; i++) upw.push(wheel(tx, ty, -1) - ib);
  const md = report(`IDE ${wrap ? 'wrap' : 'nowrap'} down`, down), mu = report(`IDE ${wrap ? 'wrap' : 'nowrap'} up`, upw);
  console.log(`try-scroll: IDE ${wrap ? 'wrap' : 'nowrap'}: up costs ${(mu / md).toFixed(2)}x down; first line ${before}, ${after} after ${STEPS} down, ${firstLine()} after as many up`);
  // to the end and back to the start: the wheel, a lot
  let n = 0;
  while (n < 2000 && !gutter().includes(lastLineOfFile())) { wheel(tx, ty, 10); n++; if (n > 400) break; }
  console.log(`try-scroll: IDE ${wrap ? 'wrap' : 'nowrap'}: the wheel down ${n} times shows lines ${firstLine()}..${lastLine()} (the file has ${lastLineOfFile()})`);
  for (let i = 0; i < 40; i++) wheel(tx, ty, 10);
  console.log(`try-scroll: IDE ${wrap ? 'wrap' : 'nowrap'}: more turns down: lines ${firstLine()}..${lastLine()}`);
  for (let i = 0; i < 500 && firstLine() > 1; i++) wheel(tx, ty, -10);
  for (let i = 0; i < 20; i++) wheel(tx, ty, -10);
  console.log(`try-scroll: IDE ${wrap ? 'wrap' : 'nowrap'}: the wheel up to the top shows lines ${firstLine()}..${lastLine()}`);
}
function lastLineOfFile() { return LINES; }
