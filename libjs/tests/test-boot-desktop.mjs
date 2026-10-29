// test-boot-desktop.mjs - C4IX booted as the web page boots it (--desktop)
// (docs/c4ix-desktop.md).
//
// Boots C4IX with a display fitted, starts `desktop` from the console,
// and drives it as a person would, with clicks and keys on the display.
// The screen is read back from the text commands of the last frame
// presented, and things are clicked by the text drawn on them, so the
// test follows the layout rather than pinning coordinates.

import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { createMachine, boot } from '../boot.js';
import { STOP } from '../c4m.js';
import { GuiDevice, CMD } from '../gui-device.js';
import { typeBytes } from '../keys.js';

const ROOT = new URL('../../src/c4bb/images/', import.meta.url).pathname;
const SW = 800, SH = 600, TASK_H = 30;
let fail = 0;
const check = (cond, what, extra = '') => {
  console.log(`test-boot-desktop: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(0, 1500) : ''}`);
  if (!cond) fail = 1;
  return cond;
};

// The disk the way the web page now serves it: lazily, a file read from
// the host the first time the machine opens it (worker.js LazyFiles).
const index = JSON.parse(readFileSync(join(ROOT, 'disk/index.json'), 'utf8'));
class LazyDisk extends Map {
  constructor(idx) { super(); this.sizes = new Map(idx); this.fetched = new Set(); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) {
    if (super.has(n)) return super.get(n);
    if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(ROOT, 'disk', n)));
    this.fetched.add(n); super.set(n, b);
    return b;
  }
}
const files = new LazyDisk(index);
const gui = new GuiDevice({ w: 640, h: 480 });
const out = [];
const vm = createMachine({ arenaMb: 64, gui, drives: [{ files, writable: false, sink: null }], onByte: b => out.push(b) });
boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), ['c4ix32.c4r', '--desktop']);
const consoleText = () => Buffer.from(out).toString('latin1');
let presents = 0, flips = 0;
const pump = () => {
  const words = gui.drain();
  for (let i = 0; words && i < words.length; i += words[i]) {
    if (words[i + 1] === CMD.PRESENT) presents++;
    if (words[i + 1] === CMD.FB || words[i + 1] === CMD.FBREF) flips++;
  }
};
const run = (cycles = 2e6) => { const r = vm.run(cycles); pump(); return r; };
const until = (cond, budget = 1500) => { for (let i = 0; i < budget; i++) { if (cond()) return true; if (run() === STOP.HALT) return cond(); } return cond(); };

// The root archive first, waited for, with the loading screen on the
// display: splash flips the logo up, then animates its bar
check(until(() => consoleText().includes('c4ix-root.tgz as task') && consoleText().includes('waiting for it')),
  'init --desktop has vfsload wait for the root archive', consoleText().slice(-600));
check(until(() => flips > 0 && gui.w === SW && gui.h === SH), 'the loading screen puts the logo on the display', consoleText().slice(-600));
{
  const p = presents;
  check(until(() => presents > p + 5) && !consoleText().includes('c4ix-root.tgz unpacked'),
    'and animates its bar while the archive unpacks', `presents ${p} -> ${presents}`);
}
check(until(() => consoleText().includes('desktop: running on the display')), 'init --desktop starts the desktop from the shell, with no typing', consoleText().slice(-600));
{
  const t = consoleText(), u = t.indexOf('vfsload: c4ix-root.tgz unpacked');
  check(u >= 0 && u < t.indexOf('desktop: running on the display') && !/top: \d+ tasks/.test(t.slice(0, u)),
    'and only once the archive is unpacked, as is top', t.slice(-600));
}
check(until(() => /top: \d+ tasks/.test(consoleText())), 'and top on the console beside it', consoleText().slice(-600));
check(until(() => gui.w === SW && gui.h === SH && presents > 0), 'the desktop draws on the display');
// the page clears the canvas when it learns the size (display.js resize),
// and says so: the desktop draws the scene again
for (let i = 0; i < 30; i++) run();
const before = presents;
gui.event({ kind: 'resize', w: SW, h: SH });
check(until(() => presents > before, 200), 'a resize from the page draws the scene again', `presents ${before} -> ${presents}`);
// top is the console's foreground job, so Ctrl-C at the console stops
// it and gives the prompt back; the desktop, in the background, carries on
const n0 = out.length;
vm.pendingSignal = 2;   // Ctrl-C, as keys.js keyDown raises it
check(until(() => /c4ix:\/\$/.test(Buffer.from(out.slice(n0)).toString('latin1')), 300), 'Ctrl-C at the console stops top and the prompt comes back', Buffer.from(out.slice(n0)).toString('latin1').slice(-400));
const n1 = out.length;
typeBytes(vm, new TextEncoder().encode('echo still here\n'));
check(until(() => Buffer.from(out.slice(n1)).toString('latin1').includes('still here\n'), 300), 'and the console shell runs commands', Buffer.from(out.slice(n1)).toString('latin1').slice(-400));
const p0 = presents;
gui.event({ kind: 'resize', w: SW, h: SH });
check(until(() => presents > p0, 200), 'while the desktop keeps drawing');
// /var/log/kernel: the kernel's lines and the boot's, not top's
const n2 = out.length;
typeBytes(vm, new TextEncoder().encode('cat /var/log/kernel\n'));
{
  const logged = () => Buffer.from(out.slice(n2)).toString('latin1');
  check(until(() => logged().includes('vfsload: c4ix-root.tgz unpacked'), 300) && logged().includes('C4IX booting') &&
    logged().includes('c4ix: task 2') && !/top: \d+ tasks/.test(logged()),
    '/var/log/kernel has the boot messages, the kernel\'s and vfsload\'s, and not top\'s', logged().slice(-800));
}
process.exit(fail);
