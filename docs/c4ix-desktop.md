# The C4IX desktop: windows, and a terminal emulator in them

Status tracker and design. Tick a box only on green evidence, and write
the evidence next to it.

## What it is

`desktop` is a C4IX user program that takes over the libjs display
(docs/libjs-design.md) and draws a Windows 95-style desktop on it: a teal
background, bevelled grey windows with navy title bars that can be
dragged, focused, minimised, maximised and closed, and a taskbar with a
Go menu, a button per window and a clock.

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

**The desktop** (src/c4ix/user/desktop/, nine modules of its own over the window kit's eleven in src/winkit, and libc's `fp.c`, linked with libc4ix -> c4ix-desktop.c4r; it was one file, `desktop.c`, until D6, `edit.c` joined in part four, `calc.c` left `apps.c` with floating point, and the window kit, which grin shares, left `wm.c`, `term.c` and `apps.c` in part fourteen):
- Asks for an 800x600 display (1024x768 was too small to read once the
  page scaled it into its pane), attaches the rings, and redraws what
  changed: the whole scene, or only the damaged regions
  (docs/c4ix-cpu.md, step 11). Idle, it waits on `SYS_EVWAIT` for a
  deadline, an event or output on a terminal's pipe.
- Windows are a z-ordered list (a linked list, as many as memory allows;
  part seven). Title bar drag moves, a click raises and
  focuses, the three caption buttons minimise, maximise/restore and close.
- The taskbar has a Start button (menu: New Terminal, About, Power Off),
  a button per window, and a clock.
- A terminal window is 80x25 cells of 8x16 pixels (80x24 until the
  fixes of 2026-09-24), with 300 rows of scrollback. Closing it ends its
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
- [x] Taskbar: Go menu, window buttons, clock
- [x] Headless test drives it with display events (`libjs/tests/test-desktop.mjs`,
      in `make test-libjs`)

### D3: the terminal
- [x] A shell per window over pipes; output through the VT100 subset
- [x] Local line editing, Enter sends the line, Ctrl-C interrupts
- [x] Headless test (2026-09-23): `ps` typed in a window runs in that
      window's shell and lands in that window; a second terminal from the
      Go menu; `spin` there and Ctrl-C shows `^C` and stops it while the
      first terminal still answers; dragging moves a window; closing one
      removes it; Power Off returns to the console, and no terminal shell
      outlives the desktop. C4IX's vfs pins widened to `[4-9]x/[4-9]x`
      entries (now 50/50) in test-libjs and test-c4bb.

### D4: in the browser
- [x] CDP gate (`test-web.mjs --gui`, 3060 Ti, 2026-09-23): `desktop` from
      the C4IX shell; the display becomes 800x600; a terminal shows the
      prompt; `ps` typed on the display runs in that window; Start opens its
      menu and New Terminal opens a second window; a window drags by its
      title bar; its close button closes it; Power Off hands the console
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

Explorer, System Monitor, Text Editor, Calculator and Run, in the Windows NT 4
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
  folder, runs a program in a new Terminal, or opens anything else
  in Text Editor. New Folder, Delete (confirmed), Rename. Several items
  can be selected: Ctrl-click adds or removes one, Shift-click or
  Shift with the arrows takes a range, Ctrl+A takes them all. Delete
  takes everything selected, a folder with all that is in it. (A click
  carries no modifiers from the page, but Ctrl and Shift are keys the
  page sends: the desktop keeps them, `wm_mods`.)
- **System Monitor** (Ctrl+Shift+Esc too): Applications (the desktop's
  windows, Close, Show), Processes (every task with PID, CPU %,
  CPU time in cycles, the memory it holds, syscalls and state, Kill),
  Performance (a CPU gauge and a scrolling history graph, totals, and
  the memory in use, free and in all, with its own history). The status
  bar says the memory in use on every page.
  - A task's memory is its image (code, data, the constructor and
    destructor lists), its stack, its copies of the shared libraries'
    data, and what it allocated (`ualloc`, or `malloc` behind protected
    mode). The System row holds the rest of what is in use: the files,
    which are RAM, the shared libraries' code, and the kernel's own.
  - The heap's size and use come from the host. libjs counts its heap
    (`HEAP_USED`, docs/c4bb-design.md); on native c4m the totals say
    "not known on this host".
- **Text Editor**: open, edit and save text files in C4IX's filesystem;
  keyboard editing, mouse placement, scroll bar and wheel.
- **Calculator** (`calc.c`), in three modes:
  - Integer: 32-bit words in Hex, Dec, Oct or Bin, with + - * /, Mod, And, Or, Xor, Not, Lsh, Rsh.
  - Single and Double: IEEE floating point (docs/c4-float.md), each operation done in that precision with c4lc's `float` or `double`. The display shows the shortest decimal that reads back as the value, and a line under it the value's bits: 0.1 + 0.2 is 0.30000000000000004 in Double and 0.3 in Single. Keys: Sqrt, x^2, 1/x, Int, Abs, Pi, e, Exp (an exponent, as in `1e-7`), and the constants Max, Min (the smallest normal), Eps and Inf. On the keyboard `.` is the point, `e` Exp, `p` Pi and `r` Sqrt.
  - The conversions both ways are libc's `fp.c`, linked into the desktop, so they are exact.
- **Run**: Go > Run... starts a program in a new Terminal.

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
      the NT 4 Go menu with its banner; five desktop icons
- [x] test-desktop.mjs green

### D7: the tools
- [x] Explorer: tree + list (Name, Size, Type), menus, address, Up,
      status bar; double-click opens folders, runs programs, opens text in
      Text Editor; New Folder, Delete (confirmed), Rename; F2, F5, Del,
      Backspace, Enter, arrows
- [x] System Monitor: Applications (Close, Show, New Task),
      Processes (name, PID, CPU %, CPU time, syscalls, state; Kill,
      confirmed; kernel tasks refused), Performance (gauge, 60 s history,
      totals); also Ctrl+Shift+Esc -- which in a browser on Windows is
      taken by Windows itself, so the Go menu and the icon are the way
      in there
- [x] Text Editor: open, edit, save (Ctrl+S, Save As), scroll bar, wheel,
      modified mark in the title
- [x] Calculator: 32-bit, Hex/Dec/Oct/Bin, + - * / Mod And Or Xor Not Lsh
      Rsh, keyboard; Run (Go > Run..., and System Monitor's New Task)
- [x] Headless (2026-09-23), 40 checks in `test-desktop.mjs`, twice green:
      Explorer browses, makes, renames and deletes a folder in /ram;
      Text Editor saves /ram/untitled.txt, Explorer lists it and reopens it in
      Text Editor with its text; Calculator 12+30=42, 2A in Hex, 2A*2=54;
      System Monitor lists spin, ends it on confirmation, shows the
      Performance and Applications tabs; Run starts a command in a new
      Terminal; Power Off leaves no shell behind.
      Found on the way: typing between two clicks no longer makes them a
      double click, and F5 and Backspace work in either Explorer pane.

### D8: in the browser
- [x] CDP gate (3060 Ti, 2026-09-23): the desktop, `ps` in a Terminal,
      Explorer opening /usr, Text Editor typing, Calculator 6*7=42, System Monitor
      processes with CPU usage and its Performance tab, Power Off; no page
      errors. Screenshots of Explorer, Text Editor with Calculator, and Task
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
- [x] **raycast in a Terminal had no colour and the desktop
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
  Headless: 30 distinct colours drawn in the Terminal; browser:
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
      showed them as plain files and opened them in Text Editor. stat now
      says whether a file is a program: by its C4R signature, or for a
      lazy file by its host name, so a listing never fetches anything.
      Explorer shows these as "C4IX Program" with the program icon, and
      double-click or File > Open runs one in a new Terminal.
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
      Terminal: `include/window.h` writes drawing commands to
      stdout as ESC _ G ... ESC \ sequences, and the window sends mouse
      and key events back on stdin. The window is the program's until it
      closes it, Ctrl-C stops it, or the shell prompts again. The header
      builds with c4cc (no preprocessor, structs or arrays) and with c4lc.
      Headless desktop test and browser gate on the 3060 Ti: c4cc
      compiles bounce.c in a Terminal; it takes the window, titled,
      animates, counts a click on its button, sees a typed key, and q
      gives the terminal back with its title.

## Part four: an IDE (asked for 2026-10-19)

The machine could compile inside itself (part three), and now rebuild
everything it ships (`b4ke -C /usr/src/c4`, docs/c4ix-design.md). What
it lacked was somewhere to edit and build in one place.

- [x] **The Editor** (`src/c4ix/user/desktop/edit.c`, window kind
      `K_EDIT`, Go > Editor, and Explorer for `.c .h .b4k .lisp .f
      .js` files). Lines are separate allocations, so there is no
      Text Editor-style 65,000-byte limit; undo and redo are snapshots of the
      text, 32 deep.
- [x] **VIM mode**, on by default. Normal, Insert, Visual and Visual-line,
      and the `:` / `?` command line. Motions with counts (`hjkl w b e W B
      E 0 ^ $ gg G { } % f t F T ; ,`), operators `d c y > <` with any
      motion or doubled, text objects `iw aw i( a( i{ a{ i" a"`, `x X D C
      s S r J ~ p P u Ctrl-R . n N *`, and `:w :q :q! :wq :x :e :N :$
      :s/a/b/ :%s/a/b/g :set nu|nonu|vim|novim`. `:set novim` is a plain
      editor in which Esc opens the command line.
- [x] **Arrow keys in every mode**, Insert included, with Home/End,
      PgUp/PgDn, Ctrl+Home/End and Ctrl+Left/Right; Shift+arrows start a
      Visual selection. Ctrl+S saves from any mode.
- [x] **Build what is open.** `:make` (F5) saves, then runs b4ke over pipes
      into a pane under the text, the way the Terminal runs its
      shell. It uses the nearest `BUILD.B4K` at or above the file;
      without one, a `.c` file gets a build file of its own
      (`/ram/edit.b4k`: c4sc, then c4rlink with `/usr/lib/libc4ix.c4l`).
      b4ke now echoes each command it runs (`+ c4sc ...`), so c4lc's
      `line N:` errors, which name no file, are matched to the source the
      command compiled. `FILE:N:` and c4cc's `N:` count too. The build
      goes to the first error; F8 / Shift+F8 or `:cn` / `:cp` move
      through them, and a double click in the pane jumps. `:run` (F6) runs
      the result in a new Terminal: the lone file's image, or a
      `BUILD.B4K`'s `SET RUN command`.
- [x] Pinned by `test-desktop.mjs`: Insert and Esc, auto-indent, `:w`, `2G
      f4 r7`, `yyp`, `dd`, `u`, arrows in Insert, `:make` building, a
      compile error listed and gone to, `ciw` and `.`, `/` and `V j d`,
      `:set novim` and back, and `:q!`.

## Part five: the IDE grows a file pane, and the desktop's scroll bars drag (asked for 2026-10-20)

- [x] **Scroll bars drag.** Every bar on the desktop -- Text Editor, the Editor,
      both of Explorer's panes, System Monitor -- moves with its thumb.
      `ui_scroll_click` starts a drag when the thumb is pressed;
      `ui_scroll_drag(x, y, py)` answers each move with the top line for
      the bar at x, y (a window may have two); `mouse_up` ends it. While a
      drag is on, moves reach the window even outside it, and each one
      redraws. A click on a bar now also brings the cursor or selection
      along, as the wheel does, where it used to be scrolled straight back
      to it by the next redraw (the Editor, System Monitor).
- [x] **Explorer's tree** scrolls (a bar and the wheel) and its folders
      collapse: a +/- box, a double click, Left/Right or -/+.
- [x] **An IDE icon** on the desktop, beside Text Editor: the Editor.
- [x] **Explorer opens source in the IDE**: `.c` and `.h` already did; so
      do shell scripts, Makefiles, assembly, Markdown, JSON, HTML, CSS,
      Python and Forth's `.fs` now. Plain `.txt` stays Text Editor's.
- [x] **The Editor's file pane**: a tree of the disk down the left, its
      folders closed to start with. A click opens or closes a folder, and
      opens a file (not over unsaved changes). Opening a file, from the
      pane, `:e`, a build error or Explorer, opens the folders on the way
      to it and marks it; a file written with `:w` joins its folder's
      list. Ctrl+B, View > Files or `:Ex` shows and hides it, and it gives
      way when the window is too narrow for it and the text.
- [x] **Syntax colours, by the file's name**: C and headers, JavaScript,
      Lisp, Forth, shell scripts and Makefiles, and B4KE build files --
      keywords, types, strings, comments, numbers and the preprocessor. A
      line is coloured on its own; only being inside a C comment carries
      over, and where that stands at the top of the view is kept with a
      count of changes, so scrolling does not read the file from the top.
      `:syntax off` / `on`, or View > Syntax Colouring.
- [x] **Bars across** (`ui_hscrollbar`, `ui_hscroll_click`, dragged
      through the same `ui_scroll_drag`): under the Editor's text, under
      its file pane and Explorer's tree when a name is wider than them, and
      under the build log. Across, the text follows the cursor only when
      the cursor has moved, so the bar can be scrolled away from it.
- [x] **Word wrap**: `:set wrap` / `:set nowrap`, View > Word Wrap. Long
      lines fold onto the rows below; `j` and `k` still move by line, and
      a click lands on the right column of a folded row.
- [x] **The build pane** scrolls both ways and keeps its place when
      scrolled up to read. While a build runs its header has **Cancel**
      (also Build > Cancel Build, `:cancel`): what b4ke is running is
      interrupted and ended first, then b4ke, and every one is waited
      for, so a cancelled compiler neither runs on nor stays a zombie.
      Closing the window mid-build does the same.
- [x] **A CPU meter** beside the clock: System Monitor's number, from its
      own sample every two seconds, redrawn only when it changes; a click
      opens System Monitor on Performance. It inherits that number's model,
      which counts the boot task as idle (see the CPU review plan).
- [x] Pinned by `test-desktop.mjs`: the icon; the pane listing the root,
      opening folders and a file; the colours of `hello.c`; `:syntax off`;
      Ctrl+B both ways; `:e` opening the folders to a file; dragging the
      Editor's thumb and Explorer's tree thumb; the meter; a long line off
      to the right, the bar across reaching its end, and `:set wrap`
      folding it; `:make` under a `BUILD.B4K` cancelled from the pane,
      with no c4sc or b4ke left in System Monitor; Explorer's tree scrolled
      across. Not yet looked at in a browser.

## Part six: the browser's boot, a first frame, Settings, a pointer (asked for 2026-10-20)

- [x] **The web page boots with `--desktop`** (`libjs/web/app.js`). init
      then starts the shell with `-e "desktop &" -e top`: the shell runs
      each `-e` line first, as if typed at the prompt, so the desktop runs
      on the display and top on the console. top is the console's
      foreground job, so Ctrl-C there stops it and gives the prompt back
      while the desktop carries on. (It was `-e "top &" -e desktop` at
      first, which left the console's Ctrl-C aimed at the desktop and top
      clearing the screen over every command.) `c4ix-sh -e CMD ...` is
      general.
- [x] **The first frame stays.** The page learns the desktop's size by
      message and resizes (clearing) the canvas, while frames come through
      the ring: a first frame could land before the resize and be wiped,
      leaving a black display until a click. `Display.resize` now tells
      the guest (a resize event), the desktop asks for resize and focus
      events and redraws on them, and it draws once more half a second
      in.
- [x] **Settings** (Go > Settings, `K_SETTINGS`, in `apps.c`): eight
      colour schemes (Classic Teal, Slate, Rainy Day, Desert, Rose,
      Spruce, Lilac, Storm) and a desktop colour (the scheme's, or one of
      eight), applied at once beside a preview, kept in
      `/etc/desktop.cfg` and read when the desktop starts -- so a choice
      outlives Power Off, not a reload of the page. The scheme's nine
      colours (`C_DESK`, `C_FACE`, `C_LIGHT`, `C_SHADOW`, the two title
      gradients and `C_SEL`) are variables now; text, white and the
      green of the graphs stay constants.
- [x] **A pointer.** The display was `cursor: crosshair`; it is the
      host's arrow now, and the guest picks a shape with a new ring
      command, `GUI_CURSOR` (`gui_cursor`, `GUI_PTR_*`: arrow, text beam,
      resize, hand, move, wait, cross, none). It takes effect at once,
      with no frame. The desktop shows a text beam over the Command
      Prompt, Text Editor's text and the Editor's text (`edit_pointer`), the
      resize arrow on a size grip, and the arrow elsewhere.
- [x] Pinned by `test-boot-desktop.mjs` (new, in `test-libjs`): `--desktop`
      starts both, and a resize from the page draws the scene again; and
      by `test-desktop.mjs`: Settings repaints in Slate, keeps it in
      `/etc/desktop.cfg`, and Defaults restores it; the pointer over the
      text and the taskbar.

## Part seven: the Terminal edits and scrolls, windows without a limit, builds that start built (asked for 2026-10-20)

- [x] **Line editing** (`term.c`). The line being typed is edited where
      it stands: Left/Right, Home/End (Ctrl-A/Ctrl-E), Backspace and
      Delete anywhere in it, Ctrl-U to clear it, Ctrl-L to draw it again.
      Up/Down walk the last 16 lines sent; Down past the newest gives back
      what was being typed. The terminal still sends the shell whole
      lines, so every program sees what it saw before.
- [x] **The prompt is on the input line**: `c4ix-sh -p`, which the
      Terminal passes. The console shell keeps its newline -- on the
      console, output is buffered until one -- but a pipe delivers every
      write at once, so the window does not need it.
- [x] **Scrollback**: 300 rows in a ring beside the 80x25 screen, with a
      scroll bar, PgUp/PgDn and the wheel. Typing comes back to the
      bottom.
- [x] **System Monitor's Interval column**, as top has: what each task ran
      in the last sample, in kilocycles (850k, 12.3M); the window is 490
      wide for it.
- [x] **Windows are a linked list** (`desktop.h`, `wm.c`): MAXW's eight
      slots are gone, and there are as many windows as memory allows.
      Each is its own allocation, on two lists through the same nodes:
      the order they opened in (the taskbar's, `win_first()`/`next` for
      the tools) and the z-order. Window numbers are never reused, so a
      number kept for a closed window finds nothing; a closed window is
      freed at the top of the next pass of the main loop, since whoever
      closed it may still hold it.
- [x] **`cc`** (`user/cc.c`): c4sc with c4lc's preprocessor, `/usr/include`
      on a task's default stack, linking with c4rlink when given `-l` or several
      sources. src/c4tui/demo.c takes 18 machine-seconds this way and 344
      with `c4sp c4lc.lisp`, for the same image.
- [x] **Builds start built**: the root archive carries `/build`
      (docs/c4ix-design.md, "Already built"), so the IDE's `:make` on a
      file under `/usr` compiles that file and links its program, not
      the libraries first.
- [x] **The repo is /usr, builds go to /build, no /ram**
      (docs/c4ix-design.md): one copy of each source, `<u0.h>` found in
      `/usr/include`, and `/tmp` for scratch. The IDE builds a `.c` file
      that no BUILD.B4K names on its own, with cc, into `/tmp/NAME.c4r`
      (its build file is `/tmp/edit.b4k`).
- [x] Pinned by `test-desktop.mjs` (history, in-line edits, scrollback,
      ten more windows than before) and `test-libjs.sh` (cc builds echo
      the disk's, byte for byte; a fresh boot's b4ke has nothing to do; an
      edited source remakes two steps).

### Found on the way, not fixed
- `make test-c4th-os`: the C4DOS leg prints an empty prompt line ahead of
  c4th's output, so it differs from the reference at byte 1. Nothing it
  runs changed in this work, and a fresh `c4dos-clock.c4r` does the same.
  *Fixed 2026-09-30:* that line is DOS's own `A>` prompt on a line of its
  own (the console is unbuffered, so the prompt ends its line); the test's
  filter now drops a bare `A>` line before stripping the prefix.
- `test-c4ke-mbox` (in `test-c4bb`) fails about one run in four with
  "Custom opcode not found: 1073741824" in the mbpair message. It fails
  as often with the C4KE source from before this work.
  *Fixed 2026-10-24:* a PIT tick delivered between C4KE's re-arm of the
  PIT and its handler's TLEV, after it had switched tasks; it hit hosts
  slower than `CYCLES_PER_MS`. c4bb (6295e18) and libjs hold the PIT and
  the mailbox inside a handler now (docs/c4bb-storage.md, "What the
  kernel taught the hardware").

## Part eight: the boot waits for its filesystem, with a loading screen (asked for 2026-10-21)

- [x] **The archive first.** The browser's boot (`--desktop`) started the
      desktop and top while c4ix-tar was still unpacking the root archive
      in the background, so Filesystem was missing files for several
      seconds. Now init runs `vfsload -w`, which waits for the archive
      before it returns: the desktop and top start on the whole
      filesystem, and the console's boot lines stay up long enough to
      read. The console's own boot (no arguments) still unpacks in the
      background, with `wait 4` to block on it.
- [x] **A loading screen** (`user/splash.c`, `/bin/splash`). While it
      waits, vfsload runs `splash -p PID`: the boot logo full screen, and
      along the bottom a band of colour that flows to the right, as the
      late-nineties loading screens cycled their palettes. When the tar
      task is gone it lets go of the display (`gui_detach`) and exits, and
      the desktop takes the display. `splash` alone, from a shell, shows
      it until a key or a click. With no display it says "not fitted".
- [x] **The logo** (`src/c4ix/art/`): drawn by Kiln (project "C4IX",
      Content/Boot Logo) from a description: "C4IX" in chrome with
      frosting dripping off it, "Built upon Robert Swierczek's C4" under
      it, and a cake made of circuit boards whose candle flame is a chip
      (C4IX, C4KE: cakes). `boot-logo.png` is Kiln's 1200x900 original;
      `mkc4p.py` scales it to 800x600 and dithers it to 256 colours, as a
      gzipped "C4P8" image (155 KB) that splash inflates with libarc. Both
      are committed, so building the disk needs no Pillow. It is a disk
      file (`/usr/share/splash/c4ix.c4p.gz`, lazy like the rest), not in
      the archive it is shown during.
- [x] **`/log/kernel`.** Everything the console says while booting is
      kept there: the kernel's lines, and the tasks' (vfsload, tar) until
      init starts the shell. After that only the kernel's own lines are
      added, or top's redraws would fill it. Lines from before the VFS
      exists are held back and written when it does. It stops at 256 KB,
      and the kernel holds it open, so it cannot be deleted.
- [x] Pinned by `test-boot-desktop.mjs`: vfsload waits; splash puts the
      logo up and animates; the desktop and top start only after the
      archive is unpacked; `cat /log/kernel` has the kernel's and vfsload's
      lines and not top's. Seen in a browser too (2026-10-21).

## Part nine: the boot logo as the desktop's picture, and screen savers (asked for 2026-10-21)

- [x] **The picture** (`wall.c`): the boot logo, stretched over the
      desktop (it is made at the desktop's 800x600, so 1:1 today). The
      desktop inflates it once at start, from the same file splash shows
      (`/usr/share/splash/c4ix.c4p.gz`, so it links libarc now), and gives
      the display 64 tiles of 100x75 (`gui_imgdef`: a tile has to fit the
      command ring), which the display keeps. Every redraw of the
      background then draws only the tiles it needs (`gui_image`), clipped
      to the damage like everything else. Icon labels get a dark shadow
      over it. Settings: "Picture: C4IX" (the default), or the scheme's
      colour, or one of the eight; `/etc/desktop.cfg` gains `wall 0|1`.
- [x] **Screen savers** (`savers.c`), after the user's RetroSavers (D3D
      ports of the classics), redone in integer arithmetic with the
      display's line, rectangle and text commands: Starfield, Polygons
      (two shapes bouncing, trailing their last outlines), Beziers (a loop
      of cubic curves with a fading trail), Marquee (a line of text
      crawling across) and Digital Rain (columns of falling characters).
      The names are generic ones. After the chosen number of minutes with
      no input -- machine minutes, the clock the desktop sleeps on -- the
      saver takes the whole display, a frame every 33 ms, with the pointer
      hidden; the first key or movement ends it and the desktop is drawn
      again as it was. Settings has a Screen saver group: which (or
      None), the wait (1-60 minutes, default 5, Starfield), and Preview;
      `/etc/desktop.cfg` gains `saver S wait M`. Defaults is the look
      only: it leaves the saver as it is.
- [x] Pinned by `test-desktop.mjs`: the picture goes to the display once
      as 64 tiles and is drawn; the scheme's colour turns it off and
      Picture back on; Preview runs a saver over everything and a key ends
      it; the choices are kept in `/etc/desktop.cfg`. The test starts the
      desktop with no saver chosen: headless, an idle machine covers five
      minutes within one of its long builds.

### Found on the way, not fixed

- One more Terminal open (a Run of `cat`, say) before Power Off,
  late in test-desktop's long run, and the desktop exits without its
  "desktop: shut down": the shell's prompt comes back, the line never
  does. Power Off straight after boot, or after the picture and saver
  checks alone, is fine; so is the whole run without that window.

## Part ten: names of its own (asked for 2026-10-21)

The desktop began as a homage to Windows 95 and NT 4, names and all.
It keeps the era's feel, but nothing it shows is Microsoft's any more:

| was | is |
|---|---|
| Start, Start menu | Go, Go menu |
| Shut Down..., "It's now safe to turn off your computer." | Power Off..., "C4IX has stopped. You can close this page." |
| Command Prompt | Terminal |
| My Computer | Filesystem |
| Explorer, "Exploring - /path" | Files, "Files - /path" |
| Notepad | Text Editor |
| Task Manager: Applications, Processes, Performance | System Monitor: Programs, Tasks, Load |
| End Task, Switch To, New Task..., End Process | Close, Show, Run..., Kill |
| Image Name, System Idle Process | Name, idle |
| Windows Standard, Slate, Rainy Day, Desert, Rose, Spruce, Lilac, Storm | Classic Teal, Granite, Drizzle, Sandstone, Blush, Pine, Lavender, Thunder |
| File Folder, Text Document, "N object(s)", Up One Level | Folder, Text, "N items", Up |
| Confirm File Delete, "Are you sure you want to delete 'x'?" | Delete File, "Delete 'x'?" |

Earlier parts above keep the names they were written with where they
describe what was done then. The code's own names (`K_NOTE`,
`taskmgr.c`, the Editor's `:Explore`) are unchanged.

## Part eleven: two looks, flat by default (asked for 2026-10-21)

- [x] **Flat** (the default), after the open-source desktops rather than
      Windows: 1-pixel outlines where there were two-pixel bevels; solid
      title bars to the window's edges, their caption buttons bare glyphs
      in the bar's text colour (white on a dark bar, the scheme's dark on
      a light one); light buttons with an accent outline for the default;
      tabs underlined in the accent; radio buttons filled with it; slim
      scroll-bar thumbs on the track; a flat taskbar whose focused button
      is underlined; the Go menu's banner in the accent. The accent is the
      scheme's title colour.
- [x] **Modern**, a ninth scheme, made for it: cool greys, slate title
      bars, no gradient. It is the default, applied as the desktop starts
      (the scheme's colours were only ever applied from /etc/desktop.cfg:
      the default had been the compiled-in Classic Teal).
- [x] **Classic**: the bevelled look the desktop had, with any scheme.
- [x] Every window keeps its geometry in both, so nothing moves when the
      look changes and no hit test differs. The routines that draw chrome
      ask `ui_style` (`d_bevel`, the caption buttons, `draw_window`,
      `draw_taskbar`, `draw_start`, and ui.c's buttons, scroll bars, tabs,
      radios and headers); the applications draw through them.
- [x] **Go's mark** is C4IX's own now in both looks: a little cake with
      its candle lit, where the four coloured squares were.
- [x] Settings has a Look group (Flat, Classic); `/etc/desktop.cfg` gains
      `style 0|1`; Defaults is Modern, Flat and the picture. Preview's
      grace for its own click is 150 ms (it was 400, long enough to swallow
      the key test-desktop pressed to end the saver).
- [x] Pinned by test-desktop.mjs: the desktop starts Flat; Look > Classic
      and back, seen in the taskbar's top line; Defaults brings Modern
      back. Seen in a browser (2026-10-22).

## Part twelve: a game in a window (asked for 2026-10-21)

Wolfenstein 3D ([c4ix-wolf3d.md](c4ix-wolf3d.md)) needed two things from
the desktop that drawing commands and key presses could not give it.

- [x] **8-bit framebuffers.** `window.h`'s `win_fb(pixels, w, h, pitch,
      pal)` sends `f` with the pixels' address and a palette of 256
      0x00RRGGBB words. The Terminal keeps it in the canvas's display list
      like any other op and draws it with `GUI_BLIT8` (16), scaled to the
      canvas.
  - The host reads the bytes and the colours when the desktop rings the
    bell, and puts 32-bit pixels in the command stream in their place (RGB,
    102). The page draws them unsmoothed, inside any clip.
  - Converting the pixels in the guest would cost it as much as drawing
    the frame.
- [x] **Flow control.** Once the desktop has drawn a frame that has one,
      the program hears `f` (WIN_FRAME). Until then it draws its next frame
      into other memory. `term_drawn`, after every redraw, sends it, so the
      program never waits on the window and the window never shows a frame
      half drawn.
- [x] **Keys let go.** The page always sent key-up events, and the desktop
      dropped them. A canvas now hears them as `K` (WIN_KEYUP), because a
      game moves while a key is held.
- [x] **Go > Wolfenstein 3D** runs `wolf3d` in a new Terminal. The first
      time, the game unpacks the shareware episode there, and then the
      window is its canvas.
- [x] Pinned by test-desktop.mjs:
  - `wolf3d` unpacks the episode and shows 320x200 8-bit frames scaled to
    640x400;
  - a key goes to the game, and its picture changes;
  - Ctrl-C makes the window a Terminal again.

## Part thirteen: a game on the whole display (asked for 2026-10-23)

A game in a Terminal window draws through the desktop. Every frame goes
over the window's pipe, and the desktop puts it on the display. Even at
400 MHz, Wolfenstein 3D was not satisfying to play that way, and
`wolf3d --direct`, drawing on the display itself, ran much better. But
the display serves one ring at a time. A program that attached its own
under the desktop fought it for the screen, and when the program ended
the display was left pointing at its freed ring. The desktop never had it
back.

Windows 3 to 98 had the answer: a DOS game took the whole screen, and the
desktop waited until it was done. C4IX does the same.

- [x] **The kernel keeps who has the display** (`SYS_DISPLAY`, 240;
      src/c4ix/sys.c, "The display"; `include/display.h`).
  - A program asks with `udisplay_take()` before it attaches, and gives
    the display back with `udisplay_give()` after it detaches.
  - The desktop takes the display at its start, with `DISP_YIELDS`: it
    agrees to give way.
  - When a program asks, the kernel wakes the desktop and tells it
    (`DISP_STATE` is 1). The program keeps asking every 10 ms.
  - The desktop yields between frames, and the display passes to the
    program.
  - A desktop that has not yielded after a second is passed over, so a
    stuck one cannot keep a game off the screen.
  - Only one level: a second program asking while a game has the display
    is told no.
- [x] **The desktop stands aside.** While a game has the display, the
      desktop draws nothing and reads no events, since the keys go to the
      game. It still reads its terminals' pipes, so the programs in its
      windows run on unseen. A program that draws into its window waits
      for the frame to be shown, as it always has.
- [x] **And comes back.** The game gives the display up, or dies and
      `task_zombify` gives it up for the game. The desktop then attaches
      its ring again, puts the pointer back and draws the whole scene. A
      Terminal shows whatever its program printed in the meantime.
- [x] **Wolfenstein 3D takes the display by default.** That covers
      `wolf3d` and Go > Wolfenstein 3D. `wolf3d --window` plays in the
      Terminal window, as before. A game that finds the display held by a
      program that does not give way plays in the window instead.
- [x] **`gui`, the display's demo, asks too.** Its source,
      libjs/guest/gui-demo.c, is shared with c4cc, which has no
      preprocessor, so src/c4ix/user/gui.c wraps its attach and detach
      in the take and the give.
- [x] Pinned by test-desktop.mjs: `gui 40` in a Terminal has the display
      at 640x480 and presents its own frames. When it ends, the desktop
      has the display back at 800x600, with the Terminal showing
      `gui: done after 40 frames`. Checked in the browser too: `wolf3d`
      from a Terminal plays on the whole display, and quitting from its
      menu brings the desktop back.

### Not done

- **No way back but the game's own.** While a game has the display,
  every key is the game's, so Ctrl-C cannot reach its Terminal. A game
  ends from its menu. A stuck one has to be killed from the page's
  console: Ctrl-C there stops top, and `kill` takes the game's id from
  `ps`. Its death gives the desktop the display back. Windows' Alt-Tab,
  which put the game in the background and brought it back later, would
  need the game to give way as the desktop does.
- **`raycast -G` does not ask.** It is one image for the bare machine and
  C4IX, so it cannot make a C4IX system call. Run under the desktop, it
  still fights it.

## Part fourteen: the window kit, shared with grin, and a Windows 3.1 look (asked for 2026-10-24)

grin, the Windows 3.1-style shell planned for C4DOS (docs/grin-plan.md),
shares its GUI code with this desktop instead of copying it. Milestone G1
of that plan moved everything here that is not C4IX's own into a window
kit, `src/winkit/` (winkit.h has the API), and rebuilt the desktop on it.

- [x] **The kit**: `draw.c` (the d_* layer: an origin, its clip, damage,
      the colour schemes, bevels, icons), `ui.c`, `wmcore.c` (windows,
      z-order, focus, modality, drag and size, menu bars, timers, the
      clipboard, events, the redraw), `dialog.c` (message, input and file
      boxes), `look95.c` (Flat, Classic), `look31.c` (Windows 3.1),
      `textgrid.c` (term.c's VT100 grid, scrollback, line editor and row
      cache, without the pipes), `savers.c`, and the accessories as
      classes: `calc.c`, `notepad.c` (the Text Editor), `clock.c` (new;
      linked in, not on the Go menu).
- [x] **A class per window kind**: a `struct wk_class` of hooks that may be
      0 (the review's M1), with per-class state sizes (B9) and int handles
      that are never reused. The desktop's own tools keep the hooks they
      were written with; `tools.c` is one table of them and the adapters
      that make each kind a class. `shell.c` is what `wm.c` kept: the
      display, the loop, the icons and picture, the taskbar, the Go menu,
      desktop.cfg. `host.c` gives the kit memory and files.
- [x] **The screen's size at run time**: a resize from the page lays the
      windows out again (a maximised one fills the new area, the rest are
      pulled back onto it), the taskbar follows the bottom, and the
      picture is stretched to the new size.
- [x] **Settings > Look > Windows 3.1**: the thick sizing border with its
      notches, a solid title bar with the title centred, the control-menu
      box (a click opens Restore, Minimize, Maximize, Close; a double
      click closes) and raised minimise and maximise arrows. The taskbar
      stays; the choice is `style 2` in desktop.cfg; Flat stays the
      default.
- [x] The review's B3 (bounded paths), B5, B6, B7, B8 and B10 are not
      carried over (see the commit). The picture's gzipped file, which
      the desktop used to keep beside the inflated picture, is freed.
- [x] test-desktop.mjs: the old checks unchanged and green, and new ones
      for 640x480 and back and for the 3.1 look across a restart.

### Not done

- B1 and B2 (the Editor's leak, End Process) and B3 in `edit.c` and
  `files.c`: those modules did not move.
- Minimised windows as icons, which the 3.1 look will have in grin (G2);
  here they go to the taskbar in every look.

## Part fifteen: Files sorts, and the uploads have a place (asked for 2026-10-29)

- [x] **Sorting** (`files.c`): View > Sort by Name, Type (the extension),
      Size or Date, and Ascending or Descending, with a check mark on the
      ones in use (the kit's `WK_CHECKED`: an item whose first byte it is).
      A click on a column's header sorts by that column, again reverses
      it, and an arrow on the header shows which. Folders come first
      either way, names sort without regard to case, and what the order
      leaves equal goes by name. Date is the VFS's modification order
      (`vfs_touch`), as C4IX keeps no clock every host has. One order for
      every Files window, kept as `sort N` in desktop.cfg (N is the key
      times two, plus one for descending).
- [x] **The uploads in /home**: the page's Uploads were laid over drive 0
      by name, so a program could open them, but no listing named them.
      `libjs/uploads.js` (worker.js, and test-desktop.mjs the same way)
      now also puts each on the drive as `home/NAME` with a line in
      `c4ix.sizes`, and the manifest's `home/: hostdir(home/)` makes them
      lazy files in `/home` (vfsload.c, `load_hostdirs`). Files, `ls` and
      the shell see them there; the bare names still open. /home is empty
      where there are none.
- [x] test-desktop.mjs: three uploads and a folder in /home, `ls` and `cat`
      on them, and each order, by the menu and by the headers, kept across
      closing Files.
