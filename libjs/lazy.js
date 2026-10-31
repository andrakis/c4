// lazy.js - a disk read on demand, and the page's cache of what it read.
//
// A lazy disk's index.json (src/c4bb/tests/lazy-index.py) names each of
// its files as [name, size], or [name, size, hash] for one worth keeping
// once read: the games' sources, megabytes of which the IDE's Code tab
// shows a file at a time (docs/c4ix-desktop.md, part twenty-one). Nothing is
// read before the machine opens it; then `fetch(name)` -- a synchronous
// read, an XHR in the worker, readFileSync under node -- brings it, once.
// A file with a hash is taken from the cache instead when the cache has
// it under that hash, and otherwise, once fetched, goes to `keep(name,
// hash, bytes)` for the cache. Names not in the index are absent, as on
// an eager disk, and files the machine writes are kept like any other.
// The counts are in stats, for the tests.
export class LazyFiles extends Map {
  constructor(index, fetch, keep = null) {
    super();
    this.fetch = fetch;
    this.keep = keep;
    this.sizes = new Map();
    this.hashes = new Map();
    for (const [n, s, h] of index) { this.sizes.set(n, s); if (h) this.hashes.set(n, h); }
    this.cache = new Map();             // name -> bytes, what the cache had
    this.fetched = 0;
    this.fetchedBytes = 0;
    this.cached = 0;                    // files read from the cache
  }
  // what the cache has, [[name, hash, bytes]]: a file whose hash is no
  // longer the index's is left out
  fill(files) {
    for (const [n, h, b] of files) if (this.hashes.get(n) === h && !super.has(n)) this.cache.set(n, b);
  }
  has(name) { return super.has(name) || this.sizes.has(name); }
  get(name) {
    if (super.has(name)) return super.get(name);
    if (!this.sizes.has(name)) return undefined;
    let b = this.cache.get(name);
    if (b) { this.cache.delete(name); this.cached++; }
    else {
      b = this.fetch(name);
      if (!b) { this.sizes.delete(name); return undefined; }
      this.fetched++; this.fetchedBytes += b.length;
      const h = this.hashes.get(name);
      if (h && this.keep) this.keep(name, h, b);
    }
    super.set(name, b);
    return b;
  }
  *keys() { const seen = new Set(super.keys()); yield* seen; for (const k of this.sizes.keys()) if (!seen.has(k)) yield k; }
}

// The cache, in a store (src/c4bb/web/store.js: IndexedDB in the page):
// one medium per disk, a file in it named "HASH NAME". loadCache gives
// what of it the index still names with the same hash, and takes the
// rest out of the store; a file that changed is read again and kept
// under its new hash.
export const cacheMedium = disk => `lazy ${disk}`;

export async function loadCache(backend, medium, index) {
  const want = new Map();
  for (const [n, , h] of index) if (h) want.set(n, h);
  const files = [];
  if (!want.size) return files;
  for (const f of await backend.listFiles(medium)) {
    const sp = f.name.indexOf(' ');
    const h = f.name.slice(0, sp), n = f.name.slice(sp + 1);
    if (want.get(n) === h) files.push([n, h, f.data]);
    else await backend.deleteFile(medium, f.name);
  }
  return files;
}

export function keepInCache(backend, medium, name, hash, bytes) {
  return backend.putFile(medium, `${hash} ${name}`, bytes);
}
