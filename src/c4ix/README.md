# C4IX — a small Unix-like kernel for the c4m VM

C4IX is the top rung of the ladder (C4DOS → C4KE → C4IX). Where C4KE is a
single file in the plain c4 subset, C4IX is written in the C that c4lc
compiles (structs, typedef, block declarations): twelve modules, each
compiled to a `.c4o` object and linked by c4rlink. User programs run behind
c4m's protected mode, so all of their I/O goes through syscalls, and the
kernel owns what C4KE cannot: per-task file descriptors, pipes,
redirection, a hierarchical RAM filesystem and a shell with pipelines.

It also runs C4KE's binaries unmodified, through a compatibility layer
(`c4ke.c`) that answers C4KE's custom opcodes. With the libjs display
fitted it runs a Windows-style desktop ([user/desktop](user/README.md)).
Compilers run inside it: c4cc, c4rlink and c4lc (on c4sp) write what they
build into `/tmp`, and c4th, cpp and c4rdump are installed beside them
(on the disk image `make c4bb-images` builds; see `examples/README.txt`).

## Build

| target | produces |
|---|---|
| `make c4ix.c4r` | the kernel: the twelve modules compiled in parallel by c4lc `-O` (its own preprocessor, no gcc), then `c4rlink` |
| `make libc4ix.c4l` | the userland C library, as a c4rlink library |
| `make libarc.c4l` | gzip (inflate, CRC-32) and tar, for `c4ix-tar.c4r` and `c4ix-gunzip.c4r`, which link it as well |
| `make c4ix-NAME.c4r` | `user/NAME.c` compiled with c4lc and linked with `libc4ix.c4l` (e.g. `c4ix-sh.c4r`, `c4ix-ls.c4r`) |
| `make c4ix-hello.c4r` | `user/hello.c` alone, no library: it calls `printf` directly |
| `make c4ix-desktop.c4r` | the desktop's six modules linked with `libc4ix.c4l` |
| `make c4ix-src.tar` | the kernel sources as a C4TAR1 archive, for building C4IX inside C4KE |

The 32-bit images the breadboard and libjs boot (`c4ix32.c4r` and the
`c4ix-*.c4r` userland) are built by `make c4bb-images`
(`src/c4bb/tests/build-images.sh`).

## Run

    make run-c4ix        # ./c4m load-c4r.c -- c4ix.c4r          (a shell on your terminal)
    make run-c4ix-c4     # ./c4 c4m.c load-c4r.c -- c4ix.c4r     (plain c4 runs c4m; slow)
    make demo-c4ix       # the guided tour, below
    make demo-c4ix-c4
    make bench-c4ix      # C4IX's benchmark and boot cost, next to C4KE's

The kernel's command line, from `c4ix.c4r --help`:

    usage: c4ix.c4r [OPTION] [PROGRAM [ARGUMENT]...]

      (no arguments)     boot to an interactive shell
      PROGRAM [ARG]...   boot, run PROGRAM with ARGs, shut down
      --demo IMAGE...    run the built-in demonstrations, below
      -h, --help         print this and shut down
      -q                 accepted and ignored, so older command
                         lines keep working

`PROGRAM` is a host path and is not expanded; the shell expands a bare
name to `c4ix-NAME.c4r` or `NAME.c4r`. `--demo` takes six images in fixed
roles (a raw-`printf` program, a libc4ix program, a pipeline writer and
reader, a shell and its script); the `demo-c4ix` target passes
`c4ix-hello.c4r c4ix-uhello.c4r c4ix-echo.c4r c4ix-wc.c4r c4ix-sh.c4r
src/c4ix/user/test.sh`.

With no arguments, init runs `c4ix-vfsload.c4r` to fill the filesystem
from `c4ix.vfs.txt` (if present), then starts `c4ix-sh.c4r`. On the
board and libjs disk the manifest also names `c4ix-root.tgz`, which
vfsload hands to `c4ix-tar` in the background: the repo's sources and
docs appear under `/usr` a few seconds after the prompt (`wait 4`
blocks until they have), the C4IX libraries in `/usr/lib`, and more
headers in `/usr/include`.

That tree is enough to rebuild C4IX inside itself. `BUILD.B4K` files sit
beside the sources, `b4ke` and `c4sc` (the compiled c4lc) are in `/bin`,
and

    b4ke -C /usr                   the kernel, libraries, userland, desktop and toolchain, into /build
    b4ke -C /usr/src/c4ix/user     or any one directory
    b4ke -C /usr -f INSTALL.B4K    then /bin and /usr/lib get the results

The archive carries `/build` already built, as these files would
build it, so the first run has nothing to do and an edit remakes only
what it feeds (`-B` rebuilds everything regardless). For one program of
your own, `cc` runs c4sc with c4lc's preprocessor and the stack it
needs -- `cc -O -l c4ix -o /tmp/prog.c4r prog.c` -- about twenty times
faster than `c4sp c4lc.lisp` (src/c4ix/examples/README.txt).

What c4lc builds comes out byte for byte the disk's; c4cc's images carry
its own heap addresses in their symbol records, so they differ there and
nowhere that runs. The whole tree is about 29G instructions (nine minutes
under node). `make test-c4ix-rebuild` checks all of it. Its builtins
are `exit jobs wait kill cd pwd help`; it has `<`, `>`, pipelines and `&`.
`exit` or Ctrl-D shuts the kernel down.

The desktop needs the libjs display, so it runs in the browser or headless
under libjs, not on a terminal:

    make c4bb-images
    make serve-libjs     # http://localhost:8472/libjs/web/

then type `desktop` at the C4IX shell. Without a display it prints
`desktop: not fitted` and exits.

## Test

| target | what | in `make test`? |
|---|---|---|
| `make test-c4ix` | the `--demo` tour on native c4m, masked and compared with `tests/x5-c4m.txt` | no |
| `make test-c4ix-fmt` | the kernel's `printf` and libc4ix's `printf` must agree, and match `tests/fmt.txt` | no |
| `make test-c4ix-c4ke` | C4KE's `ps`, `top`, `bench`, `innerbench`, `spin`, `benchtop` run unmodified as C4IX tasks (`tests/ck-*.txt`) | no |
| `make test-c4ix-c4` | `test-c4ix` with c4m interpreted by plain c4. Slow | no |
| `make test-c4ix-c4ke-nested` | innerbench's default mode: a C4KE compiled from source in a nested c4m, as a C4IX task. Takes minutes | no |
| `make test-libjs` | includes `libjs/tests/test-desktop.mjs`, which drives the desktop headless | no |
| `make test-c4dos-c4ix32` | C4DOS on c4bb builds C4IX with the compiled compiler and boots it. Minutes | no |

`user/stress.sh` is a scheduling stress script and is not pinned by any
target.

## Layout

| path | what |
|---|---|
| `c4ix.h` | kernel-wide declarations; every module includes it and nothing else |
| `boot.c` | entry point and boot sequence |
| `console.c` | `kputc`/`kputs`/`kprintf` and console input |
| `va.c` | the one varargs area for the whole image |
| `host.c` | host detection from one `__c4_info()` call; what tasks are told |
| `sl4b.c` | SL4B, a slab allocator for fixed-size kernel objects |
| `task.c` | the task list; 64 KB stacks |
| `sched.c` | the scheduler: switches inside c4m trap handlers (preemptive via the cycle interrupt), with a cooperative fallback for a host without traps |
| `vfs.c` | vnodes, open file descriptions, per-task fd tables, pipes, directories |
| `sys.c` | the syscall dispatcher: libc4ix's calls and trapped host opcodes both end here |
| `c4ke.c` | the C4KE compatibility layer: answers `u0.h`'s custom opcodes, including the RAM-filesystem ones |
| `loader.c` | the `.c4r` loader; prefers a RAM file to the host file of the same name |
| `init.c` | the first task and the kernel command line (`--demo`, `--help`) |
| `include/c4ix_user.h` | the userland API (syscalls, `uprintf`, …) |
| `include/window.h` | lets a program in a desktop Command Prompt use that window as a canvas |
| `lib/libc4ix.c` | the userland library |
| `libc/` | [C4IX's C library](libc/README.md), the shared library `/lib/libc.c4s` |
| `lib/archive.c`, `include/archive.h` | libarc: an RFC 1951 inflater (puff-style, with a 9-bit lookup table), gzip with its CRC, and a ustar/C4TAR1 reader that makes directories as it goes |
| [`user/`](user/README.md) | userland programs, shell scripts, and the desktop |
| `examples/` | `hello.c`, `bounce.c` and `README.txt`: programs to compile inside C4IX (`c4cc -o /tmp/hello.c4r /usr/src/c4ix/examples/hello.c`) |
| `tests/` | goldens for `test-c4ix`, `test-c4ix-fmt`, `test-c4ix-c4ke` |

## Hosts

- **Native c4m**: the normal host. Preemption through the cycle interrupt;
  user tasks run in protected mode, so a raw `printf` in a user program
  traps and the kernel formats it onto the task's fd 1.
- **Plain c4**: C4IX images use opcodes the base VM does not have (the
  Makefile names `C4CY`, `PUTC`, `JSRI`, `JSRS`), so the supported route is
  c4 → c4m.c → C4IX (`make run-c4ix-c4`, `make test-c4ix-c4`), where C4IX
  sees a c4m host and behaves identically. The scheduler also has a
  cooperative backend for a host whose `__c4_info()` reports no traps;
  `init` then skips the preemption demo. On Windows, plain c4 has no
  `/proc/uptime` clock; a natively built c4m does not have that problem.
- **32-bit / c4bb**: `c4ix32.c4r` boots on the breadboard, with the drives
  and, via C4DOS and C4KE, the whole climb
  ([docs/climbing-the-ladder.md](../../docs/climbing-the-ladder.md)).
- **libjs**: the only host with the display, so the only one that runs
  `c4ix-gui.c4r` and the desktop ([libjs/README.md](../../libjs/README.md)).

## Limits and known issues

- libc4ix's `printf` takes no width specifiers (`user/ps.c` pads by hand).
- fd tables have 16 slots.
- A race not yet root-caused intermittently corrupts one entry when
  `c4ix-vfsload` fills the tree on c4bb, so `test-c4bb` only requires most
  entries to load (see `user/vfsload.c` and `src/c4bb/tests/test-c4bb.sh`).
- The header comments of `boot.c` and `c4ix.h` predate the current build:
  they describe a plain-c4 mode through the c4l loader and a `gcc -E` pass
  before c4lc. The Makefile builds with c4lc's own preprocessor and names
  c4 → c4m.c as the plain-c4 route.

## Docs

- [docs/c4ix-design.md](../../docs/c4ix-design.md): the kernel's design
  and milestones X0–X5. It has not been kept up to date since 2026-08-09;
  later work (the desktop, compilers inside, the RAM-file loader
  preference) is not in it.
- [docs/c4ix-desktop.md](../../docs/c4ix-desktop.md): the desktop, its
  kernel additions and its tracker. Current.
- [docs/internals.md](../../docs/internals.md) §7.4: what C4IX does with
  protected mode.
- [docs/climbing-the-ladder.md](../../docs/climbing-the-ladder.md): C4KE
  building C4IX on the breadboard.
