# src/c4dos/fs — files copied onto the C4DOS floppies

The source copies of the configuration, batch files and manifests that the
Makefile puts onto the generated floppy directories (`c4dos-disk`,
`c4dos-disk32`, `c4dos-build`, `c4dos-build32`, `c4dos-c4ix32`,
`c4dos-c4fc` at the repo root). They are copied under lower-case names
(`config.sys`, `build.bat`, …) because a floppy's `c4dos.dir` listing is
also C4DOS's name resolver, and it must match the real file names. Some
floppies get extra lines appended to `config.sys` (`DEVICE=CONSOLE.SYS
FLUSH`, `DEVICE=DRIVES.SYS`) or a larger `SIZE=`.

## Formats

- **`CONFIG.SYS`**: one directive per line; `REM` lines are comments.
  C4DOS acts on `DEVICE=CLOCK.SYS`, `DEVICE=RAMDISK.SYS SIZE=n`,
  `DEVICE=CONSOLE.SYS FLUSH` and `DEVICE=DRIVES.SYS`, and ignores `FILES=`,
  `SHELL=` and any other `DEVICE=` line.
- **`.BAT`**: one command per line, exactly as typed at `A>`; `@ECHO OFF`,
  `ECHO`, `REM`. A command line holds at most 16 tokens.
- **Manifests** (`install.lst`, and `src/c4ke/fs/c4ix.lst` one rung up):
  one file name per line; `boot NAME` names the image the firmware loads,
  a leading `?` means optional, `#` starts a comment. Each name is fetched
  from the RAM disk first, then the boot disk.
- **Response files** (`c4ix.objs`): one argument per line, `#` comments,
  passed as `@c4ix.objs`.

## Files

| file | on which floppies | what |
|---|---|---|
| `CONFIG.SYS` | all | clock and a 2 MB RAM disk |
| `AUTOEXEC.BAT` | all except `c4dos-c4fc` | greeting and `VER` |
| `BUILD.BAT` | `c4dos-build`, `c4dos-build32` | unpack `c4ke-src.tar`, preprocess `c4ke.c` with `cpp`, compile the kernel and `init.c4r` with `c4cc`. Then `RUN dosload.c4r c4ke.c4r` boots it |
| `LADDER.BAT` | `c4dos-build`, `c4dos-build32`, `c4dos-c4ix32` | the bootstrap: `c4cc` builds `c4cc2`, which builds `cpp2` and itself again (`c4cc3`, the fixed-point check), then those tools build the kernel and init; `c4ke0.c4r` is the same kernel from the shipped compiler, for comparison |
| `BUILDC4M.BAT` | `c4dos-build` | `BUILD.BAT` with `c4m.c4r` under the compiler, for a machine with only the original c4 opcodes |
| `C4M-C4KE.BAT` | `c4dos-build` | boot C4KE under `c4m.c4r` from inside C4DOS (the host's `run-c4`, moved into the machine) |
| `IX.BAT` | `c4dos-c4ix32` | unpack `c4ix-src.tar`, compile C4IX's twelve modules with `c4sc.c4r`, link through `@c4ix.objs` |
| `c4ix.objs` | `c4dos-c4ix32` | the twelve C4IX objects in link order |
| `install.lst` | `c4dos-c4ix32` | what `RUN install.c4r B:` writes to make a bootable C4KE medium |
| `cc.f` | `c4dos-c4fc` | the C4DOS front end for c4fc: defines `CC` and `OBJ`; edit its last lines to choose the file to compile |
| `CC.BAT` | `c4dos-c4fc` | stale, see below |

`CC.BAT` loads the compiler as sixteen separate Forth files. The
`c4dos-c4fc` floppy carries them concatenated as one `c4fc.f` (sixteen
names do not fit on a C4DOS command line), and the list in `CC.BAT` lacks
`inline.f`, which the Makefile's `C4FC_FORTH` now includes. The working
command is `RUN c4th.c4r c4fc.f cc.f`, which is what `make test-c4dos-c4fc`
types.

See [../README.md](../README.md) for the floppy targets and
[docs/climbing-the-ladder.md](../../../docs/climbing-the-ladder.md) for a
session that uses `LADDER` and `IX`.
