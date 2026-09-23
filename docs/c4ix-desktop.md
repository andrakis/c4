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
- Asks for a 1024x768 display, attaches the rings, and redraws the whole
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
- [ ] CDP gate: `desktop` from the C4IX shell, open a terminal from the
      Start menu, type `ps` in it, drag it, open a second, close one
- [ ] Screenshot checked by eye
