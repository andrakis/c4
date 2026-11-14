Building and testing on Windows
===============================

c4 builds and runs its whole suite natively on Windows, under MSYS2. That
was done for Cyron, the Windows machine that hosts helios: helios is
deliberately small (4 GB) so that the rest of Cyron's memory stays free,
and the suite, which takes about 5 hours there one test at a time, can
run on Cyron itself and use its memory only while it runs.

The tree is the same one that builds on Linux. Nothing below forks it:
each Windows difference is either an MSYS2 tool or a few lines that
apply only when the host is not Linux.


Why MSYS2
---------

- **WSL1** cannot execute 32-bit ELF programs (c4sp32, c4m32 and the other
  32-bit tools) or node 24+. WSL2 or a VM would keep memory reserved.
- **MSYS2's own gcc** builds 64-bit POSIX programs (on the Cygwin runtime):
  `long` is 64 bits, files are bytes, `fork`, signals and `/proc` exist.
  So c4m, c4sp, c4cc and the rest build as they do on Linux.
- **MinGW's i686 gcc**, also from MSYS2, builds the 32-bit tools, since
  MSYS2 has no 32-bit target of its own (neither has Cygwin any more).
- Git Bash has no make and no gcc; it is a cut-down MSYS2.


Setting up
----------

1. Install MSYS2 from <https://www.msys2.org/> to `C:\msys64`. It installs
   unattended with `msys2-x86_64-latest.exe in --confirm-command
   --accept-messages --root C:/msys64`. Its first run refreshes pacman's
   keys from a keyserver; if that hangs, ending that `pacman-key
   --refresh-keys` does no harm.
2. In the **MSYS2 MSYS** shell, `pacman -Syu` (twice, if it asks to be
   restarted), then:

       pacman -S --needed make gcc diffutils zip unzip vim time python \
           bzip2 perl mingw-w64-i686-gcc mingw-w64-x86_64-python-pillow

   | package | for |
   |---|---|
   | make, gcc | the build, and the 64-bit host programs |
   | mingw-w64-i686-gcc | the 32-bit host programs (every `gcc -m32`) |
   | diffutils | cmp and diff |
   | zip, unzip, bzip2 | packaging and the game data |
   | vim | xxd |
   | time | GNU time, which `make suite` reads CPU time from |
   | python, perl | python3 and perl in the build scripts |
   | mingw-w64-x86_64-python-pillow | games/sbk's asset generator, which needs Pillow (below) |

   `procps-ng` is worth having too: its `top` and `ps` see the build's
   processes, the native ones MSYS2 started included.

   coreutils (timeout, stdbuf, sha256sum), sed, gawk, grep and tar come
   with MSYS2's base.
3. **node** is Windows' own (26.3 or later): MSYS2's is no newer and no
   different. MSYS2 has to see it on the PATH, so start the shell with
   `MSYS2_PATH_TYPE=inherit`, or put node's directory on it in `~/.bashrc`.
4. **Clone without line-ending conversion.** Git for Windows turns LF into
   CRLF on checkout by default, and c4's sources, test references and
   images must stay byte for byte as committed:

       git clone -c core.autocrlf=false --recurse-submodules ...

   The game data (`games/*/data`) is not in git: copy it from a machine
   that has it, as `games/README.md` says.

   NTFS ignores case, and one upstream tree has two names that differ
   only by it: games/bsdgames' `tests/morse.SOS` and `tests/morse.sos`.
   Git checks out one of them, and that submodule then shows a change.
   No c4 test reads either.


Running
-------

From the MSYS2 MSYS shell, in the tree, as on Linux:

	make                  # everything
	make test             # the core suite
	make suite            # the whole suite, packed (tools/suite.mjs)

To leave the rest of the machine its processors, run it at low priority
and give the packer a share of them; from cmd:

	start "" /low /b C:\msys64\usr\bin\env.exe MSYSTEM=MSYS CHERE_INVOKING=1 ^
	    MSYS2_PATH_TYPE=inherit /usr/bin/bash -lc "node tools/suite.mjs --cpus 8 -j 8 --build-jobs 8"

A process started under ssh dies with its session on Windows; one
started through WMI (`Invoke-CimMethod Win32_Process -MethodName Create`,
with a `Win32_ProcessStartup` whose `PriorityClass` is 64, Idle) does not.

To stop a run, kill it from MSYS2 (`kill`, by the pids `ps` shows), not
with `taskkill /T` alone: Windows' process tree breaks at every MSYS2
exec, so taskkill takes the top and leaves the makes under it running.


What differs on Windows, and where
----------------------------------

- **`gcc -m32`** goes to MinGW's i686 gcc: the Makefile puts
  [tools/msys2](../tools/msys2/) first on the PATH when `uname -s` says
  MSYS2 or Cygwin, and its `gcc` hands any `-m32` build to
  `/mingw32/bin/gcc -static`, with -m32 taken out. That covers the
  Makefile, the games' `c4.mk` and their scripts without changing them.
  Every other gcc is MSYS2's, unchanged.
- **The games' host builds.** Each game's test holds its c4 builds to
  what a native 32-bit build of it (`<game>-host`) prints. Under MinGW
  that build defines `_WIN32` without being MSVC, and MinGW's headers
  declare names Linux's do not: games/duke3d needed a commit of its own
  for both (its `windows-suite` branch). The BSD games and PDCurses need
  POSIX that MinGW has not got (termios, `pwd.h`, `sys/wait.h`), so on
  Windows the Makefile builds their hosts with MSYS2's gcc, 64-bit; they
  are written for any Unix, and what the tests compare is what they print.
- **MinGW's own differences** are made up by
  [tools/msys2/mingw.h](../tools/msys2/mingw.h), included first in every
  such build: files and stdin, stdout and stderr opened as bytes (MinGW
  defaults to text, which turns `\n` into `\r\n`), C99's printf rather
  than msvcrt's, and the few names it lacks (`SIGRTMAX`, `S_IRUSR`, BSD's
  `u_int64_t`). It includes no header, because each would declare names
  (`read`, `filelength`) that some program defines for itself. c4's
  headers that reach the host's by `/usr/include/...` reach MinGW's by
  `#include_next`.
- **python3** is MSYS2's, except for a script that imports Pillow:
  MSYS2 has Pillow only for its MinGW python (its own has no FreeType),
  so [tools/msys2/python3](../tools/msys2/python3) runs such a script
  (games/sbk's `mkassets.py`) under MinGW's, through
  [lf.py](../tools/msys2/lf.py), which makes it write `\n` as Linux does.
  sbk's generated assets come out byte for byte as on helios.
- **open()'s flags.** Every guest program is compiled with Linux's numbers
  for them (include/fcntl.h), and the VM's OPEN hands them to the host.
  Windows numbers them otherwise: under MSYS2 `O_CREAT` is Linux's
  `O_TRUNC`. c4m renumbers them (`c4m_oflags`, from host values that only
  `include/c4m_util.h` sets, so that plain c4 still compiles c4m.c), and
  gives a file it creates a mode. Native code that wrote `open(path, 577,
  384)` now says `O_WRONLY | O_CREAT | O_TRUNC | O_BINARY`.
- **`include/stddef.h`** no longer guards the host's stddef.h: MSYS2's
  headers include it again for one type at a time (`__need_wint_t`).
- **node** is a Windows program. `new URL(rel, import.meta.url).pathname`
  is `/C:/...` there, so the node scripts use `fileURLToPath`. The paths
  node is given are relative, which both sides read alike.
- **Processes cost.** A fork takes tens of milliseconds under MSYS2, so a
  shell loop that starts a few processes per file is slow: the ladder
  disk's (climb-disks.sh) started twenty a file and took half an hour.
  Its helpers now set variables instead of answering through `$(...)`.
- **`make suite`** measures memory without Linux's `/proc`: node is not an
  MSYS2 program and cannot read MSYS2's, and Windows' own parent links
  break at every MSYS2 exec. tools/suite.mjs keeps one MSYS2 shell that
  reads MSYS2's /proc each second (each process's parent, resident pages,
  which are 64K there, and Windows pid), and asks node for the memory
  available.
