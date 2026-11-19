// uploads.js - the person's own files (page.js opts.uploads), laid over
// drive 0 before the machine boots. worker.js does this at every boot,
// and test-desktop.mjs the same way, so it is tested as the page has it.
//
// FILES is drive 0's Map (or worker.js's LazyFiles), UPLOADS [[name,
// bytes]]. A name is a file's ("doom2.wad"), or a folder's and a file's
// ("doom/doom2.wad"): one of FOLDERS, below, which app.js's Uploads
// offers. Each upload is the disk's file of that name, hiding the disk's
// own, so a program opens it by name as if the disk had it -- a bare
// game opens "doom/doom2.wad" from drive 0 as it is. And so that each
// system's listings show them too:
// - C4DOS lists a drive by its c4dos.dir, and finds a name typed in any
//   case there: the uploads join drive 0's listing, so DIR and grin's File
//   Manager show them and DOOM2.WAD finds doom2.wad. A folder is a
//   directory there, A:\DOOM, with a c4dos.dir of its own.
// - C4IX's VFS lists only what its manifest and c4ix.sizes name. Each
//   upload is also on the drive as home/NAME, with a line in c4ix.sizes,
//   and the manifest's `home/: hostdir(home/)` (src/c4bb/fs/c4ix.vfs.txt)
//   makes every such file a lazy file in /home (vfsload.c, load_hostdirs),
//   and one in a folder a lazy file in /home/FOLDER: Files, ls and the
//   shell see them there.

// The folders an upload can go in: /home itself, and one for each game
// that plays the person's own files. Each such game looks in its own
// folder first, then in /home (and on the bare machine and under C4DOS
// in the folder at drive 0's root, then at the root): games/*/README.
export const FOLDERS = [
  ['', '/home'],
  ['wolf3d', '/home/wolf3d', 'Wolfenstein 3D: wolf3d6 plays the registered *.WL6 files'],
  ['duke3d', '/home/duke3d', 'Duke Nukem 3D: duke3d -home plays DUKE3D.GRP'],
  ['quake', '/home/quake', 'Quake: quake -home plays pak0.pak and pak1.pak'],
  ['doom', '/home/doom', 'Doom: doom -home plays doom.wad, doom2.wad, plutonia.wad, tnt.wad'],
  ['rott', '/home/rott', 'Rise of the Triad: rottreg plays the registered DARKWAR.WAD, .RTL and .RTC (and REMOTE1.RTS, if you have it: else the shareware\'s)'],
];

// What each of those games wants of the person's files, for the Uploads'
// list (app.js): `all` are needed, one of `any` is, `opt` are used when
// there. And where to start it on each system: C4IX's Go > Games and a
// Terminal, C4DOS's grin (Program Manager's Full Games group, progman.c)
// and its prompt, and the page's own system for the bare machine. Names
// are as uploadName makes them, lower case.
export const NEEDS = [
  { folder: 'wolf3d', game: 'Wolfenstein 3D (registered)',
    all: ['audiohed.wl6', 'audiot.wl6', 'gamemaps.wl6', 'maphead.wl6', 'vgadict.wl6', 'vgagraph.wl6', 'vgahead.wl6', 'vswap.wl6'],
    c4ix: 'wolf3d6', c4dos: 'Full Games > Wolfenstein 3D, or \\GAMES\\WOLF3D\\WOLF3D6', bare: 'wolf3d6' },
  { folder: 'doom', game: 'Doom (The Ultimate Doom, Doom II, Final Doom)',
    any: ['doom.wad', 'doom2.wad', 'tnt.wad', 'plutonia.wad'],
    c4ix: 'doom -home', c4dos: 'Full Games > The Ultimate Doom, Doom II, TNT, Plutonia, or \\GAMES\\DOOM\\DOOM -home', bare: 'doom-full' },
  { folder: 'duke3d', game: 'Duke Nukem 3D (1.3D, 1.4, Atomic 1.5)',
    all: ['duke3d.grp'],
    c4ix: 'duke3d -home', c4dos: 'Full Games > Duke Nukem 3D, or \\GAMES\\DUKE3D\\DUKE3D -home', bare: 'duke3d-full' },
  { folder: 'quake', game: 'Quake (registered)',
    all: ['pak1.pak'], opt: ['pak0.pak'],
    c4ix: 'quake -home', c4dos: 'Full Games > Quake, or \\GAMES\\QUAKE\\QUAKE -home', bare: 'quake-full' },
  { folder: 'rott', game: 'Rise of the Triad (registered)',
    all: ['darkwar.wad', 'darkwar.rtl', 'darkwar.rtc'], opt: ['remote1.rts'],
    c4ix: 'rottreg', c4dos: 'Full Games > Rise of the Triad, or \\GAMES\\ROTT\\ROTTREG', bare: 'rottreg' },
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

export function layUploads(files, uploads) {
  if (!uploads || !uploads.length) return;
  const ups = uploads.map(([n, b]) => [n, b instanceof Uint8Array ? b : new Uint8Array(b)]);
  for (const [n, b] of ups) files.set(n, b);
  const dirFile = files.get('c4dos.dir');
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
