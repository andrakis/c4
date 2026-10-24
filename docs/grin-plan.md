# grin: a graphical interface for C4DOS (plan)

Status: plan only, written 2026-10-24 against `smp` at 6295e18. Nothing
here is built yet. Decisions marked **(ask)** are the user's to make
before the milestone that needs them (listed again under "Open
questions").

`grin` is to C4DOS what Windows 3.x was to MS-DOS: a program you start
at the `A>` prompt that takes over the screen, gives you windows, a
Program Manager, a File Manager and a few accessories, launches
programs, and hands the prompt back when you exit it.

## 1. Goal and scope

**In scope**

- `GRIN` at the `A>` prompt starts a windowed shell on libjs's display;
  File > Exit grin returns to the prompt with the console unchanged.
- A window manager in the Windows 3.1 style: overlapping windows with a
  control-menu box, minimise/maximise buttons, a thick sizing border,
  minimised windows as icons on the desktop, and a Task List
  (Ctrl+Esc, or a double click on the desktop).
- **Cooperative multitasking of grin applications.** Several can be
  open at once (two Notepads, a Clock, a Calculator), all driven by
  grin's single event loop, the way Windows 3.x ran Windows programs on
  a single-tasking DOS.
- **Launching ordinary C4DOS programs**, one at a time, each run in one
  of three ways: full screen, with grin redrawing when it returns; in a
  window, for the four games, through a small grin back-end in their
  platform layers; or in a text window, for programs linked with libc's
  `bare.c`.
- First applications: Program Manager, File Manager, Notepad, Clock,
  Calculator, and the four games (Wolfenstein 3D, Doom, Tyrian, Rise of
  the Triad) on the Program Manager.
- Tests: a headless suite in the shape of `libjs/tests/test-desktop.mjs`,
  and a browser check over CDP for each milestone.

**Out of scope** (for this plan)

- Preemption, protection, or running two C4DOS programs at once. A
  crashing grin application takes grin down, as a crashing Windows 3.0
  program could take Windows down.
- c4bb and native c4m. They have no display, so grin says so and
  returns (`gui_present()` is 0 there).
- Making grin applications run under the C4IX desktop as well. One API
  for both would be possible later. It is not a goal here.
- A bitmap font of our own, a printer, OLE or DDE.

## 2. What grin is built on

These are the facts that shaped the design. Each was read from the
code, not assumed.

**C4DOS** (`src/c4dos/c4dos.c`, `docs/c4dos-design.md`)

- One task, no traps, no preemption. A program is a `.c4r` image that
  C4DOS loads into its heap, calls through the invoke stub
  (`run_trans`), and frees when `main` returns or the program calls
  API slot 1. Slot 1, `dos_exit`, unwinds through a double-`LEV`
  trampoline into `run_trans`'s frame.
- **RUN is not re-entrant.** The loader keeps its state in globals
  (`img_*`, `g_run_bp`, `g_in_trans`, the 4 MB `g_scratch` that the
  constructor and destructor tables point into), and the API table has
  no "run a program" slot. A transient that wants to run another
  program has to load it itself, as `dosload.c` does.
- The API table (`include/c4dos.h`, v3) offers open/read/close, RAM-disk
  create/write/enumerate, trim/release, and the clock. It is 32 words,
  zeroed, and slots 16 and up are free.
- Files: the disk is read through `c4dos.dir`, a list of names (no
  sizes). There is no directory service beyond it, and no delete or
  rename. Writes go to the RAM disk (`DEVICE=RAMDISK.SYS`), which is
  lost at power-off. With `DEVICE=DRIVES.SYS` there are drives B:, C:
  and so on.
- There is no TSR or keep-resident mechanism, and `SHELL=` is reserved.
  The libjs page boots C4DOS on the games disk at 400 MHz with 64 MB
  (`libjs/web/app.js:37`). The heap is everything below a single 1 MB
  stack, which DOS and every nested program share.

**The display** (`libjs/guest/gui.h`, `libjs/gui-device.js`)

- A device at 0x400-0x47F, announced by `__c4_info() & C4I_GUI`. Drawing
  is a command stream in a guest memory ring: `gui_rect`, `gui_recto`,
  `gui_line`, `gui_disc`, `gui_text2` (host fonts: mono, sans, sans bold,
  any size, optional fixed advance), `gui_imgdef`/`gui_image` (images
  cached by the host), `gui_blit8` (8-bit pixels plus a palette, scaled),
  `gui_clip`, and `gui_show`. Events come back through a second ring:
  mouse down/up/move/wheel in display pixels, and key down/up as
  `[keyCode, char, mods]`.
- The canvas is retained, so a damage-only redraw works. There is no
  screen-to-screen copy, no readback, and **no way to measure text**.
  The host draws the pointer, so `gui_cursor` is all a guest needs.
- One ring at a time. Writing `GUI_RING` takes the display from whoever
  had it.
- The sound card (0x480-0x4B8, `snd.h`) has OPL, 8 PCM channels and the
  speaker. The games drive it directly.

**The games under C4DOS** (`src/{wolf3d,doom,rott,opentyrian}`)

- All four are ordinary C4DOS transients, built by c4lc and linked whole
  with libc's `bare.c` layer. Each attaches its own 64 KB ring at
  640x480, blits a 320x200 8-bit frame with `gui_blit8` scaled 2x, uses
  the sound card directly, and exits through `exit()`, which runs
  `atexit` (detaching the display) and then calls DOS slot 1.
- Each already has a window back-end for the C4IX desktop
  (`plat-c4ix.c`, `dg-c4ix.c`, `rott-c4ix.c`, `tyr-c4ix.c`), built on
  `win_open`/`win_fb`/`win_present`/`win_poll`. The bare versions of
  those functions return 0, so `--window` under C4DOS means full screen
  today.
- Memory: Doom takes about 15 MB (a 6 MB zone and the WAD read whole),
  ROTT about 17 MB. **Nothing frees a program's heap when it exits.**
  C4DOS frees only the image's code and data, so every game run leaks
  everything it allocated. Doom or ROTT a few times in a row would run
  a 64 MB machine out. (Inferred from the code; not yet measured. G0
  measures it.)

**The C4IX desktop** (`src/c4ix/user/desktop/`, 8,500 lines)

- It draws only through gui.h, so its drawing layer, widgets, dialogs
  and menus work on any machine with the display. What is
  C4IX-specific is the process plumbing: pipes, `spawn`, signals,
  `utaskinfo`, `uevwait`, and the `window.h` escape-sequence protocol a
  windowed program speaks over its terminal's pipes.
- Its tools are in-process "kinds" with draw/mouse/key/command/tick
  hooks, dispatched by if-ladders in `wm.c`. Every window allocates
  72 KB of state whatever it needs. The code review
  (`c4ix-desktop-review.md`, in the session notes) proposes a
  `struct tool` table of function pointers, with a `.size` per kind.
  grin starts from that shape.

## 3. Architecture

### 3.1 The process model

grin is **one C4DOS transient, `grin.c4r`**, built by c4lc at 32 bits
and linked with libc's `bare.c`. While it runs, three kinds of code run
inside it:

| kind | what it is | how it runs | example |
|---|---|---|---|
| built-in | linked into `grin.c4r` | called from the event loop | window manager, Program Manager, File Manager, Task List, dialogs |
| **grin application** | a `.c4r` that has a `__grin_api` symbol and returns `GRIN_RESIDENT` from `main` | loaded into grin's heap and kept there. Its windows' callbacks run from grin's loop. Any number at once, cooperatively | Notepad, Clock, Calculator |
| **DOS program** | any other `.c4r` | loaded by grin's own loader and run nested, **one at a time**, until it returns or exits | the games, `hello`, the compilers |

This is the Windows 3.x split. Windows programs were event-driven and
shared one message loop. DOS programs got the machine, either full
screen or (in 386 enhanced mode) in a window. Here the DOS program in a
window keeps grin's applications alive by pumping grin's loop from its
own input poll (3.6).

**Why not the alternatives**

- *Everything built in*, as the C4IX desktop does: simplest at first,
  but every accessory grows the one image, and nothing outside the grin
  tree can add a window. Built-ins are kept for the shell itself.
- *Run each program full screen and return* (DOSSHELL's task swapper):
  needs only the loader, and it is the fallback path (3.5). On its own
  it gives no multitasking at all, and a clock that stops whenever you
  open Notepad is not Windows.
- *Preemptive multitasking* would need traps and a scheduler. That is
  C4KE's rung, not DOS's.

### 3.2 grin's loader, and why it owns a copy of the DOS API

grin cannot call C4DOS's RUN, which is not re-entrant. So it has its
own loader, `src/grin/load.c`, modelled on `dosload.c`:

- It reads the image into a doubling buffer, not DOS's 4 MB scratch, so
  ROTT (3.57 MB) and larger images load. It copies out code, data and the
  constructor and destructor tables, then frees the buffer.
- It applies the patches and injects two symbols: `__c4dos_api` and
  `__grin_api`.
- **`__c4dos_api` gets grin's copy of DOS's table, not DOS's own.**
  grin copies DOS's 32 words at start-up and changes slot 1 (`exit`)
  to grin's own `grin_exit`. Without that, a game's `exit()` would
  unwind through C4DOS's trampoline into C4DOS's `run_trans` frame, and
  grin would vanish along with the game. With it, `grin_exit` uses the
  same double-`LEV` trick (`tramp_mark`, `learn_trampoline`,
  `run_trans`, `dos_exit`, copied from `c4dos.c:243-272, 622-630`) to
  land in **grin's** run frame. The frame is recorded on a small stack,
  since a grin application's callback can also call `exit()` (3.4).
- It runs the constructors, honouring `C4DOS_REFUSE`, then `main`, then
  either keeps the image (a grin application returned `GRIN_RESIDENT`)
  or runs the destructors and frees it.

### 3.3 Memory: heap generations (a machine change)

The leak above is what makes "launch a game, quit, launch another" fail
after a few rounds. This is the only change grin needs below C4DOS.

- `libjs/heap.js` gains a **generation tag**: every block records the
  generation that was current when it was allocated. Two device
  registers sit next to `HEAP_USED` (0x1c0) in `src/c4bb/sim/devices.js`:
  - `HEAP_GEN` (r/w): the current generation.
  - `HEAP_RELEASE` (w): free every block of the generation written.

  Both are announced by `C4I_C4MJS` exactly as `HEAP_USED` is, and read
  0 where the host does not keep the heap. 0x1c4 and 0x1c8 look free,
  to be confirmed when G0 starts.
- grin sets a fresh generation before loading a program and restores
  its own afterwards. When a DOS program ends, or a grin application's
  last window closes, grin writes `HEAP_RELEASE` for that generation.
- **grin's own allocations must never land in a child's generation.**
  Every grin API entry point brackets itself with
  `g = gen_enter(); ... gen_leave(g);`, and so does the loop pump a DOS
  program calls. A test pins it: open a dialog while Doom runs
  windowed, quit Doom, and use the dialog.
- Where the registers are missing, grin still runs and says in About how
  much memory it could not get back.

### 3.4 The application API: `grin.h`

A grin application is c4lc-built: the API is function pointers, which
c4cc cannot call. It is 32-bit, linked with `bare.c`, and includes
`src/grin/grin.h`. The loader injects one pointer:

```c
// grin.h -- a grin application's view of grin (sketch; names may change)
enum { GRIN_MAGIC = 0x4752494e, GRIN_RESIDENT = 0x47524e52, GRIN_VERSION = 1 };

struct grin_class {                 // one per kind of window an app makes
  int  size;                        // bytes of per-window state, zeroed
                                    // (set at run time: c4lc has no
                                    // sizeof(struct) in a constant)
  void (*open)(int w, char *arg);
  void (*draw)(int w, int cw, int ch, int focused);   // client coordinates
  void (*mouse)(int w, int ev, int x, int y, int button);
  void (*key)(int w, int down, int code, int ch, int mods);
  void (*command)(int w, int id);                     // menu item / accelerator
  void (*timer)(int w, int id);
  void (*answer)(int w, int dialog, int result);      // a message box closed
  int  (*close)(int w);                               // 0 vetoes ("Save changes?")
  void (*destroy)(int w);
};

struct grin_menu { char *title; char **items; int *ids; };  // "-" = separator

struct grin_api {
  int magic, version, size;
  // windows: handles are small ints that are never reused, not pointers
  int   (*open)(struct grin_class *c, char *title, int w, int h, int flags, char *arg);
  void  (*close)(int w);
  void *(*state)(int w);
  void  (*title)(int w, char *s);
  void  (*invalidate)(int w, int x, int y, int ww, int hh);  // client rect; ww=0: all
  void  (*menu)(int w, struct grin_menu *m, int n);
  int   (*timer)(int w, int id, int ms);                     // ms=0 stops it
  // drawing: only inside draw(); client-relative, clipped to the client
  void  (*rect)(int x, int y, int w, int h, int rgb);
  void  (*frame)(int x, int y, int w, int h, int rgb);
  void  (*line)(int x0, int y0, int x1, int y1, int rgb);
  void  (*disc)(int x, int y, int r, int rgb);
  void  (*text)(int x, int y, int rgb, int font, char *s);
  void  (*textn)(int x, int y, int rgb, int size, int font, int adv, char *s, int n);
  void  (*bevel)(int x, int y, int w, int h, int style);
  void  (*icon)(int which, int x, int y);
  void  (*blit8)(int x, int y, int w, int h, char *px, int pitch, int *pal, int dw, int dh);
  // widgets (ui.c's, stateless: the app owns the state)
  int   (*button)(int x, int y, int w, int h, char *label, int pressed, int deflt);
  int   (*hit)(int px, int py, int x, int y, int w, int h);
  void  (*field)(int x, int y, int w, char *text, int caret, int focused);
  int   (*field_key)(char *text, int max, int *caret, int code, int ch, int mods);
  void  (*scrollbar)(int x, int y, int h, int total, int visible, int top);
  int   (*scroll_click)(int px, int py, int x, int y, int h, int total, int visible, int top);
  // dialogs: modal to the owner; the result comes back through answer()
  void  (*msgbox)(int owner, int dialog, char *title, char *text, int buttons);
  void  (*inputbox)(int owner, int dialog, char *title, char *prompt, char *buf, int max);
  void  (*filebox)(int owner, int dialog, int save, char *pattern, char *buf, int max);
  // system
  int   (*exec)(char *cmdline, int how);    // GX_FULL, GX_WINDOW, GX_TEXT; deferred
  int   (*ticks)();                         // ms
  int   (*clip_set)(char *s, int n);
  char *(*clip_get)(int *n);
  // a DOS program's window (3.6); 0 in a grin application's table
  int   (*fwin_open)(char *title, int w, int h);
  void  (*fwin_frame)(char *px, int pitch, int *pal);
  int   (*fwin_poll)(int *ev);              // runs grin's loop once; -1: closed
  void  (*con_write)(char *s, int n);       // the text window (3.7)
  int   (*con_read)(char *buf, int n);
};
extern struct grin_api *__grin_api;
```

A complete application:

```c
#include "grin.h"
struct clock { int last; };
static void clock_draw(int w, int cw, int ch, int focused) { ... }
static void clock_tick(int w, int id) { __grin_api->invalidate(w, 0, 0, 0, 0); }
static struct grin_class clock_class = { .draw = clock_draw, .timer = clock_tick };

int main(int argc, char **argv) {
  int w;
  if (!__grin_api) { printf("This program requires grin.\n"); return 1; }
  clock_class.size = sizeof(struct clock);
  w = __grin_api->open(&clock_class, "Clock", 160, 180, 0, 0);
  __grin_api->timer(w, 1, 1000);
  return GRIN_RESIDENT;
}
```

**Rules**

- An application never blocks or loops waiting for input. Everything
  happens in callbacks, and each callback returns promptly. That is the
  cooperative contract, exactly Windows 3.x's.
- Unset callbacks are 0, and grin checks before calling (the review's
  M1 note).
- An application stays loaded while it has a window open. When the last
  one is destroyed, grin runs its destructors, releases its heap
  generation and frees its image. A second launch of an application
  that is already loaded calls its `main` again on the same image (the
  `hPrevInstance` model), so per-window state belongs in the window,
  not in globals.
- `exit()` from inside a callback closes all of that application's
  windows and unloads it. grin records a trampoline frame around every
  callback it makes into an application.
- File I/O is ordinary libc. `bare.c`'s `open`/`read`/`write` go
  through `__c4dos_api`, which is grin's proxy of DOS's table, so they
  reach DOS's disk and RAM disk.
- `exec` is **deferred**. It queues the request, and grin's top-level
  loop runs it after the callback returns, so a DOS program never runs
  nested inside an application's callback.

### 3.5 The event loop

```
loop:
  sweep closed windows; unload applications with no windows (release generation)
  while gui_poll(ev): dispatch (mouse to window or chrome, keys to focus,
                      accelerators, Alt+Tab, Ctrl+Esc; resize; focus)
  fire due timers
  if a deferred exec is queued and no DOS program runs: run it (3.6)
  redraw: the damage rectangle, or everything (wm.c's redraw / redraw_part)
  idle: __c4_usleep(until the next timer, at most 50 ms); a display event
        cuts the nap short (worker.js:296-318)
```

`grin_pump()` is one pass of this loop without the exec step. It is
what a windowed DOS program's `fwin_poll` and `con_read` call.

### 3.6 Launching DOS programs

Each Program Manager item and each File Manager "Run" names a mode. The
item properties hold it, like a PIF:

- **Full screen** (the default for a program grin knows nothing about):
  grin hides its pointer, calls `gui_detach`, and runs the program with
  the display free. When the program returns, grin re-attaches its ring
  (`gui_attach` again, since the program took and dropped
  `GUI_RING`), sets its size, event mask and cursor again, calls
  `snd_reset()`, releases the program's heap generation, and redraws
  everything. This is the C4IX desktop's `udisplay_give/take` dance with
  no kernel in it.
- **Window** (the four games, once G5 is done): the program's bare
  platform layer finds `__grin_api` non-zero and uses
  `fwin_open`/`fwin_frame`/`fwin_poll` in place of the full display.
  - It never calls `gui_attach`.
  - `fwin_frame` records the frame and damages the window. The blit
    goes out in the next redraw, and the host copies the pixels at the
    bell, so the bare path needs no double buffer.
  - `fwin_poll` calls `grin_pump()` and then returns the next event
    meant for the game's window, in gui.h's own form, with mouse
    coordinates translated into the frame's pixels.
  - While the game runs, the Clock still ticks and Notepad still takes
    typing, because the game is pumping grin's loop. Closing the
    window makes `fwin_poll` return -1, which the C4IX back-ends
    already treat as quit.
- **Text** (3.7).

**One DOS program at a time.** While one runs, a launch shows "Close
Doom before starting another program" (Windows 3.x standard mode said
much the same). Exit grin is refused while a DOS program runs. A
`c4cc`-built program, which prints with `PRTF` and reads with the
`READ` opcode, cannot be captured. Its "text" mode shows a small window
saying the program is running in the console pane, and grin pumps
nothing until the program returns.

### 3.7 Text programs in a window (C4DOS API v4)

`bare.c`'s `write(1|2, ...)` puts characters with `PUTC`, and
`read(0, ...)` reads the console. Two new DOS API slots make that
redirectable, and C4DOS fills both with its own console:

| slot | v4 |
|---|---|
| 16 | `conwrite(buf, n)` |
| 17 | `conread(buf, n)`, a line at a time |
| 18 | `unlink(name)`, RAM disk only (File Manager's Delete) |

`bare.c` uses slots 16 and 17 when the version is 4 or more. grin's
proxy table points them at a text window built from `term.c`'s grid,
line editor and scrollback, with the pipe and spawn code removed.
`conread` pumps grin's loop until a line is entered in that window.
`bare.c` already calls through the table for files (`dos1`/`dos3`), so
this is a few lines there.

The same text window, with a command line on it, is grin's
**"DOS Prompt"**. It reads a line, runs the C4DOS builtins it can (DIR,
TYPE, COPY, VER, MEM, TIME) and launches programs through grin's
loader.

### 3.8 The window manager and the look

`src/grin/wm.c` starts from the C4IX desktop's `wm.c`:

- **Kept**: the drawing layer (`d_*`), clipping to the client area,
  damage tracking (one bounding rectangle, `redraw_part`), the z-ordered
  window list, focus and modality, title-bar drag, corner resize, double
  click, menu bars and drop-downs, message and input boxes, pointer
  shapes, and colour schemes.
- **Changed for Windows 3.1**:
  - No taskbar and no Go menu.
  - Minimised windows become labelled icons along the bottom of the
    desktop.
  - The title bar is solid (navy active, white inactive) with a
    centred title.
  - A control-menu box sits at the left (Restore, Move, Size,
    Minimize, Maximize, Close, Switch To...).
  - Raised minimise and maximise arrow buttons sit at the right, with
    no close button.
  - A thick sizing border goes all round.
  - The Task List (Switch To, End Task, Cascade, Tile, Arrange Icons)
    comes up on Ctrl+Esc or a desktop double click.
- **Dispatch through `struct grin_class`** from day one: one table, no
  if-ladders. Built-ins are classes too. State is `class->size` bytes,
  not 72 KB.
- **Window handles are ints that are never reused.** A stale handle
  finds nothing, which is the cure for the review's dangling `feeding`
  pointer (B5).

**Screen**: 640x480, the VGA size Windows 3.x was drawn for. The games'
2x frame then fills the screen exactly in full-screen mode, and a 1x
(320x200) game window fits beside other windows. 800x600 is a
`GRIN.INI` setting. **(ask)**

**Fonts**: the host's, through `gui_text2`. Sans bold is the "System"
font of titles, menus and buttons, sans is for dialog text, and mono is
for Notepad and the text window. There is no text measurement, so
proportional widths are estimated as the C4IX desktop does (`len * 6`,
`len * 7`). This is the main layout risk (section 7).

**Icons**: procedural, from rectangles and lines, as `d_icon` does. A
32x32 icon set for Program Manager can later be cached host-side
through `gui_imgdef` (`GUI_RING` of 64 KB carries about 8,000 pixels a
frame, so 32x32 fits easily).

### 3.9 Configuration

- `GRIN.INI`: screen size, colours, and the startup group.
- `PROGMAN.INI`: groups and items, each item `title, command line,
  icon, mode`.

Both are read with DOS open and written to the RAM disk. Changes are
lost at power-off unless the machine has a writable drive **(ask)**.

## 4. What is reused from the C4IX desktop

| C4IX file | in grin | how |
|---|---|---|
| `ui.c` (300) | `src/grin/ui.c` | as is. It needs only `d_*` and the colour names. Exported to applications through the table |
| `wm.c` (1412) | `src/grin/wm.c` | the drawing layer, damage, z-order, menus and dialogs are kept. Removed: the taskbar, Go menu, `udisplay*`, `uevwait`, `spawn`, the `persist` save, and the kind if-ladders (replaced by `struct grin_class`). Added: Windows 3.1 chrome, icons for minimised windows, the Task List, the file dialog |
| `calc.c` (495) | `calc.c4r`, an application | its state and keys are portable. Its draw/mouse/key become class callbacks. It needs `fp.c` (already in libc) |
| `apps.c` Text Editor (part) | `notepad.c4r` | the buffer and editing are kept. File I/O goes through libc, with no 64,999-byte truncation (review B6), and it gets Open/Save As through `filebox`, Edit > Copy/Paste through the clipboard, and Search > Find |
| `edit.c` (2460) | not in the first cut | the editor core is portable, and `:make` and `:run` are C4IX-only. A later "Programmer's Editor" could bring it over |
| `term.c` (820) | `src/grin/textwin.c` | the VT100 grid, scrollback, line editor and row cache are kept. The pipes, `spawn`, signals and the `window.h` protocol are removed |
| `savers.c` (314) | screen saver, G6 | as is |
| `files.c` (722) | ideas only | its tree and list drawing and multi-select are reused. The data side is new, because C4DOS has drives, `c4dos.dir` and the RAM disk, not a tree |
| `taskmgr.c` (541) | no | all `utaskinfo`. The Task List and About's memory line replace it |
| `wall.c` | maybe, G6 | needs libarc |

**Fork, not share, at first.** **(ask)** The C4IX desktop is also built
inside C4IX by c4sc (`BUILD.B4K`, with a per-file memory budget), and
sharing source would tie grin's changes to that build. Copy into
`src/grin/` with a header line naming the origin. Once both have
settled, move `ui.c` and the `d_*` layer into a common directory.

**Don't copy the review's bugs.** Use a single `PATH_MAX` and bounded
copies (B3), check every allocation (B8), store the dialog flag apart
from its text (B7), and give each input box its own buffer rather than
a shared global (B10).

## 5. The first applications

- **Program Manager** (built in): the shell window. In the first cut it
  shows one group at a time with a group list. Real MDI group windows
  come in G6. Its items come from `PROGMAN.INI`, and the default groups
  are:
  - **Main**: File Manager, DOS Prompt, Notepad.
  - **Accessories**: Clock, Calculator.
  - **Games**: Wolfenstein 3D, Doom, Tyrian, Rise of the Triad.

  Double click or Enter launches. File > New/Properties edits an item,
  including its run mode. File > Run... takes a command line. File >
  Exit grin asks "This will end your grin session" and returns to `A>`.
- **File Manager** (built in): a drive bar (A: and, with `DRIVES.SYS`,
  every drive `DISK_COUNT` reports, plus RAM:) and the file list.
  - The list comes from `c4dos.dir` for a drive and DOS slots 9-12 for
    the RAM disk. Disk sizes are fetched when a file is selected, not
    for the whole listing, because the disk has no size query short of
    reading the file.
  - Double click runs a `.c4r` (asking full screen or window the first
    time), opens a `.txt`/`.c`/`.bat`/`.ini` in Notepad, and runs a
    `.bat` in the DOS Prompt.
  - Copy works to the RAM disk. Delete works on the RAM disk only
    (slot 18).
- **Notepad** (`notepad.c4r`): as in section 4.
- **Clock** (`clock.c4r`): analog and digital, with a Settings menu. Its
  face is drawn with `line`/`disc` and its time read from `GUI_CLOCK`.
  It is the smallest application, and the one G2 uses to prove the
  loader.
- **Calculator** (`calc.c4r`): Standard first; the Integer, Single and
  Double modes come over from `calc.c`.
- **Games**: Program Manager items, full screen in G4 and windowed in G5.

## 6. Milestones

Each milestone ends with a headless test and a browser check.

- **The headless suite** is `libjs/tests/test-grin.mjs`, modelled on
  `test-desktop.mjs`.
  - It boots `c4dos32.c4r` on the games disk with a `GuiDevice` and
    types `GRIN` on the console.
  - It clicks things by the text drawn on them, read from the last
    presented frame.
  - It runs from `make test-grin` and joins `test-libjs.sh` once it is
    stable.
- **The browser check** uses `libjs/tests/cdp.mjs` against the user's
  Chrome, in a new tab, on `.../libjs/web/?system=grin`.
  - The page gets a `grin` entry: C4DOS with `/K GRIN` (G0).
  - Each check reads back the strings drawn (`display.recordText`), and
    a screenshot is looked at by eye.
  - Until the branch is merged, the page is served from the branch's
    own worktree.

**G0: groundwork and the loader**

- `src/grin/` with `grin.c` and `load.c`. `grin.c4r` attaches a 640x480
  display, paints the desktop, and returns to `A>` on Esc. It is built
  by `build-images.sh` onto the games disk.
- `load.c`: the proxy API table, `grin_exit` and its trampoline, and the
  doubling read buffer.
- C4DOS: `/K command` in its argv runs one command after
  `AUTOEXEC.BAT`. `app.js` gets a `grin` system.
- Measure the leak: `HEAP_USED` before and after running `doom.c4r`
  through grin's loader.
- Test, headless:
  - `GRIN` then Esc: `A>` comes back, and `VER` still answers.
  - grin runs `hello.c4r`, and a test program that calls `exit(3)`
    from three calls deep. grin survives both and sees status 3.
  - grin runs a program with a constructor that refuses.
- Browser: `?system=grin` boots straight to the grin desktop. Esc
  gives the prompt in the console pane.

**G1: the window manager**

- `wm.c`, `ui.c` and the drawing layer, ported. Windows 3.1 chrome,
  menus, the control menu, minimise to an icon, maximise, size, move,
  the Task List, message boxes, and the About box.
- Built-in classes only, with two dummy windows.
- Test, headless:
  - Drag a window by its title and find the title drawn at its new
    place.
  - Minimise one: its icon label appears, and a double click on the
    icon restores it.
  - Maximise one, then restore it.
  - Close one through its control menu.
  - Ctrl+Esc lists the windows, and Switch To raises one.
  - A message box is modal: a click outside it changes nothing.
- Browser: the same by hand, plus a screenshot.

**G2: the application ABI**

- `grin.h`, the API table, resident applications, timers, `filebox`, and
  exit from a callback.
- Heap generations: `HEAP_GEN` and `HEAP_RELEASE` in `heap.js` and
  `devices.js`, with `libjs/tests/test-heap.mjs` extended.
- Clock and Calculator.
- Test, headless:
  - Open two Clocks: both tick, since the drawn seconds change on both.
  - Close one: the other ticks on.
  - Close both: `HEAP_USED` returns to within a few hundred bytes of
    where it started.
  - Typing `12*12=` into Calculator shows `144`.
  - An application that calls `exit()` from its key callback is
    unloaded, and grin keeps running.
- Browser: Clock and Calculator side by side.

**G3: the shell**

- Program Manager with `PROGMAN.INI` and the three groups, and File >
  Run....
- File Manager over `c4dos.dir` and the RAM disk.
- Notepad.
- The DOS API v4 slots in C4DOS: 16 and 17, plus 18 for File Manager's
  Delete. `include/c4dos.h` and `docs/c4dos-design.md` updated.
- Test, headless:
  - Program Manager shows its groups and items.
  - File Manager lists `doom.c4r` on A:.
  - Notepad opens `vendor.doc` and shows its first line. A file typed
    in Notepad and saved as `NOTE.TXT` appears in File Manager under
    RAM: and reads back with `TYPE` after exiting grin.
  - Delete removes it.
  - `make test-c4dos` stays green (API v4 is additive).
- Browser: the whole shell, plus a screenshot.

**G4: DOS programs**

- Full-screen launch with display hand-back, `snd_reset` and heap
  release.
- The text window and the DOS Prompt, with `bare.c` on slots 16 and 17.
- Test, headless:
  - Launch Doom full screen: frames are blitted. Quit it with keys:
    grin redraws, and Program Manager's text is back.
  - Launch Doom five times in a row: `HEAP_USED` after each is within
    1% of the first.
  - A `bare.c` test program that prints and reads a line runs in a
    text window. The typed line reaches it, and its output is drawn
    there, not on the console.
  - A `c4cc` program prints on the console with grin's notice up.
- Browser: Wolfenstein 3D full screen from Program Manager, play a
  little, quit, and the desktop is back. Then the same with Tyrian
  (pointer lock).

**G5: games in a window**

- A grin back-end in each bare platform (`plat-bare.c`, `dg-bare.c`,
  `rott-bare.c`, `tyr-bare.c`). These are the C4IX `*_win_*` trios
  re-pointed at `fwin_*`, about 50 lines each.
- `__grin_api` is declared in `bare.c`, so every bare program can be
  told.
- 1x and 2x window sizes. Keys go to the game only while its window has
  focus. Pointer lock is granted on a click into the window and released
  on Esc.
- Test, headless:
  - Doom windowed: frames arrive in the window's rectangle, and the
    Clock's seconds still advance while Doom runs, which is the
    cooperative proof.
  - Closing Doom's window ends it, and grin carries on.
  - The same for Wolf3D, Tyrian and ROTT, each with one frame and a
    close.
- Browser: Doom in a window beside Clock and Notepad, typing in
  Notepad while Doom's demo plays, plus a screenshot.

**G6: finishing**

- MDI group windows in Program Manager, Cascade/Tile, the screen saver
  (`savers.c`), Control Panel (colours, desktop pattern), the clipboard
  across applications, a startup chime on the OPL, and `GRIN.INI`
  persistence if a writable drive exists.
- Test: each feature gets a headless check, and `docs/grin.md` replaces
  this plan as the design and status document.

Rough size, by analogy with the C4IX desktop: G0 400 lines, G1 1,800
(mostly ported), G2 900, G3 1,500, G4 700, G5 300, G6 1,200.

## 7. Risks

- **The exit trampoline.** It depends on the exact frame shape c4lc
  emits, and `-O` has bitten this mechanism before (the `invoke0`
  tail-call bug, `c4dos.c:225-232`). G0 pins it with a test that exits
  from several call depths, at `-O` and without.
- **Heap generations get misattributed.** If grin allocates while a
  child's generation is current, releasing the child frees grin's
  memory. Every API entry point brackets the generation, and G4/G5 test
  dialogs opened during a game. If heap generations are refused
  **(ask)**, games leak and grin can only warn.
- **No protection.** An application or game that scribbles on memory
  takes grin down, and there is no recovering. That is accepted, as it
  was for Windows 3.0.
- **One shared 1 MB stack.** A DOS program runs on top of grin's frames.
  The games already share it with C4DOS, so the extra depth is grin's
  dispatch, a few hundred words.
- **Text widths are guesses.** Host fonts cannot be measured from the
  guest, so labels may clip or leave gaps. Mitigations, in order:
  generous fixed layouts; fixed-advance text where alignment matters;
  and, if it is not good enough, a synchronous measure register in
  `gui-device.js`. The worker cannot see the page's fonts, so that
  last needs thought.
- **Re-entrancy while a game pumps the loop.** An application callback
  can run in the middle of a game's frame. grin forbids starting a DOS
  program from inside a pump, and refuses Exit while one runs. An
  application that is closed while the game pumps is unloaded only at
  top level.
- **Speed.** At 400 MHz the games run close to their frame caps
  (ROTT about 32 of 35 fps). Windowed play adds a pump and a damage
  redraw per frame, a few thousand instructions, but windows on top of
  the game are redrawn every frame. G5 measures frames per second for
  each game, both windowed and full screen.
- **Keyboard focus is the browser's.** Keys reach the display only when
  the canvas has focus. After typing `GRIN` in the console pane, the
  user has to click the canvas. The `?system=grin` entry avoids that.

## 8. Open questions for the user

1. **Look.** Windows 3.1 (Program Manager, icons on the desktop, no
   taskbar), as planned, or the C4IX desktop's Windows 95 look
   reused as it is?
2. **Screen size.** 640x480 (planned) or 800x600 like the C4IX desktop?
3. **Fork or share** the C4IX desktop's `ui.c` and drawing layer? The
   plan forks now and unifies later.
4. **Machine and DOS changes.** Are these acceptable?
   - Heap generations in `heap.js` and `devices.js`.
   - C4DOS API v4 (console slots and RAM unlink).
   - `/K` in C4DOS's argv.
   - Console hooks and `__grin_api` in libc's `bare.c`.

   Without heap generations, repeated game launches leak.
5. **Booting.** Should `?system=grin` boot straight into grin, with
   `?system=c4dos` left at the prompt? Or should the games disk's
   `AUTOEXEC.BAT` start grin, the way `WIN` often ended an
   AUTOEXEC.BAT?
6. **Persistence.** Files saved in Notepad live on the RAM disk and are
   gone on reload. Is that fine for now, or should the page give C4DOS
   a writable drive kept in the browser (as `persist` does for C4IX)?
7. **c4cc programs** print to the console pane, not a window. Is that
   acceptable, or should c4cc's `PRTF` path be redirectable too?
   That would be a machine change.
8. **The name of the command.** `GRIN` (planned), or `WIN` for the
   nostalgia, with `GRIN` as an alias?
