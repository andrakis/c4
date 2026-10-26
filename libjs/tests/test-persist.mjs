// test-persist.mjs - saves that outlast the page (docs/grin-plan.md 3.8, G4).
//
// Every system the page boots keeps drive 1 in a store of its own
// (app.js's SYSTEMS `persist`; worker.js and page.js do the keeping).
// Here each store is a Map that the machine's drive 1 writes into and
// deletes from, as the page's IndexedDB is, and that the next boot is
// given back.
//
//   - Doom on the bare machine (?system=doom, the `doom` store, on the
//     games disk, src/c4bb/images/games, as app.js boots it): libc's
//     bare.c writes to drive 1. A game is started and saved with keys;
//     doomsav0.dsg is in the store (Doom writes temp.dsg and renames it,
//     which bare.c does by copying and deleting). Doom boots again with
//     that store and -loadgame 0: it reads the save from drive 1 and plays
//     on. (The save pads itself by ftell, which bare.c now answers for a
//     file being written; before, every save was padded wrongly and none
//     loaded back.)
//   - Doom under C4DOS (?system=c4dos, the `c4dos` store, the page's
//     C4DOS disk, where Doom is in \GAMES\DOOM): DRIVES.SYS HOME=B: puts
//     the save on B:, in B:'s c4dos.dir; the bare game's store is
//     untouched by it, and C4DOS's has nothing of the bare game's.
//
//   node libjs/tests/test-persist.mjs

import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { createMachine, boot } from '../boot.js';
import { STOP } from '../c4m.js';
import { GuiDevice, CMD } from '../gui-device.js';

const DISK = new URL('../images/dos/', import.meta.url).pathname;
const GAMES = new URL('../../src/c4bb/images/games/', import.meta.url).pathname;
let fail = 0;
const check = (cond, what, extra = '') => {
  console.log(`test-persist: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-1500) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
if (!existsSync(join(DISK, 'index.json')) || !existsSync(join(DISK, 'GAMES/DOOM/doom.c4r')) || !existsSync(join(GAMES, 'doom.c4r'))) {
  console.log('test-persist: SKIPPED, no libjs/images/dos and games disk with Doom (make c4bb-images builds them)');
  process.exit(0);
}
class LazyDisk extends Map {
  constructor(dir) { super(); this.dir = dir; this.sizes = new Map(JSON.parse(readFileSync(join(dir, 'index.json'), 'utf8'))); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) {
    if (super.has(n)) return super.get(n);
    if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(this.dir, n)));
    super.set(n, b);
    return b;
  }
}

// drive 1 kept in `store`, as the page keeps it; `reads` records what the
// machine opened there
const kept = (store, reads = []) => {
  class Seen extends Map { get(n) { if (super.has(n)) reads.push(n); return super.get(n); } }
  return {
    files: new Seen([...store].map(([n, b]) => [n, b.slice()])), writable: true,
    sink: (n, b) => store.set(n, b.slice()), remove: n => store.delete(n),
  };
};

// a machine with a display, and what drives it
function machine(dir, image, argv, drives) {
  const gui = new GuiDevice({ w: 640, h: 480 });
  const out = [];
  const vm = createMachine({ arenaMb: 64, mhz: 400, gui, drives, onByte: b => out.push(b) });
  boot(vm, new Uint8Array(readFileSync(join(dir, image))), argv);
  const m = { vm, gui, presents: 0, halted: false, text: () => Buffer.from(out).toString('latin1') };
  m.run = (cycles = 4e6) => {
    const r = vm.run(cycles);
    const words = gui.drain();
    for (let i = 0; words && i < words.length; i += words[i]) if (words[i + 1] === CMD.PRESENT) m.presents++;
    if (r === STOP.HALT) m.halted = true;
    return r;
  };
  m.until = (cond, budget = 4000) => { for (let i = 0; i < budget && !m.halted; i++) { if (cond()) return true; m.run(); } return cond(); };
  m.frames = n => { const f = m.presents; return m.until(() => m.presents >= f + n); };
  m.key = (code, ch = 0) => {
    gui.event({ kind: 'keydown', code, char: ch });
    for (let i = 0; i < 20; i++) m.run(1e6);
    gui.event({ kind: 'keyup', code, char: ch });
    for (let i = 0; i < 20; i++) m.run(1e6);
  };
  return m;
}

// From Doom's title loop: a new game (Esc, New Game, the episode, the
// skill), a moment of play, then F2, slot 0, a name, Enter. (Doom writes
// .savegame/temp.dsg and renames it to doomsav0.dsg, and finds that again
// when it loads by the drive's basename fallback.)
const SAVE = 'doomsav0.dsg';
function newGameAndSave(m) {
  m.key(27); m.key(13); m.key(13); m.key(13);      // menu, New Game, Knee-Deep in the Dead, Hurt me plenty
  m.frames(60);
  m.key(113);                                      // F2: Save Game
  m.key(13);                                       // slot 0, editing its name
  m.key(65, 97);                                   // A
  m.key(13);                                       // saved
  m.frames(10);
}
const disk = dir => ({ files: new LazyDisk(dir), writable: false, sink: null });

// ---- Doom on the bare machine: the `doom` store ---------------------------------------
const doomStore = new Map();
{
  const m = machine(GAMES, 'doom.c4r', ['doom.c4r'], [disk(GAMES), kept(doomStore)]);
  check(m.frames(40), 'bare Doom draws its title', m.text().slice(-400));
  newGameAndSave(m);
  check(m.until(() => doomStore.has(SAVE), 400), `a game saved: ${SAVE} is on drive 1, in the doom store`, JSON.stringify([...doomStore.keys()]));
  check(![...doomStore.keys()].some(n => /temp\.dsg$/.test(n)), '... temp.dsg renamed over it, not left behind (bare.c\'s rename)');
  m.key(121); m.key(89, 121);                      // F10, Y: quit
  check(m.until(() => m.halted, 2000), 'Doom quits');
}
{
  const reads = [];
  const m = machine(GAMES, 'doom.c4r', ['doom.c4r', '-loadgame', '0'], [disk(GAMES), kept(doomStore, reads)]);
  check(m.frames(60) && !m.halted && !/Error|tclass/.test(m.text()),
    'bare Doom boots again with the store that came back, and -loadgame 0 plays on', m.text().slice(-400));
  check(reads.includes(SAVE), '... reading the save from drive 1', JSON.stringify(reads));
}

// ---- Doom under C4DOS: the `c4dos` store, B: ----------------------------------------------
const dosStore = new Map();
const doomBefore = new Map([...doomStore].map(([n, b]) => [n, Buffer.from(b).toString('base64')]));
{
  const d = disk(DISK);
  d.files.set('autoexec.bat', new TextEncoder().encode('@ECHO OFF\nECHO test-persist\n'));   // no GRIN
  const m = machine(DISK, 'c4dos32.c4r', ['c4dos32.c4r'], [d, kept(dosStore)]);
  const prompts = () => (m.text().match(/A:\\[^>\n]*>/g) || []).length;
  check(m.until(() => prompts() > 0) && m.text().includes('files are written to B:'), 'C4DOS boots with HOME=B:', m.text().slice(-400));
  let p = prompts();
  m.vm.dev.pushInput('CD \\GAMES\\DOOM\n');
  check(m.until(() => /A:\\GAMES\\DOOM>$/.test(m.text())), 'CD \\GAMES\\DOOM: Doom is in a directory of its own', m.text().slice(-300));
  p = prompts();
  m.vm.dev.pushInput('DOOM\n');
  check(m.frames(40), 'DOOM at A:\\GAMES\\DOOM> draws its title');
  newGameAndSave(m);
  check(m.until(() => dosStore.has(SAVE), 400), `Doom under C4DOS saves to B:: ${SAVE} in the c4dos store`, JSON.stringify([...dosStore.keys()]));
  const dir = dosStore.has('c4dos.dir') ? Buffer.from(dosStore.get('c4dos.dir')).toString('latin1') : '';
  check(dir.split('\n').includes(SAVE) && !/temp\.dsg/.test(dir) && ![...dosStore.keys()].some(n => /temp\.dsg$/.test(n)),
    "... and in B:'s c4dos.dir, with no temp.dsg left", dir);
  m.key(121); m.key(89, 121);
  check(m.until(() => prompts() > p), 'Doom quits to A>');
  p = prompts();
  m.vm.dev.pushInput('DIR B:\n');
  m.until(() => prompts() > p);
  check(m.text().slice(-300).includes(SAVE), 'DIR B: lists the save', m.text().slice(-300));
  p = prompts();
  const at = m.text().length;
  m.vm.dev.pushInput('DOOM -loadgame 0\n');
  check(m.frames(60) && !/Error|tclass/.test(m.text().slice(at)), 'DOOM -loadgame 0 loads it from B: and plays on', m.text().slice(-400));
  m.key(121); m.key(89, 121);
  check(m.until(() => prompts() > p), '... and quits to A>');
}
const same = doomStore.size === doomBefore.size &&
  [...doomStore].every(([n, b]) => doomBefore.get(n) === Buffer.from(b).toString('base64'));
check(same, "the bare game's store is untouched by C4DOS's Doom: the stores are kept apart");
console.log(fail ? 'test-persist: FAILED' : 'test-persist: OK');
process.exit(fail);
