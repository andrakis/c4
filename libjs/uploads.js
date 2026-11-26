// uploads.js - the person's own files (page.js opts.uploads), laid where
// each system finds them before the machine boots. worker.js does this at
// every boot, and test-desktop.mjs the same way, so it is tested as the
// page has it.
//
// UPLOADS is [[name, bytes]]. A name is a file's ("doom2.wad"), or a
// folder's and a file's ("doom/doom2.wad"): one of FOLDERS, below, which
// app.js's Uploads offers. Each upload hides any file of that name where
// it is laid, so a program opens it by name as if the disk had it.
// - The bare machine and C4IX: on drive 0 (layUploads), where a bare game
//   opens "doom/doom2.wad" as it is. C4IX's VFS lists only what its
//   manifest and c4ix.sizes name, so each upload is also on the drive as
//   home/NAME, with a line in c4ix.sizes, and the manifest's `home/:
//   hostdir(home/)` (src/c4bb/fs/c4ix.vfs.txt) makes every such file a
//   lazy file in /home (vfsload.c, load_hostdirs), and one in a folder a
//   lazy file in /home/FOLDER: Files, ls and the shell see them there.
// - C4DOS (the page's c4dos and c4dos-ladder systems, app.js's
//   uploadsHome): on B:, its home drive (DRIVES.SYS HOME=B:), not on A:,
//   which keeps only the system -- B:\DUKE3D\DUKE3D.GRP, as C4IX has
//   /home/duke3d/duke3d.grp. HomeFiles, below, is drive 1's files with
//   the uploads over them: what the person saves there stays theirs and
//   is kept by the page as before, the uploads never are, and B:'s
//   listings (c4dos.dir) show both. C4DOS looks for a name with no drive
//   in the home drive's current directory first (c4dos.c, dos_open), so
//   the games' own lookups, "duke3d/duke3d.grp" and then "duke3d.grp",
//   find B:\DUKE3D and then B:\. The ladder's C4IX still finds them in
//   /home, on drive 0.

// The folders an upload can go in: /home itself, and one for each game
// that plays the person's own files. Each such game looks in its own
// folder first, then in /home (on the bare machine in the folder at drive
// 0's root, then at the root; under C4DOS in B:\FOLDER, then B:\):
// games/*/README.
export const FOLDERS = [
  ['', '/home'],
  ['wolf3d', '/home/wolf3d', 'Wolfenstein 3D: wolf3d6 plays the registered *.WL6 files'],
  ['duke3d', '/home/duke3d', 'Duke Nukem 3D: duke3d -home plays DUKE3D.GRP'],
  ['sw', '/home/sw', 'Shadow Warrior: sw -home plays SW.GRP'],
  ['quake', '/home/quake', 'Quake: quake -home plays pak0.pak and pak1.pak'],
  ['doom', '/home/doom', 'Doom: doom -home plays doom.wad, doom2.wad, plutonia.wad, tnt.wad'],
  ['rott', '/home/rott', 'Rise of the Triad: rottreg plays the registered DARKWAR.WAD, .RTL and .RTC (and REMOTE1.RTS, if you have it: else the shareware\'s)'],
  ['tyrian', '/home/tyrian', 'Tyrian 2.1: tyrian --home plays it. Add tyrian21.zip as you downloaded it: its files are taken out of it, into /home/tyrian'],
];

// What each of those games wants of the person's files, for the Uploads'
// list (app.js) and A:\GAMES\README.TXT (uploadsReadme): `all` are
// needed, one of `any` is, `opt` are used when there. Where the files
// are under C4DOS (`dos`), and where to start it on each system: C4IX's
// Go > Games and a Terminal, C4DOS's grin (Program Manager's Full Games
// group, progman.c) and its prompt, and the page's own system for the
// bare machine. Names are as uploadName makes them, lower case.
export const NEEDS = [
  { folder: 'wolf3d', game: 'Wolfenstein 3D (registered)',
    all: ['audiohed.wl6', 'audiot.wl6', 'gamemaps.wl6', 'maphead.wl6', 'vgadict.wl6', 'vgagraph.wl6', 'vgahead.wl6', 'vswap.wl6'],
    c4ix: 'wolf3d6', dos: 'B:\\WOLF3D', c4dos: 'Full Games > Wolfenstein 3D, or \\GAMES\\WOLF3D\\WOLF3D6', bare: 'wolf3d6' },
  { folder: 'doom', game: 'Doom (The Ultimate Doom, Doom II, Final Doom)',
    any: ['doom.wad', 'doom2.wad', 'tnt.wad', 'plutonia.wad'],
    c4ix: 'doom -home', dos: 'B:\\DOOM', c4dos: 'Full Games > The Ultimate Doom, Doom II, TNT, Plutonia, or \\GAMES\\DOOM\\DOOM -home', bare: 'doom-full' },
  { folder: 'duke3d', game: 'Duke Nukem 3D (1.3D, 1.4, Atomic 1.5)',
    all: ['duke3d.grp'],
    c4ix: 'duke3d -home', dos: 'B:\\DUKE3D', c4dos: 'Full Games > Duke Nukem 3D, or \\GAMES\\DUKE3D\\DUKE3D -home', bare: 'duke3d-full' },
  { folder: 'sw', game: 'Shadow Warrior (registered 1.2)',
    all: ['sw.grp'],
    c4ix: 'sw -home', dos: 'B:\\SW', c4dos: 'Full Games > Shadow Warrior, or \\GAMES\\SW\\SW -home', bare: 'sw-full' },
  { folder: 'quake', game: 'Quake (registered)',
    all: ['pak1.pak'], opt: ['pak0.pak'],
    c4ix: 'quake -home', dos: 'B:\\QUAKE', c4dos: 'Full Games > Quake, or \\GAMES\\QUAKE\\QUAKE -home', bare: 'quake-full' },
  { folder: 'rott', game: 'Rise of the Triad (registered)',
    all: ['darkwar.wad', 'darkwar.rtl', 'darkwar.rtc'], opt: ['remote1.rts'],
    c4ix: 'rottreg', dos: 'B:\\ROTT', c4dos: 'Full Games > Rise of the Triad, or \\GAMES\\ROTT\\ROTTREG', bare: 'rottreg' },
  // Tyrian 2.1 is freeware, but its license.doc lets nobody pass it on, so
  // the page has only the engine (OpenTyrian, games/tyrian) and the person
  // brings the data: `zip`, as downloaded from `url` (where OpenTyrian's
  // README points), whose files unpackUpload takes out of it. `all` is
  // what of it the game reads (games/tyrian/tools/tyrdata.py's choice).
  { folder: 'tyrian', game: 'Tyrian 2.1', zip: 'tyrian21.zip', url: 'https://camanis.net/tyrian/tyrian21.zip',
    all: ['cubetxt1.dat', 'cubetxt2.dat', 'cubetxt3.dat', 'cubetxt4.dat', 'demo.1', 'demo.2', 'demo.3', 'demo.4', 'demo.5',
          'estsc.shp', 'levels1.dat', 'levels2.dat', 'levels3.dat', 'levels4.dat', 'music.mus',
          'newsh#.shp', 'newsh0.shp', 'newsh1.shp', 'newsh2.shp', 'newsh3.shp', 'newsh4.shp', 'newsh5.shp', 'newsh6.shp',
          'newsh7.shp', 'newsh8.shp', 'newsh9.shp', 'newsh^.shp', 'newsha.shp', 'newshb.shp', 'newshc.shp', 'newshd.shp',
          'newshe.shp', 'newshf.shp', 'newshg.shp', 'newshh.shp', 'newshi.shp', 'newshj.shp', 'newshk.shp', 'newshl.shp',
          'newshm.shp', 'newshn.shp', 'newsho.shp', 'newshp.shp', 'newshr.shp', 'newshs.shp', 'newsht.shp', 'newshu.shp',
          'newshv.shp', 'newsh~.shp', 'palette.dat', 'shapes).dat', 'shapesw.dat', 'shapesx.dat', 'shapesy.dat', 'shapesz.dat',
          'shipedit.pcx', 'tshp2.pcx', 'tyrend.anm', 'tyrian.cdt', 'tyrian.hdt', 'tyrian.pic', 'tyrian.shp', 'tyrian.snd',
          'tyrian1.lvl', 'tyrian2.lvl', 'tyrian3.lvl', 'tyrian4.lvl', 'tyrianc.shp', 'tyrset.pcx', 'user1.shp', 'user2.shp',
          'voices.snd', 'voicesc.snd'],
    c4ix: 'tyrian --home', dos: 'B:\\TYRIAN', c4dos: 'Full Games > Tyrian, or \\GAMES\\TYRIAN\\TYRIAN --home', bare: 'tyrian-full' },
];

// How far the uploads (their names, as kept) go towards each game of NEEDS:
// [{ need, have: [names found], missing: [names], ready }]. A file counts in
// the game's own folder or in /home, as the games look.
export function needsStatus(names) {
  const set = new Set(names.map(n => n.toLowerCase()));
  const has = (folder, f) => set.has(`${folder}/${f}`) || set.has(f);
  return NEEDS.map(need => {
    const all = need.all || [], any = need.any || [], opt = need.opt || [];
    const have = [...all, ...any, ...opt].filter(f => has(need.folder, f));
    const missing = all.filter(f => !has(need.folder, f));
    const anyOk = !any.length || any.some(f => has(need.folder, f));
    return { need, have, missing, anyMissing: anyOk ? [] : any, ready: !missing.length && anyOk };
  });
}

// An upload's name: its folder's, if it has one, and the file's, lower case
export const uploadName = (folder, file) => (FOLDERS.some(([f]) => f === folder) && folder ? folder + '/' : '') + file.toLowerCase();
// ... and back: [folder, file]
export const uploadParts = name => { const i = name.indexOf('/'); return i < 0 ? ['', name] : [name.slice(0, i), name.slice(i + 1)]; };

// What the Uploads keep of a file added in FOLDER: [[name, bytes]]. Most
// are kept as they are, under uploadName. A game's zip of NEEDS (its
// `zip`: tyrian21.zip) is taken apart instead, whatever folder was picked:
// the files of it the game reads, in the game's folder, flat and lower
// case ("tyrian21/TYRIAN1.LVL" is tyrian/tyrian1.lvl), the rest (its DOS
// programs, its documents) left out. A zip of that name with none of them
// in it is kept as it is.
export async function unpackUpload(folder, file, bytes) {
  const b = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  const need = NEEDS.find(n => n.zip && n.zip === file.toLowerCase());
  if (!need) return [[uploadName(folder, file), b]];
  const want = new Set([...(need.all || []), ...(need.any || []), ...(need.opt || [])]);
  const out = [];
  for (const [n, data] of await unzip(b)) {
    const f = n.split('/').pop().toLowerCase();
    if (want.has(f)) out.push([uploadName(need.folder, f), data]);
  }
  return out.length ? out : [[uploadName(folder, file), b]];
}

// A zip's files, [[name, bytes]], directories left out: stored or
// deflated, as zips mostly are, found by its central directory. Deflate is
// the platform's (DecompressionStream, in the page and in node).
export async function unzip(bytes) {
  const b = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
  let end = -1;
  for (let i = b.length - 22; i >= 0 && i >= b.length - 22 - 65535; i--) if (v.getUint32(i, true) === 0x06054b50) { end = i; break; }
  if (end < 0) throw new Error('not a zip (no end of central directory)');
  const count = v.getUint16(end + 10, true);
  let p = v.getUint32(end + 16, true);
  const out = [];
  for (let k = 0; k < count; k++) {
    if (v.getUint32(p, true) !== 0x02014b50) throw new Error('a broken zip (its central directory)');
    const method = v.getUint16(p + 10, true), csize = v.getUint32(p + 20, true), size = v.getUint32(p + 24, true);
    const nlen = v.getUint16(p + 28, true), xlen = v.getUint16(p + 30, true), clen = v.getUint16(p + 32, true);
    const local = v.getUint32(p + 42, true);
    const name = new TextDecoder().decode(b.subarray(p + 46, p + 46 + nlen));
    p += 46 + nlen + xlen + clen;
    if (name.endsWith('/')) continue;
    if (v.getUint32(local, true) !== 0x04034b50) throw new Error(`a broken zip (${name})`);
    const start = local + 30 + v.getUint16(local + 26, true) + v.getUint16(local + 28, true);
    const raw = b.subarray(start, start + csize);
    let data;
    if (method === 0) data = raw.slice();
    else if (method === 8) data = new Uint8Array(await new Response(new Blob([raw]).stream().pipeThrough(new DecompressionStream('deflate-raw'))).arrayBuffer());
    else throw new Error(`${name} is packed in a way this does not unpack (method ${method})`);
    if (data.length !== size) throw new Error(`a broken zip (${name}: ${data.length} bytes, not ${size})`);
    out.push([name, data]);
  }
  return out;
}

const text = b => new TextDecoder().decode(b);
const bytes = s => new TextEncoder().encode(s);

// NAMES joined to directory DIR's c4dos.dir ("" the root, "DOOM/" a
// directory), those it does not list already in any case. A directory
// the disk does not have gets a listing of its own.
function list(files, dir, names) {
  const key = dir + 'c4dos.dir';
  const had = files.has(key) ? text(files.get(key)) : '';
  const listed = new Set(had.split(/\r?\n/).map(l => l.toLowerCase()));
  const add = names.filter(n => !listed.has(n.toLowerCase()));
  if (add.length || !had) files.set(key, bytes((had ? had.replace(/\n?$/, '\n') : '') + add.join('\n') + (add.length ? '\n' : '')));
}

// A SoundFont, for General MIDI music (page.js, opts.music): an upload
// ending .sf2, .sf3 or .dls. The one named `want` if there is one, else
// the first in a soundfonts/ folder (/home/soundfonts), else the first.
// [name, bytes], or null.
export const isSoundFont = name => /\.(sf2|sf3|dls)$/i.test(name);
export function pickSoundFont(uploads, want) {
  const sfs = (uploads || []).filter(([n]) => isSoundFont(n)).sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0));
  if (want) {
    const w = want.toLowerCase();
    const hit = sfs.find(([n]) => n.toLowerCase() === w) || sfs.find(([n]) => n.toLowerCase().split('/').pop() === w);
    if (hit) return hit;
  }
  return sfs.find(([n]) => n.toLowerCase().startsWith('soundfonts/')) || sfs[0] || null;
}

// The uploads on drive 0 (FILES, its Map or worker.js's LazyFiles), for
// the bare machine and C4IX. With HOME (a C4DOS system, which has them on
// B:: HomeFiles), only C4IX's /home copies, for the ladder's C4IX.
export function layUploads(files, uploads, home = false) {
  if (!uploads || !uploads.length) return;
  const ups = uploads.map(([n, b]) => [n, b instanceof Uint8Array ? b : new Uint8Array(b)]);
  if (!home) for (const [n, b] of ups) files.set(n, b);
  const dirFile = home ? null : files.get('c4dos.dir');
  if (dirFile) {
    const root = text(dirFile).split(/\r?\n/);
    const folders = new Map();   // folder -> [file]
    for (const [n] of ups) {
      const [f, file] = uploadParts(n);
      if (f) { if (!folders.has(f)) folders.set(f, []); folders.get(f).push(file); }
    }
    // a folder the root lists already, in its own spelling (C4DOS asks
    // the host for "DOOM/x" as the listing spells it): the files there too
    const spelt = f => (root.find(l => l.toLowerCase() === f + '/') || f + '/').slice(0, -1);
    for (const [f, names] of folders) {
      const real = spelt(f);
      if (real !== f) for (const file of names) files.set(`${real}/${file}`, files.get(`${f}/${file}`));
      list(files, real + '/', names);
    }
    list(files, '', [...ups.map(([n]) => n).filter(n => !n.includes('/')), ...[...folders.keys()].map(f => spelt(f) + '/')]);
  }
  const sizesFile = files.get('c4ix.sizes');
  if (sizesFile) {
    for (const [n, b] of ups) files.set('home/' + n, b);
    files.set('c4ix.sizes', bytes(text(sizesFile).replace(/\n?$/, '\n') + ups.map(([n, b]) => `${b.length} home/${n}\n`).join('')));
  }
}

// ---- under C4DOS: the uploads on B: ----
//
// Drive 1's files (BASE, the Map the page keeps) with the uploads over
// them, as the disk device asks a drive's files (devices.js): get, has,
// set, delete. An upload is found by its name in any case
// (B:\DUKE3D\DUKE3D.GRP is "duke3d/duke3d.grp"), and hides a file of
// that name of the person's own while it is uploaded. B:'s listings
// (c4dos.dir, the root's and each folder's) are BASE's with the uploads
// added, so DIR and grin's File Manager show them; a listing C4DOS writes
// back (a file saved, a directory made) is kept without them (kept), so
// that an upload forgotten in the Uploads dialog leaves nothing behind,
// and a folder the person saves into is in the root's kept listing too
// (KEEP, the page's store, is told). Uploads cannot be deleted from the
// machine (locked: DEL says it could not): Forget in the Uploads dialog
// does that, from the next boot.
const lc = s => s.toLowerCase();
export class HomeFiles {
  constructor(base, uploads, keep = null) {
    this.base = base;
    this.keep = keep;
    this.up = new Map();
    for (const [n, b] of uploads || []) this.up.set(lc(n), b instanceof Uint8Array ? b : new Uint8Array(b));
  }
  // the folder ('' the root) whose listing NAME is, or null for any other file
  static listingOf(name) { const m = /^(?:(.*)\/)?c4dos\.dir$/i.exec(name); return m ? (m[1] || '') : null; }
  // the names the uploads put in folder DIR's listing: files, and at the root folders ("duke3d/")
  added(dir) {
    const d = lc(dir), out = new Set();
    for (const n of this.up.keys()) {
      const i = n.indexOf('/');
      if (!d) out.add(i < 0 ? n : n.slice(0, i + 1));
      else if (i >= 0 && n.slice(0, i) === d) out.add(n.slice(i + 1));
    }
    return [...out];
  }
  // does BASE have NAME, in any case? Has it anything in folder F?
  baseHas(name) { const l = lc(name); for (const k of this.base.keys()) if (lc(k) === l) return true; return false; }
  baseHasIn(f) { const l = lc(f) + '/'; for (const k of this.base.keys()) if (lc(k).startsWith(l)) return true; return false; }
  get(name) {
    const u = this.up.get(lc(name));
    if (u) return u;
    const dir = HomeFiles.listingOf(name), had = this.base.get(name);
    if (dir === null) return had;
    const adds = this.added(dir);
    if (!adds.length) return had;
    const text = had ? new TextDecoder().decode(had) : '';
    const listed = new Set(text.split(/\r?\n/).map(lc));
    const more = adds.filter(n => !listed.has(n));
    if (!more.length) return had;
    return new TextEncoder().encode((text ? text.replace(/\n?$/, '\n') : '') + more.join('\n') + '\n');
  }
  has(name) { return this.get(name) !== undefined; }
  // what of a listing C4DOS writes is the person's: the uploads' names out,
  // but for a file or folder of the same name of the person's own
  kept(name, bytes) {
    const dir = HomeFiles.listingOf(name);
    if (dir === null) return bytes;
    const adds = new Set(this.added(dir));
    if (!adds.size) return bytes;
    const lines = new TextDecoder().decode(bytes).split(/\r?\n/).filter(l => {
      const n = lc(l);
      if (!n || !adds.has(n)) return true;
      if (n.endsWith('/')) return this.baseHasIn(n.slice(0, -1));
      return this.baseHas(dir ? `${dir}/${l}` : l);
    }).filter(Boolean);
    return new TextEncoder().encode(lines.length ? lines.join('\n') + '\n' : '');
  }
  set(name, bytes) {
    const k = this.kept(name, bytes);
    this.base.set(name, k);
    // a folder's listing kept: its "NAME/" in the root's kept listing too
    const dir = HomeFiles.listingOf(name);
    if (dir && !dir.includes('/')) {
      const root = this.base.get('c4dos.dir');
      const text = root ? new TextDecoder().decode(root) : '';
      if (!text.split(/\r?\n/).some(l => lc(l) === lc(dir) + '/')) {
        const b = new TextEncoder().encode((text ? text.replace(/\n?$/, '\n') : '') + dir + '/\n');
        this.base.set('c4dos.dir', b);
        if (this.keep) this.keep('c4dos.dir', b);
      }
    }
    return this;
  }
  delete(name) { return this.locked(name) ? false : this.base.delete(name); }
  locked(name) { return this.up.has(lc(name)); }
  keys() { return new Set([...this.base.keys(), ...this.up.keys()]).values(); }
  get size() { return [...this.keys()].length; }
  *entries() { for (const k of this.keys()) yield [k, this.get(k)]; }
  [Symbol.iterator]() { return this.entries(); }
}

// ---- A:\GAMES\README.TXT ----
//
// What grin's Full Games (Uploads) group's README.TXT says, from NEEDS
// (page-disks.sh writes it onto the C4DOS disks at build time, with
// libjs/uploads-readme.mjs), so the two cannot part. Plain words and
// short lines, for Notepad and TYPE; DOS's line ends. DRIVE is where that
// C4DOS has the uploads: B, the page's (its home drive), or A, the Build
// Ladder's, which has no home drive and keeps them beside the system.
export function uploadsReadme(drive = 'B') {
  const L = [], d = drive.toUpperCase() + ':';
  const where = d === 'B:' ? 'B:, the home drive,' : `${d}, beside the disk's own files,`;
  // a paragraph in lines of 70 at most (Notepad's width at 640 x 480), the first after INDENT, the rest after MORE
  const wrap = (text, indent = '', more = indent) => {
    let line = indent;
    for (const w of text.split(' ')) {
      if (line.trim() && line.length + w.length + 1 > 70) { L.push(line); line = more; }
      line += (line.trim() ? ' ' : '') + w;
    }
    if (line.trim()) L.push(line);
  };
  L.push('FULL GAMES (UPLOADS): playing the full games with your own files', '');
  wrap(`The games in the Full Games (Uploads) group play the full, registered versions, from game files you own. This disk does not have them: you add them to the page, and C4DOS finds them on ${d}.`);
  L.push('', 'ADDING YOUR FILES', '');
  wrap('1. Click Uploads, at the top of the page.', '', '   ');
  wrap('2. In "Put new files in", pick the game\'s folder (for Duke Nukem 3D, /home/duke3d).', '', '   ');
  wrap('3. Click Add files..., and pick the game\'s files (or drop them on the page). The dialog\'s "What the games need" list ticks each one it has.', '', '   ');
  wrap('4. Click Boot with them.', '', '   ');
  L.push('');
  wrap(`Under C4DOS the files are then on ${where} in the game's folder: ${d}\\DUKE3D\\DUKE3D.GRP. They are kept in the browser, not on the disk, and Forget in the Uploads dialog takes them away. While a file is uploaded, it hides a file of the same name on ${d}.`);
  L.push('', 'WHAT EACH GAME NEEDS', '');
  for (const n of NEEDS) {
    L.push(n.game);
    // a game whose zip the Uploads take apart: the zip, and where it is
    if (n.zip) {
      wrap(`Needs: ${n.zip.toUpperCase()}, as you downloaded it${n.url ? ', from' : ''}`, '    ');
      if (n.url) L.push(`    ${n.url}`);
      wrap('Add the zip itself: the Uploads take its files out of it.', '    ');
    } else if (n.all && n.all.length) wrap(`Needs: ${n.all.map(f => f.toUpperCase()).join(', ')}`, '    ');
    if (n.any && n.any.length) wrap(`Needs one of: ${n.any.map(f => f.toUpperCase()).join(', ')}`, '    ');
    if (n.opt && n.opt.length) wrap(`Also, if you have it: ${n.opt.map(f => f.toUpperCase()).join(', ')}`, '    ');
    wrap(`In: ${d}${n.dos.slice(2)} (the Uploads folder /home/${n.folder})`, '    ');
    wrap(`Start: ${n.c4dos.replace(/^Full Games/, 'grin, Full Games (Uploads)')}`, '    ');
    L.push('');
  }
  wrap('A game started without its files says which it wants, and stops.');
  return L.join('\r\n') + '\r\n';
}
