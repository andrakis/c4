// uploads.js - the person's own files (page.js opts.uploads), laid over
// drive 0 before the machine boots. worker.js does this at every boot,
// and test-desktop.mjs the same way, so it is tested as the page has it.
//
// FILES is drive 0's Map (or worker.js's LazyFiles), UPLOADS [[name,
// bytes]]. Each upload is the disk's file of that name, hiding the disk's
// own, so a program opens it by name as if the disk had it. And so that
// each system's listings show them too:
// - C4DOS lists a drive by its c4dos.dir, and finds a name typed in any
//   case there: the uploads join drive 0's listing, so DIR and grin's File
//   Manager show them and DOOM2.WAD finds doom2.wad.
// - C4IX's VFS lists only what its manifest and c4ix.sizes name. Each
//   upload is also on the drive as home/NAME, with a line in c4ix.sizes,
//   and the manifest's `home/: hostdir(home/)` (src/c4bb/fs/c4ix.vfs.txt)
//   makes every such file a lazy file in /home (vfsload.c, load_hostdirs):
//   Files, ls and the shell see them there.

export function layUploads(files, uploads) {
  if (!uploads || !uploads.length) return;
  const ups = uploads.map(([n, b]) => [n, b instanceof Uint8Array ? b : new Uint8Array(b)]);
  for (const [n, b] of ups) files.set(n, b);
  const dirFile = files.get('c4dos.dir');
  if (dirFile) {
    const dir = new TextDecoder().decode(dirFile);
    const listed = new Set(dir.split(/\r?\n/).map(l => l.toLowerCase()));
    const add = ups.map(([n]) => n).filter(n => !listed.has(n.toLowerCase()));
    if (add.length) files.set('c4dos.dir', new TextEncoder().encode(dir.replace(/\n?$/, '\n') + add.join('\n') + '\n'));
  }
  const sizesFile = files.get('c4ix.sizes');
  if (sizesFile) {
    const sizes = new TextDecoder().decode(sizesFile);
    const home = ups.filter(([n]) => !n.includes('/'));
    for (const [n, b] of home) files.set('home/' + n, b);
    files.set('c4ix.sizes', new TextEncoder().encode(sizes.replace(/\n?$/, '\n') + home.map(([n, b]) => `${b.length} home/${n}\n`).join('')));
  }
}
