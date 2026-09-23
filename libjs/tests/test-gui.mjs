// test-gui.mjs - the display device, headless.
//
// Boots gui-demo.c4r on a machine with a GuiDevice fitted (no page: the
// command frames are decoded here), feeds it a mouse move, a click and
// two keys, and checks what comes out: RECT/TEXT/PRESENT frames, the
// text following the mouse, the events echoed to the console, and the
// program quitting on q.

import { readFileSync } from 'node:fs';
import { createMachine, boot } from '../boot.js';
import { STOP } from '../c4m.js';
import { GuiDevice, CMD } from '../gui-device.js';

const IMG = new URL('../../src/c4bb/images/gui-demo.c4r', import.meta.url);
let fail = 0;
const check = (cond, what, extra = '') => {
  console.log(`test-gui: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + extra : ''}`);
  if (!cond) fail = 1;
};

const gui = new GuiDevice({ w: 640, h: 480 });
const out = [];
const vm = createMachine({ arenaMb: 16, gui, onByte: b => out.push(b) });
vm.dev.rxEof = true;
boot(vm, new Uint8Array(readFileSync(IMG)), ['gui-demo.c4r', '200']);

const counts = {};
const texts = [];
let presents = 0;
const decode = words => {
  for (let i = 0; i < words.length; i += words[i]) {
    const type = words[i + 1];
    counts[type] = (counts[type] || 0) + 1;
    if (type === CMD.PRESENT) presents++;
    if (type === CMD.TEXT) {
      const n = words[i + 6];
      let s = '';
      for (let k = 0; k < n; k++) s += String.fromCharCode((words[i + 7 + (k >> 2)] >>> ((k & 3) * 8)) & 0xff);
      texts.push({ x: words[i + 2], y: words[i + 3], s });
    }
  }
};
const text = () => Buffer.from(out).toString('latin1');

// Run slice by slice, as the Worker does, and inject events part way.
let r, slices = 0;
for (; slices < 10000; slices++) {
  r = vm.run(320000);
  const f = gui.drain();
  if (f) decode(f);
  if (r === STOP.HALT) break;
  if (presents === 5 && !gui.injected) {
    gui.injected = true;
    gui.event({ kind: 'move', x: 300, y: 200, buttons: 0 });
    gui.event({ kind: 'down', x: 300, y: 200, button: 0 });
    gui.event({ kind: 'up', x: 300, y: 200, button: 0 });
    gui.event({ kind: 'keydown', code: 75, char: 107, mods: 0 });   // k
  }
  if (presents === 20 && !gui.quit) {
    gui.quit = true;
    gui.event({ kind: 'keydown', code: 81, char: 113, mods: 0 });   // q
  }
}

check(r === STOP.HALT && vm.dev.status === 0, 'the demo exits cleanly', `stop ${r}, status ${vm.dev.status}`);
check(text().includes('gui: fitted, 640x480'), 'it sees the display', text());
check(counts[CMD.CLEAR] >= 20 && counts[CMD.RECT] >= 20 && counts[CMD.LINE] > 100, `it draws (${counts[CMD.CLEAR]} clears, ${counts[CMD.RECT]} rects, ${counts[CMD.LINE]} lines)`);
check(presents >= 20 && presents < 200, `it presents frames (${presents}) and quits on q before its limit`);
check(texts.some(t => t.s === 'hello from c4m' && t.x === 316 && t.y === 194), 'the text follows the mouse to (300,200)');
check(text().includes('gui: click at 300,200'), 'a click comes back to the program');
check(text().includes("gui: key 75 'k'") && text().includes("gui: key 81 'q'"), 'keys come back to the program');
check(gui.dropped === 0 && gui.lostCommands === 0, 'nothing dropped either way');

// ---- the framebuffer, and events by interrupt ------------------------
{
  const FB = new URL('../../src/c4bb/images/fb-demo.c4r', import.meta.url);
  const g = new GuiDevice({ w: 640, h: 480 });
  const o = [];
  const m = createMachine({ arenaMb: 16, gui: g, onByte: b => o.push(b) });
  m.dev.rxEof = true;
  boot(m, new Uint8Array(readFileSync(FB)), ['fb-demo.c4r', '100']);
  let flips = 0, first = null, sent = false, rr;
  for (let i = 0; i < 20000; i++) {
    rr = m.run(320000);
    const f = g.drain();
    if (f) for (let k = 0; k < f.length; k += f[k]) {
      if (f[k + 1] !== CMD.FB) continue;
      flips++;
      if (!first) first = f.slice(k, k + f[k]);
    }
    if (rr === STOP.HALT) break;
    if (flips >= 3 && !sent) {
      sent = true;
      g.event({ kind: 'move', x: 100, y: 60, buttons: 0 });
      g.event({ kind: 'keydown', code: 81, char: 113, mods: 0 });      // q
    }
  }
  const t = Buffer.from(o).toString('latin1');
  check(rr === STOP.HALT && m.dev.status === 0, 'fb-demo exits cleanly', t);
  check(first && first[2] === 320 && first[3] === 240, 'a framebuffer frame is 320x240');
  // frame 0 is a pure function of x and y (t = 0), crosshair aside
  const px = (x, y) => first[4 + y * 320 + x];
  const want = (x, y) => ((x & 255) << 16) | (((y + y) & 255) << 8) | ((x ^ y) & 255);
  check(first && px(10, 20) === want(10, 20) && px(300, 7) === want(300, 7) && px(160, 120) === 0xffffff,
        'its pixels are the ones the guest computed');
  const irq = /(\d+) event interrupts, (\d+) keys/.exec(t);
  check(irq && +irq[1] >= 1 && +irq[2] === 1, `events arrive by interrupt (${irq && irq[0]})`, t);
  check(flips < 100, `q ended it after ${flips} flips`);
}
process.exit(fail);
