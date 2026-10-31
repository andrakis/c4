// test-lazy-root.mjs - C4IX's root archive read where it is kept
// (docs/c4ix-design.md, "The root archive, read where it is kept").
//
// Where there is a drive 1, persist inflates c4ix-root.tgz onto it as
// 1:c4ix-base.tar and makes each file in it a lazy file on its span of
// the tar ("1:c4ix-base.tar@OFF+LEN", devices.js diskOpen): nothing of
// the tree is in RAM after boot. A file read is read from the drive; a
// small one is kept while memory allows (the cache, vfs.c) and dropped
// when memory is short, a changed one stays. Checked here:
//
//   - the first boot unpacks onto the drive and reads from there; the
//     second reads the copy it kept; either way /proc/cache has the
//     tree's files still on the host, and the heap after boot is
//     megabytes smaller than a boot without a drive (the tree in RAM)
//   - the files are the archive's, byte for byte (crc32 in the machine
//     against the tar here), small, big and a program; a program in the
//     tree runs; and b4ke finds /build exactly as up to date as the boot
//     without a drive does, so the modification order is the tar's
//   - eviction: files read are kept; with the heap taken past 7/8 from
//     outside, the next allocation drops them all, and read again they
//     are the same; a file changed since boot is not dropped, and keeps
//     its change
//   - a desktop boot that does not fit stops, saying it ran out of memory
//
//   node libjs/tests/test-lazy-root.mjs

import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { gunzipSync, crc32 } from 'node:zlib';
import { createMachine, boot } from '../boot.js';
import { STOP } from '../c4m.js';
import { GuiDevice } from '../gui-device.js';
import { typeBytes } from '../keys.js';

const ROOT = new URL('../../src/c4bb/images/', import.meta.url).pathname;
const DISK = join(ROOT, 'disk');
let fail = 0;
const check = (cond, what, extra = '') => {
  console.log(`test-lazy-root: ${what} ${cond ? 'OK' : 'FAILED'}${!cond && extra ? '\n  ' + String(extra).slice(-1500) : ''}`);
  if (!cond) fail = 1;
  return cond;
};
if (!existsSync(join(DISK, 'c4ix-root.tgz')) || !existsSync(join(DISK, 'index.json'))) {
  console.log('test-lazy-root: SKIPPED, no c4ix-root.tgz (make c4bb-images builds it)');
  process.exit(0);
}

// the archive's files, as the machine should read them
const tar = gunzipSync(readFileSync(join(DISK, 'c4ix-root.tgz')));
const want = new Map();
for (let pos = 0; pos + 512 <= tar.length && tar[pos]; ) {
  const h = tar.subarray(pos, pos + 512), str = (a, n) => h.subarray(a, a + n).toString('latin1').replace(/\0.*$/s, '');
  const name = (h[345] ? str(345, 155) + '/' : '') + str(0, 100), size = parseInt(str(124, 12).trim() || '0', 8);
  if (h[156] === 48 || h[156] === 0) want.set('/' + name.replace(/^\.\//, ''), tar.subarray(pos + 512, pos + 512 + size));
  pos += 512 + Math.ceil(size / 512) * 512;
}
const hex = b => (crc32(b) >>> 0).toString(16).padStart(8, '0');
const small = [...want].filter(([n, b]) => b.length > 0 && b.length <= 65536 && n.startsWith('/usr/src/c4ix/')).slice(0, 24).map(([n]) => n);
const big = [...want].filter(([n, b]) => b.length > 65536).map(([n]) => n).slice(0, 3);
const prog = '/build/bin/c4ix-echo.c4r';
const files = [...small, ...big, '/usr/BUILD.B4K'];
// one a line: the shell's lines are not long enough for them all
const crcs = names => names.map(n => `crc32 ${n}`).join('\n');
const crcLines = (text, names) => names.map(n => (text.match(new RegExp(`^[0-9a-f]{8} \\d+ ${n.replace(/[.+]/g, '\\$&')}$`, 'm')) || [''])[0]);
const wanted = names => names.map(n => `${hex(want.get(n))} ${want.get(n).length} ${n}`);

class LazyDisk extends Map {
  constructor() { super(); this.sizes = new Map(JSON.parse(readFileSync(join(DISK, 'index.json'), 'utf8'))); }
  has(n) { return super.has(n) || this.sizes.has(n); }
  get(n) {
    if (super.has(n)) return super.get(n);
    if (!this.sizes.has(n)) return undefined;
    const b = new Uint8Array(readFileSync(join(DISK, n)));
    super.set(n, b);
    return b;
  }
}
const disk = new LazyDisk();

// C4IX on the console, drive 1 kept in STORE (none: no drive 1)
function machine({ mb = 64, store = null, mmu = true, desktop = false } = {}) {
  const out = [];
  const drives = [{ files: disk, writable: false, sink: null }];
  if (store) drives.push({ files: new Map(store), writable: true, sink: (n, b) => store.set(n, b), remove: n => store.delete(n) });
  const gui = desktop ? new GuiDevice({ w: 640, h: 480 }) : null;
  const vm = createMachine({ arenaMb: mb, mmu, gui, drives, onByte: b => out.push(b) });
  boot(vm, new Uint8Array(readFileSync(join(ROOT, 'c4ix32.c4r'))), desktop ? ['c4ix32.c4r', '--desktop'] : ['c4ix32.c4r']);
  let halted = false;
  const text = (from = 0) => Buffer.from(out.slice(from)).toString('latin1');
  const run = () => { if (vm.run(2e6) === STOP.HALT) halted = true; if (gui) gui.drain(); };
  const until = (cond, budget = 4000) => { for (let i = 0; i < budget && !halted; i++) { if (cond()) return true; run(); } return cond(); };
  let seq = 0;
  // CMDS typed at the prompt; what they printed
  const sh = (cmds, budget = 4000) => {
    const n0 = out.length, mark = `@@${++seq}@@`;
    typeBytes(vm, new TextEncoder().encode(`${cmds}\necho ${mark}\n`));
    until(() => text(n0).includes(mark + '\n'), budget);
    return text(n0);
  };
  return { vm, sh, text, until, halted: () => halted };
}
const cache = t => { const m = /lazy (\d+) (\d+)\ncached (\d+) (\d+)\ndropped (\d+)/.exec(t); return m ? m.slice(1).map(Number) : null; };
const kb = n => Math.round(n / 1024);

// ---- no drive: the tree unpacked into RAM, as before ----
const ram = machine();
const ramOut = ram.sh('wait 4\n' + crcs(files) + '\nb4ke -n -C /usr');
const ramUsed = ram.vm.heap.used;
check(crcLines(ramOut, files).join('\n') === wanted(files).join('\n'), 'without a drive the tree is unpacked into RAM, the archive\'s bytes',
  crcLines(ramOut, files).join('\n'));
const ramB4ke = ramOut.split('\n').filter(l => /^b4ke|RUN|would/.test(l)).join('\n');

// ---- the first boot with a drive: inflated onto it, read from there ----
const store = new Map();
const first = machine({ store });
let t = first.sh('wait 4\ncat /proc/cache');
check(/persist: c4ix-root\.tgz: \d+ files, unpacked onto drive 1 and read from there/.test(t) &&
  store.has('c4ix-base.tar') && store.get('c4ix-base.tar').length === tar.length,
  'the first boot inflates the archive onto drive 1 and reads it from there', t.split('\n').filter(l => /persist|vfsload/.test(l)).join(' | '));
let c = cache(t);
check(c && c[0] >= want.size && c[2] < 16, `its ${want.size} files are on the host after boot (/proc/cache: ${c && c.join(' ')})`, t);
const firstUsed = first.vm.heap.used;
check(ramUsed - firstUsed > 12 << 20, `the heap after boot: ${kb(firstUsed)} KB, ${kb(ramUsed - firstUsed)} KB less than with the tree in RAM`,
  `${kb(ramUsed)} KB without a drive`);

// ---- the second: the copy kept ----
const kept = machine({ store });
t = kept.sh('wait 4\n' + crcs(files) + `\n${prog} lazy-run-ok\nb4ke -n -C /usr\ncat /proc/cache`);
check(/persist: c4ix-root\.tgz: \d+ files from the copy kept on drive 1, read from there/.test(t),
  'the next boot reads the copy kept on drive 1', t.split('\n').filter(l => /persist/.test(l)).join(' | '));
check(crcLines(t, files).join('\n') === wanted(files).join('\n'), `${small.length} small files, ${big.length} big ones and /usr/BUILD.B4K are the archive's, byte for byte`,
  crcLines(t, files).join('\n'));
check(/^lazy-run-ok$/m.test(t), 'a program in the tree runs from the drive');
const keptB4ke = t.split('\n').filter(l => /^b4ke|RUN|would/.test(l)).join('\n');
check(keptB4ke === ramB4ke && ramB4ke.length > 0, 'b4ke finds /build as up to date as with the tree in RAM', `drive:\n${keptB4ke}\nRAM:\n${ramB4ke}`);
c = cache(t);
check(c && c[2] >= small.length && c[3] > 0, `the small files read are kept (/proc/cache: ${c && c.join(' ')})`);

// ---- eviction: memory short, the cache gives way; a changed file stays ----
t = kept.sh(`echo changed > /usr/BUILD.B4K\n${crcs(['/usr/BUILD.B4K'])}`);
const changed = Buffer.from('changed\n');
const changedLine = `${hex(changed)} ${changed.length} /usr/BUILD.B4K`;
check(crcLines(t, ['/usr/BUILD.B4K'])[0] === changedLine, 'a file of the tree written to has its change', crcLines(t, ['/usr/BUILD.B4K'])[0]);
const h = kept.vm.heap, size = h.end - h.base;
const hog = h.alloc(Math.floor(size * 0.92) - h.used);
t = kept.sh('cat /proc/cache');
c = cache(t);
check(hog && c && c[2] === 0 && c[4] >= small.length, `with 92% of the heap taken, the next allocation drops every cached file (/proc/cache: ${c && c.join(' ')})`, t);
h.free(hog);
t = kept.sh(crcs(files));
const again = wanted(files).map(l => l.endsWith(' /usr/BUILD.B4K') ? changedLine : l);
check(crcLines(t, files).join('\n') === again.join('\n'), 'read again from the drive they are the same, and the changed file kept its change',
  crcLines(t, files).join('\n'));
t = kept.sh('persist status');
check(/^  \/usr\/BUILD\.B4K$/m.test(t), 'and persist status has it as a change to save', t);

// ---- without the MMU, the same ----
const nommu = machine({ store, mmu: false });
t = nommu.sh('wait 4\n' + crcs(files));
check(/read from there/.test(t) && crcLines(t, files).join('\n') === wanted(files).join('\n'), 'without the MMU too', crcLines(t, files).join('\n'));

// ---- a desktop boot that does not fit stops, and says why ----
// (without the MMU: with it, swap makes room, and 12 MB boots slowly)
const tiny = machine({ mb: 10, mmu: false, store: new Map(), desktop: true });
tiny.until(() => tiny.halted() || /desktop: running on the display/.test(tiny.text()), 3000);
t = tiny.text();
check(tiny.halted() && /out of memory/.test(t) && !/desktop: running/.test(t) && !/store to|Trap type/i.test(t),
  'a desktop boot in 10 MB without the MMU stops, saying it ran out of memory', t.slice(-700));

process.exit(fail);
