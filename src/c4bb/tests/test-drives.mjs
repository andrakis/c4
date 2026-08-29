// test-drives.mjs -- the browser's drives, and the media that outlive
// the page.
//
// The rules worth pinning are the ones the climb turns on, and they are
// the ones that were got wrong in the CLI first: a soft reset must not
// put back a medium somebody took out, and a file written to a medium
// has to still be there when the machine is built again from nothing.
// Both live in DriveSet and MediaStore, neither of which touches the
// DOM or IndexedDB -- which is why they can be tested at all.
//
// Run: node src/c4bb/tests/test-drives.mjs
import { DriveSet, EMPTY, humanBytes } from '../web/drives.js';
import { MediaStore, memoryBackend } from '../web/store.js';

let fails = 0;
const eq = (name, got, want) => {
  if (JSON.stringify(got) === JSON.stringify(want)) { console.log(`  ok   ${name}`); return; }
  console.log(`  FAIL ${name}\n       got  ${JSON.stringify(got)}\n       want ${JSON.stringify(want)}`);
  fails++;
};
const ok = (name, cond) => eq(name, !!cond, true);

const rom = (id, label, files = {}) => ({
  id, label, files: new Map(Object.entries(files).map(([k, v]) => [k, new Uint8Array(v)])),
  sink: null,
});
const rw = (id, label, files = {}) => ({ ...rom(id, label, files), sink: () => {} });

// ---- what is in each drive ------------------------------------------
{
  const s = new DriveSet(3);
  s.define(rom('climb', 'climb disk', { 'c4dos32.c4r': [1, 2, 3] }));
  s.define(rw('blank1', 'blank medium'));

  eq('starts with nothing in it', s.slots, [EMPTY, EMPTY, EMPTY]);
  ok('insert takes', s.insert(0, 'climb'));
  ok('insert refuses a medium that does not exist', !s.insert(1, 'nope'));
  eq('drive 0 holds it', s.summary(0),
     { drive: 0, label: 'climb disk', id: 'climb', empty: false, files: 1, bytes: 3, writable: false });
  eq('drive 1 is empty', s.summary(1).empty, true);

  s.insert(1, 'blank1');
  eq('read-only unless it has a sink',
     s.toDevices().map(d => d.writable), [false, true, false]);
}

// ---- one medium, one drive -------------------------------------------
{
  const s = new DriveSet(3);
  s.define(rw('m', 'a medium'));
  s.insert(0, 'm');
  s.insert(2, 'm');
  eq('moving a medium takes it out of the drive it was in', s.slots, [EMPTY, EMPTY, 'm']);
}

// ---- eject, and what a reset does about it ---------------------------
{
  const s = new DriveSet(3);
  s.define(rom('floppy', 'C4DOS floppy', { 'boot.cfg': [99] }));
  s.define(rw('made', 'what we built', { 'c4ke.c4r': [7, 7] }));
  s.insert(0, 'floppy');
  s.insert(1, 'made');

  ok('eject reports it did something', s.eject(0));
  ok('ejecting an empty drive does not', !s.eject(0));
  eq('the drive is empty now', s.slots[0], EMPTY);
  eq('and the machine sees nothing there', s.toDevices()[0].files.size, 0);

  // The rule the whole climb rests on.
  const after = s.survivesReset();
  eq('a reset does NOT put back what was ejected', after[0].files.size, 0);
  eq('and does keep what was not', after[1].id, 'made');
  eq('the ejected marks are spent', s.ejected, [false, false, false]);
}

// ---- the machine ejecting from inside --------------------------------
{
  const s = new DriveSet(2);
  s.define(rom('floppy', 'C4DOS floppy'));
  s.define(rw('made', 'what we built'));
  s.insert(0, 'floppy');
  s.insert(1, 'made');
  // `RUN reboot.c4r 0` -- the guest takes drive 0 out and resets.
  s.ejectedByGuest(0);
  const after = s.survivesReset();
  eq('a guest eject is the same as the button', after[0].id, EMPTY);
  eq('so the BIOS finds drive 1', after[1].id, 'made');
}

// ---- putting one back ------------------------------------------------
{
  const s = new DriveSet(2);
  s.define(rom('floppy', 'C4DOS floppy'));
  s.insert(0, 'floppy');
  s.eject(0);
  s.insert(0, 'floppy');
  eq('inserting clears the ejected mark', s.ejected[0], false);
  eq('and it survives the next reset', s.survivesReset()[0].id, 'floppy');
}

// ---- forgetting a medium that is in a drive ---------------------------
{
  const s = new DriveSet(2);
  s.define(rw('gone', 'about to be deleted'));
  s.insert(1, 'gone');
  s.forget('gone');
  eq('deleting a medium takes it out of the drive', s.slots[1], EMPTY);
  eq('and the machine is not left pointing at it', s.toDevices()[1].id, EMPTY);
}

// ---- media that outlive the machine -----------------------------------
{
  const store = new MediaStore(memoryBackend());
  const m = await store.create('blank medium');
  ok('a new medium has an id', !!m.id);
  eq('and starts empty', (await store.load(m.id)).size, 0);

  // What the machine does when a guest closes a written file.
  const sink = store.sinkFor(m.id);
  sink('c4ke.c4r', new Uint8Array([1, 2, 3, 4]));
  sink('boot.cfg', new Uint8Array([99]));
  await new Promise(r => setTimeout(r, 0));      // the sink is fire-and-forget

  // Now throw the machine away and build a new one, which is what a
  // page reload is.
  const files = await store.load(m.id);
  eq('the medium remembers what was written', [...files.keys()].sort(),
     ['boot.cfg', 'c4ke.c4r']);
  eq('and the bytes are the bytes', [...files.get('c4ke.c4r')], [1, 2, 3, 4]);

  const listed = await store.list();
  eq('and it is in the catalogue', listed.map(x => x.id), [m.id]);

  await store.erase(m.id);
  eq('erase empties it', (await store.load(m.id)).size, 0);
  eq('but keeps the medium', (await store.list()).map(x => x.id), [m.id]);

  await store.remove(m.id);
  eq('remove takes it away entirely', (await store.list()).length, 0);
}

// ---- two media do not see each other's files --------------------------
{
  const store = new MediaStore(memoryBackend());
  const a = await store.create('a');
  const b = await store.create('b');
  store.sinkFor(a.id)('only-on-a', new Uint8Array([1]));
  await new Promise(r => setTimeout(r, 0));
  eq('a has it', [...(await store.load(a.id)).keys()], ['only-on-a']);
  eq('b does not', [...(await store.load(b.id)).keys()], []);
  await store.remove(a.id);
  eq('and removing a leaves b alone', (await store.list()).map(x => x.label), ['b']);
}

// ---- the label a player reads ------------------------------------------
{
  eq('bytes, small', humanBytes(512), '512 B');
  eq('bytes, kilo', humanBytes(4096), '4 KB');
  eq('bytes, mega', humanBytes(4101902), '3.9 MB');
}

console.log(fails ? `test-drives: ${fails} FAILED` : 'test-drives: OK');
process.exit(fails ? 1 : 0);
