// kept.js - the files a system keeps in this browser (its store: drive 1,
// page.js opts.persist), as the person sees them, for app.js's Saved
// files to list and download. Pure functions of the store's contents, so
// node tests them as the page uses them (tests/test-kept.mjs).
//
// What a store holds depends on who wrote it:
// - the bare games and C4DOS write their files to drive 1 by name
//   (libc's bare.c, C4DOS's home drive): "savegam0.wl6", "DOOM/x.dsg".
//   Each is a file of the same name. C4DOS's directory listings
//   (c4dos.dir) are its own bookkeeping, not shown.
// - C4IX keeps every change to its filesystem in one tar
//   (src/c4ix/user/persist.c): c4ix-persist.tar or c4ix-persist-b.tar,
//   whichever c4ix-persist.cur names ("a" or "b"; none: the first), and
//   the system itself in c4ix-base.tar. The person's files are the
//   current tar's, by their paths in C4IX ("/usr/games/wolf3d/
//   savegam0.wl6"), but for persist's own (/var/persist). They are what
//   the last save kept: the desktop saves a minute after a change, and
//   when it is shut down; C4IX's own download (the Files' File >
//   Download) hands over a file as it is now.

const text = b => new TextDecoder().decode(b);

// The ustar archive TAR's regular files and directories, [[path, bytes]]
// and [path], paths without a leading slash. A tar this does not read
// (no "ustar" magic) is empty.
export function untar(tar) {
  const files = [], dirs = [];
  const str = (at, n) => { let e = at; while (e < at + n && tar[e]) e++; return text(tar.subarray(at, e)); };
  for (let at = 0; at + 512 <= tar.length && tar[at];) {
    if (str(at + 257, 5) !== 'ustar') break;
    const name = (tar[at + 345] ? str(at + 345, 155) + '/' : '') + str(at, 100);
    const size = parseInt(str(at + 124, 12).trim() || '0', 8) || 0;
    const type = String.fromCharCode(tar[at + 156] || 48);
    const body = at + 512;
    if (type === '0' || type === '\0') files.push([name, tar.subarray(body, body + size)]);
    else if (type === '5') dirs.push(name.replace(/\/$/, ''));
    at = body + Math.ceil(size / 512) * 512;
  }
  return { files, dirs };
}

// A ustar archive of FILES, [[path, bytes]] (paths relative, '/' between
// their parts), with an entry for each directory they are in, as C4IX's
// download writes one (persist.c, dl_tree).
export function tar(files) {
  const enc = new TextEncoder();
  const blocks = [];
  const header = (path, size, dir) => {
    const h = new Uint8Array(512);
    let name = enc.encode(path), prefix = new Uint8Array(0);
    if (name.length > 100) {
      // the prefix ends at a slash, and what follows fits in 100
      let cut = -1;
      for (let i = path.indexOf('/'); i >= 0 && i <= 155; i = path.indexOf('/', i + 1))
        if (enc.encode(path.slice(i + 1)).length <= 100) { cut = i; break; }
      if (cut < 0) return null;
      prefix = enc.encode(path.slice(0, cut));
      name = enc.encode(path.slice(cut + 1));
    }
    const oct = (at, len, v) => h.set(enc.encode(v.toString(8).padStart(len - 1, '0')), at);
    h.set(name, 0);
    oct(100, 8, dir ? 0o755 : 0o644);
    oct(108, 8, 0); oct(116, 8, 0);
    oct(124, 12, dir ? 0 : size);
    oct(136, 12, Math.floor(Date.now() / 1000));
    h[156] = dir ? 53 : 48;
    h.set(enc.encode('ustar\x0000'), 257);
    h.set(prefix, 345);
    h.fill(32, 148, 156);
    let sum = 0;
    for (const b of h) sum += b;
    h.set(enc.encode(sum.toString(8).padStart(6, '0') + '\0 '), 148);
    return h;
  };
  const made = new Set();
  const dirOf = path => {
    const parts = path.split('/');
    for (let i = 1; i < parts.length; i++) {
      const d = parts.slice(0, i).join('/') + '/';
      if (made.has(d)) continue;
      made.add(d);
      const h = header(d, 0, true);
      if (h) blocks.push(h);
    }
  };
  for (const [path, bytes] of files) {
    dirOf(path);
    const h = header(path, bytes.length, false);
    if (!h) continue;
    blocks.push(h, bytes);
    if (bytes.length % 512) blocks.push(new Uint8Array(512 - bytes.length % 512));
  }
  blocks.push(new Uint8Array(1024));
  const out = new Uint8Array(blocks.reduce((n, b) => n + b.length, 0));
  let at = 0;
  for (const b of blocks) { out.set(b, at); at += b.length; }
  return out;
}

// The person's files in STORE (a Map, or [[name, bytes]], as the page's
// store loads), [[path, bytes]] sorted by path: absolute ("/usr/...") for
// C4IX's, the store's own names for the rest.
export function keptFiles(store) {
  const m = store instanceof Map ? store : new Map(store);
  const has = n => m.has(n);
  if (has('c4ix-persist.tar') || has('c4ix-persist-b.tar') || has('c4ix-base.tar')) {
    const cur = has('c4ix-persist.cur') ? text(m.get('c4ix-persist.cur')).trim() : 'a';
    const t = m.get(cur === 'b' ? 'c4ix-persist-b.tar' : 'c4ix-persist.tar');
    if (!t) return [];
    return untar(t).files.map(([n, b]) => ['/' + n, b])
      .filter(([n]) => !n.startsWith('/var/persist/'))
      .sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0));
  }
  return [...m].filter(([n]) => !/(^|\/)c4dos\.dir$/i.test(n))
    .sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0));
}

// The folders FILES are in ("/usr/games/wolf3d", "DOOM", "" the top), each
// with what is in it, under it too: [[folder, [[path, bytes]]]], the top
// first and each folder before those in it
export function keptFolders(files) {
  const folders = new Map();
  for (const f of files) {
    const p = f[0], parts = p.split('/');
    // "/usr/x": /usr, then the top (""); "DOOM/x": DOOM, then the top
    for (let i = parts.length - 1; i >= (p.startsWith('/') ? 1 : 0); i--) {
      const d = parts.slice(0, i).join('/');
      if (!folders.has(d)) folders.set(d, []);
      folders.get(d).push(f);
    }
  }
  return [...folders].sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0));
}

// FILES (from keptFiles) under FOLDER as an archive, each named from the
// folder's own last part down ("wolf3d/savegam0.wl6"), and its file name
// ("wolf3d.tar"; the top's is NAME.tar)
export function folderTar(files, folder, name) {
  const base = folder.replace(/\/$/, '').split('/').pop() || name;
  const cut = folder ? folder.length + 1 : 0;
  const inside = files.filter(([p]) => !folder || p.startsWith(folder + '/'))
    .map(([p, b]) => [base + '/' + p.slice(cut).replace(/^\//, ''), b]);
  return { name: base + '.tar', bytes: tar(inside) };
}
