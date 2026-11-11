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
  ['rott', '/home/rott', 'Rise of the Triad: rottreg plays the registered DARKWAR.WAD, .RTL and .RTC'],
];

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
