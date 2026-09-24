# The C4IX desktop: windows, and a terminal emulator in them

Status tracker and design. Tick a box only on green evidence, and write
the evidence next to it.

## What it is

`desktop` is a C4IX user program that takes over the libjs display
(docs/libjs-design.md) and draws a Windows 95-style desktop on it: a teal
background, bevelled grey windows with navy title bars that can be
dragged, focused, minimised, maximised and closed, and a taskbar with a
Start menu, a button per window and a clock.

The first application is a terminal. Each terminal window runs a real
`c4ix-sh` as its own task, its stdin and stdout pipes that the desktop
holds. This is what C4IX's fd layer was built for: the shell and every
program it starts read and write fds 0 and 1, and the kernel decides
where those go. C4KE cannot do this; its programs print with host
opcodes that go straight to the console.

The terminal draws what the shell writes through a VT100 subset
(cursor movement, erase, SGR colours), and edits lines locally the way a
tty in cooked mode does: typed characters are echoed, Backspace erases,
and a line goes down the pipe when Enter is pressed. Ctrl-C interrupts
that window's foreground job.

One process owns the display. Separate programs opening their own windows
(a window-server protocol over pipes) is a later step; the window code is
shaped so it can be added.

## Design

**Kernel additions** (src/c4ix):
- `SYS_AVAIL` (221): bytes waiting on a descriptor, without blocking: 0
  when a pipe is empty but still has a writer, -1 at end of file. The
  desktop polls every terminal's output pipe from one loop, so it must
  never park on one of them. libc4ix: `uavail(fd)`.
- `SYS_CLOEXEC` (223): mark a descriptor close-on-spawn. Spawn clones the
  whole fd table, so without this every terminal's shell would hold the
  desktop's end of its own stdin (and never see end of file) and of every
  other terminal's pipes, and the 16-slot tables would fill. Per fd, not
  per description: a dup of a marked fd is inherited. libc4ix:
  `ucloexec(fd, on)`.
- `SYS_INTR` (222): interrupt the foreground job below a given task,
  found the way the console finds its own (descend the wait chain from
  that task). A terminal window uses its shell's pid. libc4ix:
  `uintr(pid)`.

**Display additions**: a CLOCK register (0x440, host local seconds since
midnight) for the taskbar clock, the window growing to 0x47F; and `TEXT2 [x, y, rgb, size, font, advance, n, chars]`.
Font 0 is monospace, 1 a sans-serif UI face, 2 its bold. A non-zero
advance places each character exactly `advance` pixels after the last,
which is what a character grid needs; 0 is the font's own spacing.

**The desktop** (src/c4ix/user/desktop.c -> c4ix-desktop.c4r):
- Asks for an 800x600 display (1024x768 was too small to read once the
  page scaled it into its pane), attaches the rings, and redraws the whole
  scene when something changed. Idle, it sleeps (`umsleep`).
- Windows are a z-ordered list. Title bar drag moves, a click raises and
  focuses, the three caption buttons minimise, maximise/restore and close.
- The taskbar has a Start button (menu: New Terminal, About, Shut Down),
  a button per window, and a clock.
- A terminal window is 80x24 cells of 8x16 pixels. Closing it ends its
  shell (SIGTERM) and the desktop reaps it.

## Tracker

### D0: kernel
- [x] `SYS_AVAIL`, `SYS_INTR`, `SYS_CLOEXEC`; libc4ix `uavail`, `uintr`,
      `ucloexec` (`SYS_TOP` is now 224)
- [x] `make test-c4ix` and `test-c4ix-c4ke` green (2026-09-23). x5 re-pinned
      for one line: the task struct's slab size, 432 -> 560, which is the
      sixteen `fdcloexec` words

### D1: display
- [x] `TEXT2` in gui-device/display.js and `gui_text2` in gui.h; CLOCK

### D2: the desktop shell
- [x] Windows: draw, raise, focus, drag, minimise, maximise, close
- [x] Taskbar: Start menu, window buttons, clock
- [x] Headless test drives it with display events (`libjs/tests/test-desktop.mjs`,
      in `make test-libjs`)

### D3: the terminal
- [x] A shell per window over pipes; output through the VT100 subset
- [x] Local line editing, Enter sends the line, Ctrl-C interrupts
- [x] Headless test (2026-09-23): `ps` typed in a window runs in that
      window's shell and lands in that window; a second terminal from the
      Start menu; `spin` there and Ctrl-C shows `^C` and stops it while the
      first terminal still answers; dragging moves a window; closing one
      removes it; Shut Down returns to the console, and no terminal shell
      outlives the desktop. C4IX's vfs pins widened to `[4-9]x/[4-9]x`
      entries (now 50/50) in test-libjs and test-c4bb.

### D4: in the browser
- [x] CDP gate (`test-web.mjs --gui`, 3060 Ti, 2026-09-23): `desktop` from
      the C4IX shell; the display becomes 800x600; a terminal shows the
      prompt; `ps` typed on the display runs in that window; Start opens its
      menu and New Terminal opens a second window; a window drags by its
      title bar; its close button closes it; Shut Down hands the console
      back; no page errors. The strings drawn are read back through the
      display's opt-in text log (`display.recordText`).
- [x] Screenshot checked by eye. At the user's request the desktop is
      800x600 (1024x768 was scaled down into the pane and hard to read), and
      the page gives the display its natural width when it has room, so
      it is shown pixel for pixel.

Also fixed on the way: `build-images.sh` rebuilt C4IX only when one of
four kernel files changed, so edits to `sys.c`, `vfs.c`, libc4ix or any
user program were silently not built. It now rebuilds when anything under
`src/c4ix` (or `libjs/guest/gui.h`) is newer than the kernel image.

## Part two: NT-style tools (asked for 2026-09-23)

Explorer, Task Manager, Notepad, Calculator and Run, in the Windows NT 4
style, all inside the desktop process. The desktop becomes several
modules linked together (`src/c4ix/user/desktop/`): C4IX's compiler is
given a fixed memory budget per file, and one file holding all of this
would not fit it.

**Kernel**: `SYS_STAT` (224: type and size of a path), `SYS_UNLINK` (225:
remove a file, or an empty directory) and `SYS_RENAME` (226: give an
entry a new name in the same directory). libc4ix: `ustat`, `uunlink`,
`urename`.

**Shared pieces**: a drawing layer every window draws through, clipped to
its client area; widgets (buttons, list views with columns and
selection, scroll bars, a menu bar with drop-down menus, text fields,
tabs); a message box; resizable windows (drag the bottom-right corner).

- **Explorer**: a folder tree next to a file list (Name, Size, Type), a
  menu bar, an address box with Up, a status bar. Double-click opens a
  folder, runs a program in a new Command Prompt, or opens anything else
  in Notepad. New Folder, Delete (confirmed), Rename.
- **Task Manager** (Ctrl+Shift+Esc too): Applications (the desktop's
  windows, End Task, Switch To), Processes (every task with PID, CPU %,
  CPU time in cycles, syscalls and state, End Process), Performance (a
  CPU gauge and a scrolling history graph, totals).
- **Notepad**: open, edit and save text files in C4IX's filesystem;
  keyboard editing, mouse placement, scroll bar and wheel.
- **Calculator**: 32-bit integers in Hex, Dec, Oct or Bin, with + - * /,
  Mod, And, Or, Xor, Not, Lsh, Rsh.
- **Run**: Start > Run... starts a program in a new Command Prompt.

### D5: kernel
- [x] `SYS_STAT`, `SYS_UNLINK`, `SYS_RENAME`; libc4ix `ustat`, `uunlink`,
      `urename` (`SYS_TOP` is now 227). A removed directory is unlinked but
      its vnode kept, since a task may still have it as its working
      directory; an open file cannot be removed.
- [x] native suites green: `make test-c4ix`, `test-c4ix-c4ke` (2026-09-23)

### D6: the desktop as modules, and the shared pieces
- [x] `src/c4ix/user/desktop/`: `wm.c` (window manager, drawing layer,
      menu bars, dialogs), `ui.c` (widgets), `term.c`, `files.c`,
      `taskmgr.c`, `apps.c`, sharing `desktop.h`; linked by a Makefile rule
      and by `build-images.sh`. The disk carries the modules concatenated
      as `c4ix-desktop.c`.
- [x] each window's client area is clipped; buttons, list views with
      headers, scroll bars, menu bars with drop-downs, text fields, tabs,
      group boxes, radio buttons; message and input boxes (modal);
      windows resize from their corner; title-bar double click maximises;
      the NT 4 Start menu with its banner; five desktop icons
- [x] test-desktop.mjs green

### D7: the tools
- [x] Explorer: tree + list (Name, Size, Type), menus, address, Up,
      status bar; double-click opens folders, runs programs, opens text in
      Notepad; New Folder, Delete (confirmed), Rename; F2, F5, Del,
      Backspace, Enter, arrows
- [x] Task Manager: Applications (End Task, Switch To, New Task),
      Processes (name, PID, CPU %, CPU time, syscalls, state; End Process,
      confirmed; kernel tasks refused), Performance (gauge, 60 s history,
      totals); also Ctrl+Shift+Esc -- which in a browser on Windows is
      taken by Windows itself, so the Start menu and the icon are the way
      in there
- [x] Notepad: open, edit, save (Ctrl+S, Save As), scroll bar, wheel,
      modified mark in the title
- [x] Calculator: 32-bit, Hex/Dec/Oct/Bin, + - * / Mod And Or Xor Not Lsh
      Rsh, keyboard; Run (Start > Run..., and Task Manager's New Task)
- [x] Headless (2026-09-23), 40 checks in `test-desktop.mjs`, twice green:
      Explorer browses, makes, renames and deletes a folder in /ram;
      Notepad saves /ram/untitled.txt, Explorer lists it and reopens it in
      Notepad with its text; Calculator 12+30=42, 2A in Hex, 2A*2=54;
      Task Manager lists spin, ends it on confirmation, shows the
      Performance and Applications tabs; Run starts a command in a new
      Command Prompt; Shut Down leaves no shell behind.
      Found on the way: typing between two clicks no longer makes them a
      double click, and F5 and Backspace work in either Explorer pane.

### D8: in the browser
- [x] CDP gate (3060 Ti, 2026-09-23): the desktop, `ps` in a Command Prompt,
      Explorer opening /usr, Notepad typing, Calculator 6*7=42, Task Manager
      processes with CPU usage and its Performance tab, Shut Down; no page
      errors. Screenshots of Explorer, Notepad with Calculator, and Task
      Manager checked by eye.

Known: C4IX's boot loader race (docs/c4bb-design.md, "Known issue") now
shows on every boot of this build as one extra, empty entry: "vfsload:
cannot open" and 50/51. All 50 real entries load. It moves with timing --
an instrumented vfsload loads 50/50 -- and is not caused by this work.

## Fixes after the user's first look (2026-09-24)

- [x] **`top` ran three times and quit.** It was written before the console
      could be interrupted. It now runs until Ctrl-C, redrawing in place
      (ESC[2J ESC[H) once a machine-second and sleeping in between; `top N`
      still stops after N screens.
- [x] **raycast in a Command Prompt had no colour and the desktop
      flickered.** Four causes, all fixed:
  - the terminal kept 16 colours and dropped raycast's 256-colour codes;
    cells now hold xterm-256 indices (24-bit colours map to the nearest);
  - it was 80x24, so raycast's 25-row frames scrolled every frame; it is
    80x25 now;
  - on the shared display path the page presented at the end of every
    ring batch, so a batch that ended part-way through the desktop's
    redraw showed the half-drawn scene. The page now presents only where
    the guest presents, and draws a trailing partial frame off screen
    (`display.js` drawCoalesced);
  - the terminal read 1 KB per pass; it now drains its pipe before drawing,
    so whole frames arrive together, and output-driven redraws are capped
    at one per 25 ms of machine time (machine time, not host time: the two
    run at different rates when the machine is unpaced, and on host time the
    headless test saw no redraw at all).
  The page's default pace also went from 20 to 100 MHz, the difference
  between raycast managing a few frames a second in the desktop and 80.
  Headless: 30 distinct colours drawn in the Command Prompt; browser:
  raycast's status line and colour counted, screenshot checked.
  raycast still reads keys through `/dev/tty`, which is the page's
  terminal, not the window it draws in: `-d` (demo) runs it hands-free.
- [x] **A movable split between the terminal and the display.** Drag the
      bar between the panes (remembered; double-click resets). The canvas
      scales to fill its pane, snapping to a whole-number scale with sharp
      pixels when one is within 15% of the best fit. Browser gate: dragging
      400 px grew the display from 800 to 1064 px.

## Part three: a lazy disk, programs from Explorer, compilers inside (asked for 2026-09-24)

The web page fetched all 181 disk files, one by one, before the machine
started, although a C4IX boot opens almost none of them. Explorer did not
show the C4IX programs as programs, and could not run them. innerbench
failed. The machine had no compilers to build a program with, and the
user wants to compile a test GUI application inside it and run it.

### L: the lazy disk
- [x] **Lazy RAM files in the kernel.** A vnode can name a host file and
      its size without holding its bytes (`vfs_lazyfile`, SYS_LAZYFILE 227).
      The first read, write or program load fills it (`vfs_fill`), and a
      truncating open drops the host copy. `ls` and stat need only the
      size, so they never fill a file.
- [x] **vfsload makes entries lazy.** The image build writes `c4ix.sizes`,
      a "SIZE NAME" line per disk file. vfsload makes each entry named in
      it a lazy file, and an alias of a lazy file another lazy file on the
      same host bytes. Anything the table does not list is copied as
      before, and with no table at all everything is.
- [x] **The page fetches on demand.** The build also writes `index.json`,
      names and sizes. The page sends only that to the worker. The
      worker's disk fetches a file with a synchronous request the first
      time the machine opens it, which is allowed in a worker, and the
      machine's open is synchronous anyway. A disk with no index falls
      back to the eager fetch, as does `lazy: false`.
- [x] Evidence. Headless desktop test: booting reads 4 of 182 files.
      Browser gate on the 3060 Ti: 4 of 182 fetched, shell prompt 1.2 s
      after the page loaded. `x5-c4m.txt` re-pinned for the larger vnode
      (112 to 128 bytes).

### E: Explorer runs programs
- [x] C4IX's programs live in `/bin` without a `.c4r` suffix, so Explorer
      showed them as plain files and opened them in Notepad. stat now
      says whether a file is a program: by its C4R signature, or for a
      lazy file by its host name, so a listing never fetches anything.
      Explorer shows these as "C4IX Program" with the program icon, and
      double-click or File > Open runs one in a new Command Prompt.
      Headless: `/bin` lists them as programs and double-clicking `hello`
      runs it.

### I: innerbench
- [x] innerbench compiles C4KE from source in nested c4m instances, and
      c4m has no preprocessor. The mailbox commit (ad57ad0, 2026-09-09)
      gave c4ke.c global arrays, which c4's compiler cannot parse, and a
      scheduler call into `c4bb_mbox.h`, which c4m never reads. Both inner
      kernels died with "bad global declaration" while the outer one
      still said "innerbench complete" and exited 0. The table is now
      allocated, and the two mailbox reads the scheduler needs are
      written out against the registers. `make test` now fails when an
      inner kernel does not compile or no inner benchmark finishes; it
      passes with the fix and fails on the old source.

### C: compilers inside C4IX
The survey (2026-09-24): the VM has no write opcode, so a compiler writes
its output through C4KE's `OP_VFS_PUT`, the C4DOS RAM-disk API, or
stdout. C4IX offers none of the first two, so c4cc, c4rlink and c4sp/c4lc
all fail at the write. c4th writes images to stdout, and the C4IX shell
can redirect that into a RAM file, so the Forth route works already.
- [x] **C1: C4KE's RAM-filesystem opcodes in C4IX.** OP_VFS_PUT, GET,
      UNLINK, COUNT and NAME in `src/c4ix/c4ke.c`, over the C4IX VFS (a
      bare name lands in the working directory; GET hands back the
      kernel's bytes in place). CK_TOP went from 160 to 161. Tasks are now
      told C4I_TRAPH: C4IX read the info bits at boot, before it installed
      its trap handler, and c4cc will not probe for OP_VFS_PUT without it.
- [x] **C2: the tools in `/bin`.** c4cc, c4rlink, c4rdump, cpp, c4sp (with
      c4lc beside it on the disk) and c4th, with its Forth sources in
      `/usr/src/forth`. `/usr/include` has `window.h` and `c4ix_user.h`;
      `/usr/src/examples` has a README, hello.c and bounce.c. A 32-bit
      `libc4ix.c4l` is not on the disk yet, so c4lc builds plain programs,
      not C4IX-library ones.
- [x] **C3: room to run them.** Measured rather than raised: c4cc
      compiles bounce.c, c4lc compiles hello.c and c4th compiles and runs
      its self-test image, all on the stock 32 KB task stack. Larger
      c4lc builds are not measured.
- [x] **The loader preferred the host.** A program compiled to
      `/ram/hello.c4r` ran the disk's own `hello.c4r`, because the board's
      disk answers a missing path by its bare file name. A RAM file by
      the path now wins.
- [x] **C4: compile a GUI program inside the machine and run it.** The
      desktop owns the display, so a program gets a window through its
      Command Prompt: `include/window.h` writes drawing commands to
      stdout as ESC _ G ... ESC \ sequences, and the window sends mouse
      and key events back on stdin. The window is the program's until it
      closes it, Ctrl-C stops it, or the shell prompts again. The header
      builds with c4cc (no preprocessor, structs or arrays) and with c4lc.
      Headless desktop test and browser gate on the 3060 Ti: c4cc
      compiles bounce.c in a Command Prompt; it takes the window, titled,
      animates, counts a click on its button, sees a typed key, and q
      gives the terminal back with its title.

### Found on the way, not fixed
- `make test-c4th-os`: the C4DOS leg prints an empty prompt line ahead of
  c4th's output, so it differs from the reference at byte 1. Nothing it
  runs changed in this work, and a fresh `c4dos-clock.c4r` does the same.
- `test-c4ke-mbox` (in `test-c4bb`) fails about one run in four with
  "Custom opcode not found: 1073741824" in the mbpair message. It fails
  as often with the C4KE source from before this work.
