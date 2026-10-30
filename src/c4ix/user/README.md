# src/c4ix/user — C4IX userland and the desktop

Programs that run as C4IX user tasks, behind protected mode. Each
`NAME.c` builds to `c4ix-NAME.c4r`:

    make c4ix-NAME.c4r
    # = c4lc -O -c -I src/c4ix/include user/NAME.c, then c4rlink with libc4ix.c4l

They include `c4ix_user.h` and do all I/O through libc4ix's syscalls
(`uprintf`, `ufprintf`, `uread`, …), so fd 0 and fd 1 are whatever the
shell or the desktop set them to: the console, a RAM file, or a pipe. The
C4IX shell finds a bare command name in the directories its `PATH`
names (`/bin`, `/usr/bin`, `/usr/games/bin`), and then as
`c4ix-NAME.c4r` or `NAME.c4r`.
`C4IX_PROGS` in the Makefile is the set the tests build.

## Programs

| file | what |
|---|---|
| `sh.c` | `c4ix-sh`, the shell: pipelines of any length, `<` and `>`, `&` jobs, builtins `exit jobs wait kill cd pwd rm mv help` (`wait PID` waits for any task), and `PATH=DIR:DIR...`, where a bare name is looked for. No fork: it points its own fd 0/1 at a stage, spawns, and restores them. `-p` puts the prompt on the input line (the Terminal passes it); `-e CMD` runs a line first, as if typed |
| `ls.c`, `cat.c`, `echo.c`, `wc.c`, `mkdir.c` | the usual; `cat` and `wc` read fd 0 when given no file |
| `ps.c` | task table, one fixed-shape record per syscall |
| `top.c` | samples the task table twice and reports what each task did in between |
| `spin.c` | burns cycles with progress output, for Ctrl-C and preemption; takes a tick count (default 20) |
| `bench.c` | the pi/factorial workload of `src/bench/bench.c` plus OS microbenchmarks (used by `make bench-c4ix`) |
| `cycles.c` | prints the VM cycle counter first thing; the counterpart of `src/tests/cycles.c` under C4KE |
| `fmt.c` | prints every format case through both `printf` paths (kernel-trapped and libc4ix) for `make test-c4ix-fmt` |
| `hello.c` | calls `printf` directly, no library: a trapped host opcode the kernel redirects. Built alone as `c4ix-hello.c4r` |
| `uhello.c` | the same greeting through libc4ix only |
| `gui.c` | runs `libjs/guest/gui-demo.c` as a C4IX task; prints `gui: not fitted` without the libjs display |
| `vfsload.c` | `c4ix-vfsload`: fills the hierarchical VFS from `c4ix.vfs.txt` at boot (same manifest format as `src/c4ke/bin/vfsload.c`), then starts `c4ix-tar` in the background on each archive the manifest's `ARCHIVES` names and prints its task id |
| `tar.c` | `tar t[v] FILE`, `tar x[v] [-C DIR] FILE`: ustar or C4TAR1, gzipped or not (recognised by its magic). Links `libarc.c4l` too |
| `b4ke.c` | the build tool, native to C4IX: C4KE's B4KE1 files, plus B4KE2's `SET`/`$(NAME)`, `CD`, `MKDIR`, `DEPS` (rebuild only what is older than its sources), `COPY`, `RUN ... > FILE` and sub-builds (`B4KE DIR`). `b4ke -C /usr` rebuilds everything C4IX ships |
| `cc.c` | `cc [-O] [-c] [-conforming] [-fno-builtin] [-fpic] [-mfuse] [-v] [-stack BYTES] [-I DIR] [-D NAME] [-l LIB] [-o OUT] FILE.c ...`: compiles with c4sc (c4lc, compiled) and c4lc's preprocessor, on a task's default stack (`-stack` for more), searching `/usr/include` (C4IX's headers and the repo's) with `C4CC` and `__c4cc__` defined; with more than one source or a `-l` (`c4ix`, `arc`, or a shared `/lib/NAME.c4s`) it links with c4rlink |
| `crc32.c` | `crc32 FILE ...`: gzip's CRC-32 and the size, to check a build made inside the machine against the host's. Links `libarc.c4l` too |
| `gunzip.c` | `gunzip FILE.gz` writes `FILE`; `-c` writes to fd 1. The gzip CRC is checked before anything is written. Links `libarc.c4l` too |

## Shell scripts

Run by `c4ix-sh` with the script as its argument.

| file | what |
|---|---|
| `test.sh` | the functional suite: C4KE's test coverage re-expressed as shell commands; the sixth `--demo` image, so `make test-c4ix` pins its output |
| `bench.sh` | `bench c4ix-hello.c4r`, for `make bench-c4ix` |
| `demo.sh` | a short tour of redirection and pipelines |
| `stress.sh` | runs under abnormal scheduling pressure; deliberately not pinned by any target |

## desktop/

A Windows NT 4-style desktop, one C4IX task built from seven modules and
linked with libc4ix (`make c4ix-desktop.c4r`; the 32-bit build comes from
`build-images.sh`). It needs the libjs display: start C4IX in the libjs
page and type `desktop`.

| file | what |
|---|---|
| `desktop.h` | what the modules share: screen size (800x600), window kinds, colours, the `d_*` drawing layer |
| `wm.c` | the window manager: the only module that touches the display (`libjs/guest/gui.h`); windows, taskbar with clock, Go menu (including Run…), menu bars, dialogs |
| `ui.c` | stateless widgets drawn through `wm.c` |
| `term.c` | Terminal: a real `c4ix-sh -p` task on pipes, drawn through a VT100 subset, line-edited locally (Left/Right, Home/End, a 16-line history on Up/Down) with 300 rows of scrollback; Ctrl-C interrupts its foreground job |
| `files.c` | Explorer: folder tree and file list over the VFS; double-click runs a program in a new Terminal |
| `taskmgr.c` | System Monitor: Applications, Processes (CPU share over the last second, with System and idle rows, and an Interval column), Performance |
| `apps.c` | Text Editor, Calculator, About |
| `edit.c` | Editor, the IDE: a VIM-mode editor (`:set novim` for a plain one; arrow keys in every mode) that builds what it edits. `:make`/F5 runs b4ke on the nearest `BUILD.B4K` above the file that names it, or has `cc -O -l c4ix` build a `.c` none names into `/tmp/NAME.c4r`; errors are listed under the text and F8 goes to the next. `:run`/F6 runs the result. Explorer opens source files here |
| `explore.c` | the IDE's Code tab (View > Code, F7): the functions, and the C4 code of one with its source lines between the instructions, for a `.c` file (by `cc -g`, following the cursor) or any compiled file opened as it is |

Kernel calls the desktop added (`SYS_AVAIL`, `SYS_CLOEXEC`, `SYS_INTR`)
and the display registers it uses are described in
[docs/c4ix-desktop.md](../../../docs/c4ix-desktop.md). It is tested
headless by `libjs/tests/test-desktop.mjs` (part of `make test-libjs`).
