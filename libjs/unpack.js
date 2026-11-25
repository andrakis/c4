// unpack.js - a file a disk carries as the package it came in: made from
// the package the first time the machine opens it.
//
// id's shareware Quake may be passed on only whole, as it was distributed,
// its licence with it (games/quake/data/README.md). So the site's disks
// carry quake106.zip, not the pak0.pak in it, and beside the zip a recipe,
// pak0.pak.unpack, which says how pak0.pak is made from it. To the
// machine pak0.pak is there: listed where the disk's listings are made
// (src/c4bb/tests/unpacked.py: C4DOS's c4dos.dir, C4IX's c4ix.sizes, the
// page's index.json), and when it is first opened, read from the package,
// checked and kept.
//
// A recipe is JSON, NAME.unpack beside the package, for the file NAME:
//   from     the package, a zip, by its name in the recipe's directory
//   member   the zip's member that holds the file
//   lh5      [offset, packed]: the member is an LHA archive, and the file
//            is its -lh5- member of PACKED bytes at OFFSET (lh5.js)
//   size     the file's size
//   sha256   the file's SHA-256: what is made is refused unless it is this
// Without lh5 the file is the member itself.
//
// fitUnpack(files) fits a drive's files (a Map, or lazy.js's LazyFiles)
// with the files their recipes make, once, when boot.js's createMachine
// is given the drive, so every host has them: cli.js, worker.js, the
// tests. A file is made when first asked for (get), by its name in any
// case, and then kept in FILES like any file read from the disk. On a
// lazy disk it is made where it would have been fetched, so LazyFiles
// counts it and gives it to the page's cache when its index names it
// with a hash (the page then has it at the next visit without making it
// again), and the package, read for it, is let go again. A file of the
// name the disk has (an upload laid over it) is that file, never made.
//
// Making Quake's pak0.pak, 18.7 MB from the 9 MB zip: inflating
// resource.1, decoding the lh5 member and its SHA-256 take under a second
// (games/quake/data/README.md has the numbers), during which the machine
// waits: the disk's OPEN is synchronous. So inflating is fflate's
// (vendor/), not DecompressionStream, whose answer is a promise.
import { unzipSync } from './vendor/fflate.js';
import { unlh5 } from './lh5.js';
import { sha256 } from './sha256.js';

export const RECIPE = '.unpack';

const text = b => new TextDecoder().decode(b);
const now = () => (typeof performance !== 'undefined' ? performance.now() : Date.now());

// The bytes RECIPE (parsed) makes from its package PKG, the zip's bytes.
// Throws if they cannot be made, or are not the recipe's.
export function unpack(recipe, pkg) {
  const zip = unzipSync(pkg, { filter: f => f.name === recipe.member });
  let b = zip[recipe.member];
  if (!b) throw new Error(`no ${recipe.member} in ${recipe.from}`);
  if (recipe.lh5) b = unlh5(b, recipe.lh5[0], recipe.lh5[1], recipe.size);
  if (b.length !== recipe.size) throw new Error(`${b.length} bytes, not ${recipe.size}`);
  const sum = sha256(b);
  if (sum !== recipe.sha256) throw new Error(`its SHA-256 is ${sum}, not ${recipe.sha256}`);
  return b;
}

// FILES, fitted: see above. OPTS.log(message), if given, is told of each
// file made and how long it took; a file that cannot be made is said to
// console.error, and is not there. What was made is in files.unpacked:
// [{ name, from, ms, bytes }].
export function fitUnpack(files, opts = {}) {
  if (!files || typeof files.keys !== 'function') return files;
  const byName = new Map();             // the made file's name, lower case -> its name
  for (const k of files.keys()) {
    if (k.length > RECIPE.length && k.endsWith(RECIPE)) {
      const n = k.slice(0, -RECIPE.length);
      byName.set(n.toLowerCase(), n);
    }
  }
  if (!byName.size) return files;
  const get0 = files.get.bind(files), has0 = files.has.bind(files), keys0 = files.keys.bind(files);
  const resident = n => Map.prototype.has.call(files, n);
  const lazy = typeof files.fetch === 'function' && files.sizes instanceof Map;
  const failed = new Set();
  files.unpacked = [];
  const make = name => {
    if (failed.has(name)) return null;
    const dir = name.includes('/') ? name.slice(0, name.lastIndexOf('/') + 1) : '';
    let from = '?';
    try {
      const t0 = now();
      const r = get0(name + RECIPE);
      if (!r) throw new Error(`no ${name + RECIPE}`);
      const recipe = JSON.parse(text(r));
      from = recipe.from;
      const pkgName = dir + recipe.from, had = resident(pkgName);
      const pkg = get0(pkgName);
      if (!pkg) throw new Error(`no ${pkgName}`);
      const b = unpack(recipe, pkg);
      // a lazy disk fetches the package again if the machine opens it
      if (lazy && !had) Map.prototype.delete.call(files, pkgName);
      const ms = Math.round(now() - t0);
      files.unpacked.push({ name, from: pkgName, ms, bytes: b.length });
      if (opts.log) opts.log(`libjs: ${name} made from ${pkgName} in ${ms} ms (unpack.js)`);
      return b;
    } catch (e) {
      failed.add(name);
      console.error(`libjs: ${name} could not be made from ${dir}${from}: ${e.message} (unpack.js)`);
      return null;
    }
  };
  // a lazy disk's: made where it would have been fetched
  // (and not counted as fetched: LazyFiles counts what fetch gives it)
  if (lazy) {
    const fetch0 = files.fetch;
    files.fetch = n => {
      if (byName.get(n.toLowerCase()) !== n) return fetch0(n);
      const b = make(n);
      if (b) { files.fetched--; files.fetchedBytes -= b.length; }
      return b;
    };
  }
  files.get = n => {
    const b = get0(n);
    if (b !== undefined) return b;
    const real = byName.get(n.toLowerCase());
    if (!real || failed.has(real)) return undefined;
    if (real !== n) {
      const b2 = get0(real);
      if (b2 !== undefined) return b2;
    }
    if (failed.has(real)) return undefined;     // a lazy disk tried, just now
    const made = make(real);
    if (!made) return undefined;
    Map.prototype.set.call(files, real, made);
    return made;
  };
  files.has = n => {
    if (has0(n)) return true;
    const real = byName.get(n.toLowerCase());
    return !!real && !failed.has(real);
  };
  files.keys = function* () {
    const seen = new Set();
    for (const k of keys0()) { seen.add(k); yield k; }
    for (const n of byName.values()) if (!seen.has(n) && !failed.has(n)) yield n;
  };
  return files;
}
