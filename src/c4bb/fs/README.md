# c4bb/fs — VFS manifests

Each file here describes a file tree that an OS builds in memory at boot.
C4KE's and C4IX's `vfsload` read a manifest from the boot disk and populate
the RAM filesystem (C4KE) or the real directory tree (C4IX) from the files it
names. These are **source**: `images/` is generated and safe to delete, so
the manifests live here and are copied onto each disk.

The grammar is indentation-driven: `NAME = words` definitions, `/: root`,
`dir/:` lines, `name: diskfile` entries, and `each($LIST):` loops.
The prototype `c4ke.vfs.txt` at the repo root documents the full grammar;
`src/c4ke/bin/vfsload.c` and `src/c4ix/user/vfsload.c` implement the parts
each system uses.

| File | Copied to | Describes |
|---|---|---|
| `c4ke.vfs.txt` | `images/disk` by [../tests/build-images.sh](../tests/build-images.sh) | C4KE on the shared disk: binaries plus a `.c` beside each. |
| `c4ke-dev.vfs.txt` | appended to `c4ke.vfs.txt` on `images/c4ke-root` | The developer half: the toolchain that builds C4IX and C4IX's sources, under `/usr/src`. Kept separate so the shared disk's manifest stays small. |
| `c4ix.vfs.txt` | `images/disk` | C4IX on the shared disk: `/bin` and the toolchain. |
| `c4ix-cli.vfs.txt` | the repo root as `c4ix.vfs.txt`, by the Makefile (`make run-c4ix`) | C4IX booted by c4m from the root: the `c4ix-*.c4r` built there, the libraries, and the sources and headers where they are in the tree. No root archive. |
| `c4ke-build.vfs.txt` | the C4DOS build floppy (`c4dos-build`, `c4dos-build32`) as `c4ke.vfs.txt`, by the Makefile | The floppy where C4DOS compiles C4KE: a userland and two source archives. |
| `c4ke-climb.vfs.txt` | the climb disk (`c4dos-c4ix32`) as `c4ke.vfs.txt`, by the Makefile | A C4KE medium that `INSTALL` wrote: binaries plus source archives, no loose `.c` files. |
| `c4ix-climb.vfs.txt` | the climb disk as `c4ix.vfs.txt`, by the Makefile | A C4IX medium that `kinstall` wrote: binaries plus `c4ix-src.tar`. |

## Rules

- A manifest must name only files that are on the disk it describes. A
  missing file is skipped and reported, one line per file, which on an 80x25
  console fills the screen. That is why each disk shape has its own manifest.
- Keep each climb manifest in step with the list that puts files on the
  medium: `src/c4dos/fs/install.lst` for `c4ke-climb.vfs.txt`,
  `src/c4ke/fs/c4ix.lst` for `c4ix-climb.vfs.txt`, and the
  `$(C4DOS_BUILD_DISK)` rule in the Makefile for `c4ke-build.vfs.txt`.
