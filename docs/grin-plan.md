# grin: a graphical interface for C4DOS (plan)

Status: written 2026-10-24 against `smp` at 6295e18, and revised the
same day with the user's answers (section 8). **G0 and G1 are built**
(branches `grin-g0` and `grin-g1`, merged here on 2026-10-24), and
**G2 is built** (branch `grin-g2`, 2026-10-24), **G3** (branch `grin-g3`,
2026-10-25), **G4** (branch `grin-g4`, 2026-10-25), **G5** (branch
`grin-g5`, 2026-10-26) and **G6** (branch `grin-g6`, 2026-10-27).
Sections 3.2 and 3.3 say what G0 built where it differs from the first
draft, 3.4 shows the window kit's API as G1 built it and G3's mechanism,
3.6 and 3.7 say what G5 and G6 built, 3.8 and 3.9 what G2 and G4 built,
and so do the milestones. The rest is plan.

`grin` is to C4DOS what Windows 3.x was to MS-DOS: a program started
from `AUTOEXEC.BAT` that takes over the screen and gives you windows, a
Program Manager, a File Manager and a few accessories. It launches
programs, including DOS command prompts in windows, and hands the `A>`
prompt back when you exit it.

## 1. Goal and scope

**In scope**

- **Booting into grin.** The libjs page boots C4DOS from a disk of its
  own, whose `AUTOEXEC.BAT` ends with `GRIN`. Exiting grin leaves you at
  `A>`, and typing `GRIN` there starts it again.
- **A Windows 3.1 window manager**:
  - overlapping windows with a control-menu box, minimise and maximise
    buttons, and a thick sizing border;
  - minimised windows shown as icons on the desktop;
  - a Task List.
- **640x480 and 800x600**, switched at run time from the Control
  Panel's Display item and remembered.
- **Cooperative multitasking of grin applications.** Several can be
  open at once, all driven by grin's single event loop, as Windows 3.x
  ran Windows programs on a DOS that did one thing at a time.
- **Launching ordinary C4DOS programs**, one at a time, in one of three
  ways:
  - full screen, with grin redrawing when the program returns;
  - in a window, for the four games;
  - in a **DOS Prompt window**. This is C4DOS's own command
    interpreter, with its output and input redirected into the window.
- **File redirection in C4DOS** (`>`, `>>`, `<`, `|`), switched on by
  `DEVICE=REDIR.SYS`. It works at the plain `A>` prompt as well as in
  grin.
- **Persistence.** Files saved under C4DOS go to a writable drive that
  the page keeps in the browser. Each bare-machine game page gets the
  same, so a game's saves and settings survive a reload whether it ran
  under C4DOS or on its own. Every system has a store of its own, so a
  later system such as the ladder (C4DOS, then C4KE, then C4IX, each
  built by the one before) can have its own as well.
- **One body of GUI code, shared with the C4IX desktop.** The drawing
  layer, widgets, window manager core, the looks, and the Calculator,
  Notepad, Clock and terminal grid are written once and built into both.
- **The first applications**: Program Manager, File Manager, Notepad,
  Clock, Calculator, Control Panel, DOS Prompt, and the four games
  (Wolfenstein 3D, Doom, Tyrian, Rise of the Triad).
- **Tests**: a headless suite in the shape of
  `libjs/tests/test-desktop.mjs`, and a browser check over CDP for every
  milestone.

**Out of scope**

- Preemption, protection, or two C4DOS programs running at once. The
  multitasking is cooperative throughout, as the user expects. A
  crashing application can take grin down, as one could take down
  Windows 3.0.
- grin on c4bb or native c4m. Neither has a display, so grin says so
  and returns. The C4DOS changes in G0, G4 and G5 do run there, and
  keep `make test-c4dos` and its purity pins green.
- A bitmap font of our own, printing, OLE and DDE.

## 2. What grin is built on

The facts that shaped the design were read from the code, not assumed.

**C4DOS** (`src/c4dos/c4dos.c`, `docs/c4dos-design.md`)

- **One task, no traps, no preemption.** A program is a `.c4r` image that
  C4DOS loads into its heap, calls through the invoke stub
  (`run_trans`), and frees when `main` returns or the program calls API
  slot 1. Slot 1, `dos_exit`, unwinds through a double-`LEV` trampoline
  into `run_trans`'s frame.
- **RUN is not re-entrant.** The loader's state is in globals: `img_*`,
  `g_run_bp`, `g_in_trans`, `g_line`/`g_argv`, and the 4 MB `g_scratch`
  that the constructor and destructor tables point into. The API table
  has no "run" slot.
- **The API table** (`include/c4dos.h`, v3) is 32 words, zeroed, with
  slots 16 and up free. It offers open/read/close, create/write and
  enumeration on the RAM disk, trim/release, and the clock.
- **Files.** The disk is listed by `c4dos.dir`, which holds names only.
  Writes go to the RAM disk, which is lost at power-off. With
  `DEVICE=DRIVES.SYS` there are more drives (B:, C:, ...), and c4bb's
  disk controller can write to a writable one (`DISK_WNAME`..`DISK_WCLOSE`;
  `bbsave` and `install` use those registers), but DOS's own API never
  does.
- **Devices are announced, never probed.** Each `DEVICE=` line in
  `CONFIG.SYS` declares hardware that DOS cannot safely probe for,
  because under native c4m a device address is ordinary memory.
- **No redirection, by an earlier decision (2026-08-21).** Section 3.7
  revisits it, keeping today's behaviour as the default.
- **Memory.** There is no TSR mechanism. The heap is everything below a
  single 1 MB stack, which DOS and every nested program share.

**The libjs page** (`libjs/web/app.js`, `libjs/page.js`, `libjs/worker.js`)

- `?system=c4dos` boots `c4dos32.c4r` on `src/c4bb/images/games` at
  400 MHz with 64 MB. The bare-machine game systems use the same disk.
- **Persistence already exists for C4IX.** `C4M.boot({ persist: NAME })`
  gives the machine a writable drive 1 (B:). Every file closed on it is
  posted to the page, which keeps it in IndexedDB under NAME and hands
  the files back at the next boot. `?persist=0` and `?persist=reset`
  control it, and so does the Reset Files button.

**The display** (`libjs/guest/gui.h`, `libjs/gui-device.js`)

- **The device.** It sits at 0x400-0x47F and is announced by
  `__c4_info() & C4I_GUI`.
- **Drawing is a command stream** in a guest memory ring:
  - `gui_rect`, `gui_recto`, `gui_line`, `gui_disc`;
  - `gui_text2`, which draws in the host's fonts (mono, sans, sans bold)
    at any size, with an optional fixed advance;
  - `gui_imgdef` and `gui_image`, for images the host caches;
  - `gui_blit8`, 8-bit pixels with a palette, scaled;
  - `gui_clip`, `gui_size` and `gui_show`.
- **Events come back through a second ring**: mouse down, up, move and
  wheel in display pixels, keys as `[keyCode, char, mods]`, and resize.
- **What it keeps and lacks.** The canvas is retained, so a redraw of
  only the damaged part works. There is no screen-to-screen copy, no
  readback, and no way to measure text. The host draws the pointer.
- **One ring at a time.** Writing `GUI_RING` takes the display from
  whoever had it.
- **The sound card** (0x480-0x4B8, `snd.h`) has OPL, 8 PCM channels and
  the speaker.

**The games under C4DOS** (`src/{wolf3d,doom,rott,opentyrian}`)

- **Built** by c4lc and linked with libc's `bare.c` layer, of which they
  carry only what they use (c4rlink drops unreached functions,
  docs/c4dos-design.md "Linking a bare program").
- **How they run.** Each attaches its own ring at 640x480 and blits a
  320x200 8-bit frame scaled 2x. Each uses the sound card directly, and
  exits through `exit()`: `atexit` detaches the display, then DOS slot
  1 is called.
- **They already have window back-ends for the C4IX desktop**
  (`plat-c4ix.c`, `dg-c4ix.c`, `rott-c4ix.c`, `tyr-c4ix.c`). The bare
  versions return 0, so `--window` under C4DOS means full screen today.
- **Saves.** Saves and config go through `bare.c`'s `uopen` for writing,
  which only DOS's RAM disk takes. On the bare machine they fail.
- **Memory.** Doom takes about 15 MB and ROTT about 17 MB, and **nothing
  freed a program's heap when it exited**. C4DOS freed only the image's
  code and data. Measured in G0: each run of Doom at `A>` left
  14,853,107 bytes behind. `HEAP.SYS` (3.3) fixes it.

**The C4IX desktop** (`src/c4ix/user/desktop/`, 8,500 lines)

- **Its drawing is portable.** It draws only through gui.h, so the
  drawing layer, widgets, dialogs and menus are portable.
- **What is C4IX-specific** is the process plumbing: pipes, `spawn`,
  signals, `utaskinfo`, `uevwait`, and the `window.h` escape-sequence
  protocol.
- **How it is built.** On the host, c4lc builds it (`Makefile`,
  `build-images.sh`). Inside C4IX, c4sc builds it from
  `src/c4ix/user/desktop/BUILD.B4K`, one module per compile, with
  `libgui.c4s` as a shared library. Shared code must keep compiling on
  both routes.
- **The review this plan relies on.** The code review
  (`c4ix-desktop-review.md`, in the session notes) proposes replacing
  the per-kind if-ladders with a `struct tool` table of function
  pointers and a `.size` per kind. It also lists bugs that must not
  be carried over.

## 3. Architecture

### 3.1 The process model

grin is **one C4DOS program, `grin.c4r`**, built by c4lc at 32 bits and
linked with `bare.c` and the shared window kit (3.5). Three kinds of
code run under it:

| kind | what it is | how it runs | examples |
|---|---|---|---|
| built-in | linked into `grin.c4r` | called from the event loop | window manager, Program Manager, File Manager, Task List, Control Panel, dialogs |
| **grin application** | a `.c4r` that calls the window kit and returns `GRIN_RESIDENT` from `main` | loaded by DOS and kept, with its windows' callbacks run from grin's loop. Any number at once, cooperatively | Notepad, Clock, Calculator |
| **DOS program** | any other `.c4r`, or a builtin or batch file | run by C4DOS's own interpreter, nested under grin, **one at a time** | the games, `hello`, the compilers, `DIR > list.txt` |

This is the Windows 3.x split. Windows programs were event-driven and
shared one message loop. DOS programs got the machine, either full
screen or in a window. A DOS program in a window keeps grin's
applications alive by running grin's loop whenever it waits for input
(3.6, 3.7).

**C4DOS does the loading; grin does not have a loader of its own.**
The first draft gave grin a copy of DOS's loader, with a proxy API
table so that a child's `exit()` came back to grin. The user asked for
shared code over duplicated code. So instead C4DOS's RUN becomes
re-entrant and its services are offered through the API table (3.2).
Then:

- grin's DOS Prompt windows are C4DOS's own interpreter, with its
  builtins, batch files and redirection;
- a child's `exit()` lands in the innermost run frame;
- the heap is reclaimed by DOS for every program, at the plain prompt
  as well as under grin.

### 3.2 C4DOS services (API v4)

**Re-entrant RUN** (built in G0; `docs/c4dos-design.md` "Run frames").

- Every call DOS makes into a program's code -- constructors, `main`,
  destructors, and `catch` -- goes through a run frame
  (`frame_call`/`frame_enter`), which keeps the outer frame's `g_run_bp`
  and `g_in_trans` in its locals, so `dos_exit` returns into the
  innermost. The status rides home in a global, not in A: the trampoline
  tail may hold an `IMM`.
- A load is a handle owning the code, data, its own copies of the
  constructor and destructor tables, its heap generation and its parent.
  RUN is load + start + unload. Unload takes the images an image left
  loaded with it, and refuses one whose code is still on the stack.
- Each command line is parsed into a buffer of its own per exec.

All of this is plain c4, so the clockless build still runs under
unmodified `c4` (`make test-c4dos` pins it, v4 scenarios included).

**New slots** (the version word is 4). Each is gated on the version,
and on the device line it needs, the way slots 9-15 are. As built:

| slot | call | for |
|---|---|---|
| 16 | `conwrite(buf, n)` | console output, through the current sink (3.7) |
| 17 | `conread(buf, n)` | console input, at most a line, through the current sink. DOS's own input buffer, so a line typed ahead of a program is the program's |
| 18 | `unlink(name)` | delete on the RAM disk (only with RAMDISK.SYS; writable drives come with `HOME=`, G4) |
| 19 | `exec(cmdline)` | run one command line exactly as the prompt would, with builtins, batch files and `.c4r` (redirection with REDIR.SYS, G5); returns its status. `EXIT` inside an exec ends the exec, not DOS: a DOS Prompt window checks for `EXIT` itself |
| 20 | `load(path)` | load an image and run its constructors without calling `main`; returns a handle, 0 on failure or refusal |
| 21 | `start(h, argc, argv)` | call `main` under a run frame, in the image's generation; returns its status. The image stays loaded whatever it returns: keeping it is the caller's decision (`GRIN_RESIDENT` is grin's convention, G3) |
| 22 | `unload(h)` | run the destructors, free the image, release its generation (3.3); also unloads images it left loaded; -1 for a dead handle or one still running |
| 23 | `catch(fn, arg, exited)` | call `fn(arg)` under a run frame, so an `exit()` inside it returns here, as its status; `*exited` (if `exited` is not 0) says which happened. **Changed from the draft's `catch(fn, arg)`**: without the flag grin could not tell an application that exited from one that returned. Runs in the caller's generation |
| 24 | `sink(write, read, ctx)` | point the console at the caller's `write(ctx, buf, n)` and `read(ctx, buf, n)`; 0 for either gives that direction back to DOS |
| 25 | *data*: the shell | a pointer that grin writes and clears: the window kit's table (3.4) |
| 26 | `gen(h)` | **added in G0**: image `h`'s heap generation, or with `h` = 0 the caller's own (the image DOS last started); 0 without HEAP.SYS |
| 27 | `setgen(g)` | **added in G0**: make generation `g` current; returns the previous one, -1 without HEAP.SYS |

Later versions added 28 and 29 (v5, G4: `home()`, `drives()`), 30 (v6,
G5: `drive()`) and **35 (v8, G6): `image(h)`**, which makes `h` the image
DOS counts as running -- the parent of what it loads -- and returns the
one before; grin's game window needs it (3.6, "As built in G6").
`docs/c4dos-design.md` "API versions" has them all.

`include/c4dos.h` documents and wraps every slot, through the invoke
stub. **Changed from the draft**, which kept c4cc programs to slots
0-18: the stub calls DOS's functions as ordinary calls, so it serves
every slot, `dos_exit` included, and `src/tests/dosexec.c`, a c4cc
program, exercises 16-27 under unmodified `c4`. c4lc-built callers
(grin, `bare.c`) call through the table directly.

### 3.3 Memory: heap generations

The leak makes "launch a game, quit, launch another" fail after a few
rounds, at the `A>` prompt today as much as under grin. The fix is
below DOS, and DOS uses it for every program.

**The machine.** `libjs/heap.js` gains a generation tag: each block
records the generation that was current when it was allocated. Two
registers sit next to `HEAP_USED` (0x1c0) in `src/c4bb/sim/devices.js`,
which libjs shares:

- `HEAP_GEN` (0x1c4, r/w): the current generation;
- `HEAP_RELEASE` (0x1c8, w): free every block of the generation
  written; read, the bytes the last release freed.

Both were confirmed free in G0. Generation 0 is the boot one: the heap
never tracks it and refuses to release it. Where the host keeps no heap
with generations (c4bb, whose heap is its firmware's; `sheap.js`, the
parallel engine's), `HEAP_GEN` reads 0 whatever was written.

**C4DOS.** `DEVICE=HEAP.SYS` announces them. The announcement is a
config line because reading `INFO` would put DOS above the base rung,
and under native c4m these addresses are ordinary memory. DOS then
checks that `HEAP_GEN` reads back what it wrote, and on c4bb says the
machine keeps no generations. With the line:

- `load` gives each image a fresh generation, and its code and data are
  allocated in it; its constructors, `main` and destructors run in it;
- `unload` (and the end of every RUN) writes `HEAP_RELEASE` for it;
- **every DOS API entry that allocates** (`create` and `write` growing a
  RAM-disk file, `scratch_need`, `load`, `exec`) switches to DOS's own
  generation and back. Otherwise a RAM-disk file created by a child
  would be freed with the child.
- `MEM` reports the heap's free and used bytes.

**grin.** It follows the same rule, through slots 26 and 27 (added in
G0: the draft had `gen_enter()`/`gen_leave()` with nothing for them to
call). grin learns its own generation with `gen(0)` in `main`, and an
application's with `gen(h)` after loading it. Every window-kit entry
point does `old = setgen(grin's)` ... `setgen(old)`, and so does the
loop pump that a DOS program runs (3.6). Every call into an application
does `setgen(the application's)` around its `catch`, because `catch`
runs in the caller's generation. A window or dialog that grin opens
while a game runs therefore belongs to grin, and what an application's
callback allocates goes when the application is unloaded.

Tests pin both directions:

- A file created by a child survives the child.
- `HEAP_USED` after Doom has run five times stays within 1% of its value
  after the first.
- A dialog opened during a windowed game still works after the game
  quits.

### 3.4 The application API: the window kit and `grin.h`

A grin application is written against the **window kit** (3.5), the
same functions the C4IX desktop's tools call. Each window kind is a
class. This is `src/winkit/winkit.h` as G1 built it; the shape is the
first draft's, with what building it on the desktop asked for added
(marked *G1*, and explained after the listing):

```c
// winkit.h -- what a window's code sees
struct wk_class {
  int  size;                        // bytes of per-window state, zeroed
                                    // (set at run time: c4lc has no
                                    // sizeof(struct) in a constant)
  int  kind;                        // G1: the shell's number for it, and its icon
  int  pointer_shape;               // G1: PTR_* over the client area, unless pointer says
  void (*open)(int w, char *arg);
  void (*draw)(int w, int cw, int ch, int focused);   // client coordinates
  void (*mouse)(int w, int ev, int x, int y, int button);   // E_WHEEL: button is the steps
  void (*key)(int w, int down, int code, int ch, int mods);
  void (*command)(int w, int id);                     // menu item: WK_CMD(menu, item)
  void (*timer)(int w, int id);
  void (*answer)(int w, int dialog, int result);      // a dialog closed
  int  (*close)(int w);                               // 0 vetoes ("Save changes?")
  void (*destroy)(int w);
  int  (*pointer)(int w, int x, int y);               // cursor shape there
  void (*idle)(int w);              // G1: every pass of the loop (a pipe to read)
  void (*drawn)(int w);             // G1: a frame with it in has been shown
};
struct wk_menu { char *title; char **items; };        // items end in 0, "-" a separator

// windows: handles are small ints (1 up) that are never reused, not pointers
int   wk_open(struct wk_class *c, char *title, int w, int h, int flags, char *arg);
                                    // flags: WK_FIXED, WK_DIALOG, WK_CENTER
void  wk_close(int w);              // at once
void  wk_ask_close(int w);          // G1: as the close box does, so close may veto
void *wk_state(int w);
void  wk_title(int w, char *s);
void  wk_invalidate(int w, int x, int y, int ww, int hh);   // ww = 0: all of it
void  wk_menu(int w, struct wk_menu *m, int n);
int   wk_timer(int w, int id, int ms);                     // ms = 0 stops it
void  wk_raise(int w); int wk_focus(); void wk_size(int w, int *cw, int *ch);   // G1, and
int   wk_find(struct wk_class *c); int wk_next(int w); int wk_alive(int w);   // a few more
void  wk_msgbox(int owner, int dialog, char *title, char *text, int buttons);   // WK_OK, WK_YESNO
void  wk_inputbox(int owner, int dialog, char *title, char *prompt, char *buf, int max);
void  wk_filebox(int owner, int dialog, int save, char *pattern, char *buf, int max);
int   wk_clip_set(char *s, int n);  char *wk_clip_get(int *n);
int   wk_now(); int wk_clock();     // G1: machine ms; the host's time of day
// drawing, only inside draw(): client-relative and clipped
void  d_rect(...); d_recto(...); d_line(...); d_disc(...); d_text(...); d_textn(...);
void  d_bevel(...); d_icon(...); d_blit8(...); d_clip(...); d_clip_end();
// widgets: ui.c, stateless, so the window owns the state
int   ui_button(...); ui_field(...); ui_field_key(...); ui_scrollbar(...); ...
int   ui_ncpy(char *d, char *s, int n); ui_join(...); ...   // G1: bounded strings (B3)
// G1: the screen, the look, the modifiers and the colour scheme, in one struct
struct wk_env { int sw, sh, ax, ay, aw, ah, style, mods, desk, face, ...; };
extern struct wk_env wk;            // C_FACE is (wk.face), ui_style (wk.style)
// G1: what the host gives the kit: memory and files
char *wk_alloc(int n); void wk_free(char *p);
int   wk_fopen(char *path, int write); wk_fread(...); wk_fwrite(...); wk_fclose(...);
int   wk_readdir(char *dir, int i, char *name);
```

What G1 added, and why:

- **`kind`, `pointer_shape`, `idle`, `drawn`.** The desktop's tools
  needed them: a Terminal reads its pipe every pass and tells a canvas
  program when its frame has been shown, and every window has a 16x16
  icon on its title bar and taskbar button. Unset, they cost nothing.
- **`wk_ask_close`.** `close` can veto only if something asks first; the
  close box, the control menu and Notepad's File > Exit ask, `wk_close`
  does not.
- **`struct wk_env wk`.** The colours change with the scheme and the
  screen with Control Panel, so they cannot be constants an application
  copies. Keeping them in one struct lets `import.c` reach them through
  one pointer. *As built in G3*: an application's files are compiled
  with `-D WK_IMPORT`, under which `winkit.h` declares
  `extern struct wk_env *wk_import_env;` and `#define wk
  (*wk_import_env)`, so every `C_FACE` is a load through grin's struct.
  (G1's idea, the define placed before including `winkit.h`, does not
  compile: c4lc takes `extern struct wk_env (*wk_import_env);` for a
  function pointer with its parameters missing; and `clock.c` and
  `calc.c` are compiled unchanged, so the switch has to come from the
  command line.)
- **The host's functions.** Notepad reads and writes files and the file
  box lists folders, and the kit must not know whether that is
  libc4ix or C4DOS's libc. The shell provides them (the desktop's
  `host.c`). *As built in G3*: `import.c` serves memory and files
  (`wk_alloc`, `wk_free`, `wk_fopen` ... `wk_fclose`) from the
  application's own libc, so that what an application allocates and
  opens is its own and goes when it is unloaded; it forwards
  `wk_readdir` and `wk_clock`, which are grin's.

`grin.h` adds what only grin has, such as `grin_exec(cmdline, how)`
(deferred, see the rules below) and the game-window calls (3.6). *As
built in G3* it has `GRIN_RESIDENT` and `grin_present()`; `grin_exec`
comes with the DOS programs (G5). *As built in G6* the game-window calls
are not in `grin.h` but in the table itself (gkit.h, version 2), since
the games are DOS programs that never link `import.c`: 3.6.

**One source, two hosts.** In the C4IX desktop these names are the real
functions, linked in. A grin application links **`grin/import.c`** in
their place: a forwarding stub per function that calls through the
table grin publishes in DOS slot 25. This is Win16's import library.
So `calc.c`, `notepad.c` and `clock.c` compile unchanged into both:

- into the desktop, as built-in tools;
- into grin, as `calc.c4r`, `notepad.c4r` and `clock.c4r`, each with a
  ten-line `main` that registers its class.

Each forwarded call costs a few instructions more, which is nothing
beside the draw command it issues.

**As built in G3** (`src/grin`):

- **`kitfns.h`** lists the functions an application may call, one line
  each (`K(type, name, (params), (args))`, `V(...)` for void): the
  windows, dialogs, clipboard, drawing, schemes and widgets of
  `winkit.h`, 72 of them. It is included three times: `gkit.h` makes
  the table's members of it, `import.c` a stub each, `apps.c` the
  filling. Not in it: the shell's section (`wk_init`, `wk_event`,
  `wk_tick` and the rest), the screen savers, the accessories (an
  application links its own), and memory and files (above). New
  functions go at the end, so the table only grows.
- **`gkit.h`**: `struct grin_kit` -- `magic` ('GRIN'), `version` (1),
  `gen` (grin's heap generation, 0 without HEAP.SYS), `env` (`&wk`),
  `hello`, then the functions.
- **`import.c`**: every stub runs the kit in grin's generation, `old =
  setgen(grin); call; setgen(old)` through slot 27, so the plan's
  "every kit entry point reached from an application brackets setgen"
  lives here, once, rather than in a wrapper per function in grin. Its
  constructor finds the table (version 4 or more, slot 25, the magic and
  version) and calls `hello`: that is how grin tells an application from
  a DOS program it was asked to start.
- **`apps.c`**, grin's side:
  - the table is published in slot 25 by `apps_init` (after `wk_init`)
    and taken back by `apps_end` (after `wk_close_all`, before
    `gui_detach`);
  - **proxies.** The kit calls a class's hooks itself, so for each class
    an application opens windows of, grin keeps a proxy class whose
    hooks are trampolines, made to match the real class at every
    `wk_open` (the application sets `size` first). The table's
    `wk_open`, `wk_find` and `wk_class_of` are grin's: they take and
    give the application's own class, so it never sees a proxy;
  - **every call into an application** is a trampoline: `setgen(app)`,
    `catch(thunk, &call, &exited)`, `setgen(old)`, with grin's `cur` (the
    application running, which `wk_open` needs) kept around it;
  - **exit()**: `exited` marks the application dead, and nothing more is
    called in it (a dead application's `close` says yes). At the top of
    the loop, after `wk_tick` (`apps_reap`), a dead application's dialogs
    and windows are closed, and every application with no window left
    is unloaded. `unload` is never reached from inside a callback;
  - **starting one**, `grin_run(cmdline)`: the words, `.c4r` added
    when the program has no extension; an application loaded already
    gets its `main` called again; otherwise `load`, refused with a
    message box when the image did not say hello ("DOOM is not a grin
    application...", and it is unloaded at once). A first launch whose
    `main` does not return `GRIN_RESIDENT` (or exits) is dead at once.
    At most 16 applications and 32 classes.
    (G6: a program that is not an application goes to `grin_dos`, which
    runs it in a window or full screen as Program Manager's item for it
    says, 3.6.)

A complete application under grin:

```c
#include "winkit.h"
#include "grin.h"
struct clock { int last; };
static void clock_draw(int w, int cw, int ch, int focused) { ... }
static void clock_tick(int w, int id) { wk_invalidate(w, 0, 0, 0, 0); }
static struct wk_class clock_class = { .draw = clock_draw, .timer = clock_tick };

int main(int argc, char **argv) {
  int w;
  if (!grin_present()) { printf("This program requires grin.\n"); return 1; }
  clock_class.size = sizeof(struct clock);
  w = wk_open(&clock_class, "Clock", 160, 180, 0, 0);
  wk_timer(w, 1, 1000);
  return GRIN_RESIDENT;
}
```

**Rules**

- **Callbacks never block.** An application never loops waiting for
  input, and each callback returns promptly. That is the cooperative
  contract, exactly Windows 3.x's.
- **Unset callbacks are 0**, and the kit checks before calling (the
  review's M1).
- **Lifetime.** grin starts an application with DOS `load` + `start`.
  The image stays while it has a window open. When the last window is
  destroyed, grin calls `unload`, which frees the image and its heap.
- **A second launch** of a loaded application calls its `main` again on
  the same image (the `hPrevInstance` model). Per-window state belongs
  in the window, not in globals.
- **`exit()` inside a callback** closes that application's windows and
  unloads it. grin makes every call into an application through DOS
  `catch`.
- **Files are ordinary libc.** `bare.c` reaches DOS through
  `__c4dos_api` as it does today.
- **`grin_exec` is deferred.** grin runs it from its top-level loop
  after the callback returns, so a DOS program never runs nested
  inside an application's callback.

### 3.5 The shared window kit (`src/winkit/`)

The user chose sharing over copying. The C4IX desktop's code moves into
a kit that both shells build on.

| file | what | from |
|---|---|---|
| `winkit.h` | the API above, the colours, event and `KEY_*` names | `desktop.h` |
| `draw.c` | the `d_*` layer over gui.h: client origin and clip, damage tracking, schemes | `wm.c` |
| `ui.c` | widgets | `ui.c`, as is |
| `wmcore.c` | window list, z-order, focus, modality, drag, resize, menus and drop-downs, timers, and class dispatch | `wm.c`, minus the shell |
| `dialog.c` | message, input and file boxes (G1 split them from `wmcore.c`, to keep each module small for c4sc) | `wm.c` / new |
| `look95.c`, `look31.c` | window chrome: the desktop's Classic and Flat looks, and the new Windows 3.1 one (control-menu box, arrow buttons, thick border, centred title, icons for minimised windows, G2's `wk_min_icons`) | `wm.c` / new |
| `textgrid.c` | VT100 grid, scrollback, line editor, row cache, with no pipes | `term.c` |
| `savers.c` | screen savers | as is |
| `calc.c`, `notepad.c`, `clock.c` | the accessories, as classes | `calc.c`, `apps.c` / new |

The shells stay separate:

- **C4IX's `desktop`** keeps the taskbar, the Go menu, Terminal's
  processes, System Monitor, Explorer and the Editor.
- **grin** has Program Manager, File Manager, the Task List, Control
  Panel, the DOS Prompt and the launcher.

On C4IX the kit can also become a shared library (`libwk.c4s`, beside
`libgui.c4s`).

**What sharing costs.** The kit is compiled by c4lc on the host for
both shells, and by c4sc inside C4IX for the desktop's `BUILD.B4K`. So
it keeps to what both compilers take, which the review's probes cover:
function-pointer tables, designated initialisers, and no
`sizeof(struct)` in a constant. `test-desktop.mjs` and the desktop's
CDP gate pin "no visible change" when the desktop moves onto the kit.

**Two kinds of change happen at the move.**

- **Behaviour carried over.** The class table (review M1), sizing each
  window's state by its class (B9/M8), and int handles that are never
  reused, which fix the dangling `feeding` pointer (B5).
- **Bugs not carried over.** Paths get one `PATH_MAX` and bounded
  copies (B3). Allocations are checked (B8). The dialog flag lives apart
  from its text (B7). Each input box gets its own buffer (B10). Notepad
  no longer truncates at 64,999 bytes (B6).

**Screen size is a run-time property of the kit.** `gui_size` asks for
it, and the RESIZE event that comes back triggers the relayout:
maximised windows refill the screen, others are pulled back inside it,
and icons are rearranged. grin switches between 640x480 and 800x600
from Control Panel. The C4IX desktop gains the same ability for free.

### 3.6 The event loop, and launching DOS programs

```
loop:
  sweep closed windows; unload applications with no windows left
  while gui_poll(ev): dispatch (mouse to window or chrome, keys to focus,
                      accelerators, Alt+Tab, Ctrl+Esc; resize; focus)
  fire due timers
  if a deferred exec is queued and no DOS program runs: run it
  redraw the damage, or everything
  idle: __c4_usleep(until the next timer, at most 50 ms); a display
        event cuts the nap short (worker.js:296-318)
```

`grin_pump()` is one pass of this loop, without the exec step. A
running DOS program calls it whenever it waits for grin: from a game
window's poll, and from the DOS Prompt sink's `conread` (3.7).

Each Program Manager item and each File Manager "Run" has a mode,
kept in its properties, as a PIF file kept it:

- **Full screen**, the default for a program grin knows nothing about.
  - Before the program: grin hides its pointer, calls `gui_detach`,
    and runs the program with DOS `exec`.
  - When the program returns, grin:
    1. re-attaches its ring, because the program took and dropped
       `GUI_RING`;
    2. sets its size, event mask and cursor again;
    3. calls `snd_reset()`;
    4. redraws everything.
  - DOS has already released the program's heap. This is the C4IX
    desktop's `udisplay_give`/`take` with no kernel in it.
- **Window** (the games, from G6).
  - The game's bare platform layer reads slot 25. Finding grin there,
    it uses `grin_fwin_open`, `grin_fwin_frame` and `grin_fwin_poll`
    instead of the whole display. It never calls `gui_attach`.
  - `fwin_frame` records the frame and damages the window. The blit
    goes out in the next redraw, and the host copies the pixels at the
    bell, so no double buffer is needed.
  - `fwin_poll` calls `grin_pump()` and returns the next event meant for
    the game's window, in gui.h's form, with mouse coordinates in the
    frame's pixels.
  - While the game runs, the Clock ticks and Notepad takes typing.
    Closing the window makes `fwin_poll` return -1, which the C4IX
    back-ends already treat as quit.
- **DOS Prompt**: 3.7.

**One DOS program at a time.** While one runs, starting another shows a
message box ("Close Doom before starting another program"). Exit grin
is refused while a DOS program runs.

*As built in G5* (`src/grin/grin.c`):

- **The loop** is `pass(top)`: the display's events, `wk_tick`, the
  screen saver, a redraw and a nap, with `apps_reap` only at the top.
  `grin_pump()` is `pass(0)`. After each top-level pass, `main` runs one
  DOS program that is waiting, if none is running: a full-screen one
  (`full_run`) or a DOS Prompt's line (`dosprompt_top`, 3.7).
- **Full screen.** `grin_full(cmdline)` queues the line, or, while a DOS
  program runs or waits, says "A DOS program is running (DOOM). Let it
  finish before starting another." `full_run`: the saver stopped, the
  pointer hidden, the screen cleared, `gui_detach`, DOS's `exec` of the
  line; then `gui_attach` with grin's ring at GRIN.INI's size, the event
  mask, the arrow, `snd_reset()` (when the sound card is fitted),
  `wk_repoint` and `wk_redraw_all`. The program's text goes to the
  page's console, as at A>.
- **What launches one:** a Program Manager item whose mode is `full`;
  File > Run... and File Manager's double click on a `.c4r`, through
  `grin_run`, which loads the program and, when it does not say hello,
  unloads it and gives the line to `grin_full` (so DOS loads it twice,
  and its constructors run twice; none of the games minds). A `window`
  item is greyed and says that comes with G6.
- **Exit grin** is refused, with the same message, while a DOS program
  runs (which can only be a DOS Prompt's, waiting for a line).

*As built in G6* (`src/grin/fwin.c`, and each game's bare platform
layer):

- **The table.** `struct grin_kit` (gkit.h) is version 2: after `hello`
  come `fwin_open(title, w, h, flags)`, `fwin_frame(pixels, w, h, pitch,
  pal)` and `fwin_poll(ev)`, words 5 to 7, before the kit's functions. A
  game declares those eight words itself and needs nothing else of
  grin's; every application is built with grin, so moving the kit's
  functions one word once broke nothing (the "table only grows" rule
  holds from here). `flags` 1 says the mouse steers.
- **A game finds grin** when slot 25 holds the table (C4DOS v4 or later,
  the magic, version 2). grin takes it out of slot 25 while a
  full-screen program has the display (`apps_publish`), so a game that
  finds it can always have a window, and one that does not takes the
  display as at `A>`. In each bare layer the C4IX back-end's trio is
  pointed at grin: Doom's `dg_take` says the display is not the game's,
  and `dg-gui.c` asks for the window it asks for on C4IX when a program
  has the display (it says so on the console); Tyrian's and ROTT's
  `*_open_display` likewise; Wolf3D's `plat-bare.c` opens grin's window
  in `plat_open`. Never `gui_attach`. 50 to 80 lines each, the table's
  declaration included.
- **`fwin_frame`** copies the frame into a buffer of grin's (one, with
  its palette) and damages the window's client area. grin's next redraw,
  which the poll makes at once for a fresh frame rather than within 25
  ms, sends it as `GUI_BLIT8`, scaled 1x or 2x by the host. Copied rather
  than kept as a pointer, so that a redraw for another window's sake (a
  Clock's tick, a window dragged over it) always has a whole frame.
- **`fwin_poll`** runs one pass of grin's loop when its queue is empty
  and it has not run one since it last said "none" (`grin_pump_game`:
  `pass(PASS_GAME)`, which naps never and starts no screen saver -- the
  game keeps its own time), then returns the next event in gui.h's form,
  0 for none, -1 once the window has gone. Around the pass it switches
  to grin's heap generation (`setgen`) and to grin's image (C4DOS v8's
  `image()`, slot 35, new): without that an application started from
  Program Manager while a game played would have been the game's child,
  and C4DOS would have unloaded it with the game while its windows were
  grin's.
- **Keys** go to the game only while its window has the focus and no
  dialog or menu holds them, all but Ctrl+Esc (the Task List); grin's
  loop gives them to it before the kit sees them (`fwin_event`), since
  the kit draws the whole scene again after every key. When the window
  loses the focus the game is told (`GUI_EV_FOCUS`) and every key it
  holds is let go. Mouse positions are in the frame's pixels.
- **Pointer lock**, for a game that steers with the mouse (Tyrian, ROTT):
  over its frame the window's pointer is `GUI_PTR_LOCK` ("Click to
  capture the mouse"), so the click on the window locks it
  (libjs/display.js); grin's event mask has `GUI_M_REL`, so the motion
  comes as `GUI_EV_REL` and goes to the game alone (in frame pixels, the
  remainder kept), with its buttons, until the first plain move, which
  says Esc let it go. Losing the focus lets it go too. Doom and Wolf3D
  take only keys, as full screen, and keep the arrow.
- **Sizes**: the window's Size menu, 1x (320x200 client) or 2x (640x400);
  2x when it fits the area (800x600), else 1x (at 640x480 a 2x window is
  8 pixels wider than the screen).
- **Launching**: Program Manager's `window` items (the four games, the
  default now: `Doom=DOOM,game,window`), through `grin_window`, which
  queues the line as `grin_full` does; at the top of the loop `win_run`
  sets `dos_busy`, puts a sink on the console that keeps the last 200
  bytes, and runs the line with DOS's `exec`, the display kept. A program
  that ends without opening a window gets a message box with its last
  line ("HELLO ended without opening a window. It said: yello"). File
  Manager's double click on a `.c4r` and File > Run... reach `grin_dos`
  when the program is not an application: the first Program Manager item
  that runs the same program says window or full screen, as a PIF did;
  with none, full screen. A game typed in a DOS Prompt plays in a window
  too, its text in the DOS Prompt.
- **Closing**: the window closed (control menu, End Task) makes the next
  poll say -1, which each game takes as quit (Doom, Tyrian and ROTT
  `exit(0)`, Wolf3D its quit event). At the top of the loop, after any
  DOS program, `fwin_end` closes a window the program left, frees the
  frame and lets the mouse go; `snd_reset()` quiets the sound card.

### 3.7 Redirection, and DOS Prompt windows

**At the prompt.** `DEVICE=REDIR.SYS` lets C4DOS's command line take
`> file`, `>> file`, `< file` and `a | b`. A pipe runs through a
temporary RAM-disk file, as MS-DOS's did. Without the line, C4DOS
parses command lines exactly as today: the 2026-08-21 decision stays
the default, and the new behaviour is opt-in, as the user asked. The
batch files on the build floppies are unaffected.

**What gets redirected.** A program's console traffic takes one of two
roads, and both have to be redirected.

1. **Programs linked with libc's `bare.c`.** Their `write(1|2)` and
   `read(0)` go through DOS slots 16 and 17 (a few lines in `bare.c`,
   which already calls DOS through the table for files). DOS sends them
   to the current sink: the console, a redirect file, or a grin window.
   This road works on every machine C4DOS runs on, native c4m
   included.
2. **c4cc programs.** These print with the `PRTF`/`PUTC` opcodes and read
   with `READ` on fd 0, so they never come near DOS. Redirecting them
   needs the machine: a **console tap** in `src/c4bb/sim/devices.js`,
   which c4bb and libjs share.
   - Its registers turn on capture. The host then keeps console output
     in a buffer of its own instead of sending it to the UART, and DOS
     drains it afterwards (`CON_ADDR`/`CON_LEN`, the disk registers'
     shape).
   - Fd-0 reads can be fed from a guest buffer.
   - `DEVICE=REDIR.SYS TAP` announces the tap. Native c4m has no tap, so
     there a c4cc program's output stays on the console even when
     redirected, and DOS says so once.

**DOS Prompt windows** are C4DOS's interpreter in a text window.

- Each window is a `textgrid.c` window showing the prompt. grin reads a
  line in it, points DOS's console sink (slot 24) at that window, and
  calls `exec(line)`.
- So `DIR`, `TYPE`, `COPY`, batch files, `>`, `<` and `|` behave exactly
  as at `A>`, because they are the same code.
- **`bare.c` programs run live.** Their output appears as they write,
  and their `conread` pumps grin's loop until a line is typed in their
  window. Other applications keep running meanwhile.
- **c4cc programs** run through the tap. Their output shows when they
  return, since a c4cc program never yields to grin. This is Windows
  3.x standard mode, where a DOS program had the machine to itself.
- **Several windows, one command at a time.** Several DOS Prompt windows
  can be open. While one runs a command, the others take typing but
  say "busy" on Enter.

*As built in G5.* C4DOS's side -- `REDIR.SYS`, the formatter its
builtins print through, the console tap, API v6 -- is in
`docs/c4dos-design.md` ("Redirection: DEVICE=REDIR.SYS"). grin's is
`src/grin/dosprmpt.c`:

- **A window** is "DOS Prompt N", N the lowest number free, an 80x25
  `textgrid.c` grid (WK_FIXED). At 640x480 it is cut to the screen's
  width, so the last three columns and the scroll bar are off its right
  edge; at 800x600 it fits.
- **A line typed** is queued; the top of grin's loop runs it: the
  window's own drive selected first (`exec("B:")`) when DOS's differs --
  DOS has one prompt drive for everyone, and slot 30 says which it is
  after each line -- then `sink(dp_write, dp_read, w)`, `exec(line)`,
  `sink(0, 0, 0)`, and the prompt again.
- `dp_write` feeds the grid, and draws at once at most every 50 ms, for a
  program that writes and does not read. `dp_read` pumps grin's loop, in
  grin's own heap generation, until a line is typed in its window;
  Ctrl+Z (or Ctrl+D) gives the program the end of its input.
- **C4DOS calls a sink as the program that installed it**, so an
  application started while a DOS Prompt's program waits (Clock from
  Program Manager) is grin's child, not that program's, and is not
  unloaded with it.
- **Busy:** the other windows say "grin: busy -- ASKLINE is running"
  on Enter; Program Manager and File Manager refuse a second DOS program
  with the message box above. A window whose line runs refuses to close.
- **EXIT** (any case, spaces around it) closes the window, which looks
  for the word before DOS sees it.
- **c4cc programs** run through the tap: their output appears when they
  return, and a read of fd 0 gets the end of input.
- **File Manager** runs a `.bat` in a new DOS Prompt, typed at its first
  prompt, and File > Edit opens the selected file in Notepad (the way to
  edit a `.bat` now).

### 3.8 Persistence

The page's existing drive-1 store does the keeping. What is new is that
C4DOS and `bare.c` write to it.

**The page.** Every system persists in an IndexedDB store of its own,
named in its `SYSTEMS` entry, as C4IX's is `c4ix`. `?system=c4dos`
uses `c4dos`, and each bare-machine game uses its own name (`doom`,
`wolf3d`, `tyrian`, `rott`). The user ruled out one shared store: the
planned ladder system, which boots C4DOS, builds and boots C4KE, then
builds and boots C4IX, needs persistence separate from the others.
That system gets its own store, `ladder`, when it is added. A game's
saves made under C4DOS live on C4DOS's B:, and those made on the bare
machine live in the game's own store. `?persist=0`, `?persist=reset`
and Reset Files work as they do for C4IX, on the current system's store
only.

**C4DOS.** `DEVICE=DRIVES.SYS HOME=B:` names a home drive.

- **Writes.** `create` writes to a writable drive when the name carries
  its prefix (`B:NOTE.TXT`), or to the home drive when it carries none.
  It falls back to the RAM disk when there is no home drive, or the
  drive is read-only (`DISK_RO`). The build floppies set no home, so
  their RAM-disk flow is unchanged.
- **Reads.** `dos_open` looks in the RAM disk, then the home drive, then
  the current drive.
- **Listings.** DOS keeps `c4dos.dir` up to date on any drive it writes,
  so `DIR B:` and File Manager can list it.
- `unlink` (slot 18) removes the file and its `c4dos.dir` line.

**The bare machine.** With no DOS present, `bare.c` writes a file with
c4bb's write registers on drive 1, when the machine announces it
(`__c4_info()` bits, checked once). It reads drive 1 before drive 0.
The four games' settings and saves then persist with no change to the
games.

**grin.** `GRIN.INI` (screen size, colours, startup group) and
`PROGMAN.INI` (groups and items, each with its title, command line,
icon and run mode) are ordinary files on the home drive.

**As built in G4.** As above, with these particulars (docs/c4dos-design.md,
"The home drive", has C4DOS's side whole):

- **The machine** gains `DISK_UNLINK` (0x1cc), a file taken off a
  writable drive, and the info bit `C4I_DRV` (0x40000), which says the
  drive registers are fitted. A drive's `remove` hook carries a delete to
  the host: worker.js posts it, page.js takes the file out of the store
  (store.js's `deleteFile`), and the CLIs delete it from their directory.
- **C4DOS** is API v5: slots 28 `home()` and 29 `drives()`; `create`
  reaches drives; `unlink` and the new `DEL` take files off them; DOS
  keeps each drive's `c4dos.dir`; `DIR B:` and `COPY x B:`. G0's caveat
  is resolved: write handles are an indirection, so an unlink no longer
  moves an open file onto its neighbour, and a program that ends with
  files open has them closed.
- **bare.c**, with no DOS, writes to drive 1 and reads it first, as
  planned. Two things were missing for the games' saves to work at all,
  under DOS as on the bare machine: a rename (Doom writes `temp.dsg` and
  renames it; bare.c copies and deletes) and `ftell` on a file being
  written (Doom pads its saves by it; it said -1, so no save of the bare
  libc's had ever loaded back).
- **The page**: `c4dos`, `doom`, `wolf3d` (shared by `wolf3d` and
  `wolf3d6`, whose files are `.WL1` and `.WL6`), `tyrian` and `rott`
  stores, beside C4IX's `c4ix`. `?persist=0`, `?persist=reset` and Reset
  Files act on the current system's alone, and the button names it.
- **Uploads** (the page's button, from smp) stay one medium, `uploads`,
  for every system. They are the person's own files (Wolfenstein's
  registered data, a WAD), read-only on drive 0, where every system
  reads: the bare games look on drive 1 and then drive 0, C4DOS on A:.
  They are not a system's saved state, so the one-store-per-system rule
  is about what systems write; Reset Files leaves them alone, and one
  upload serves `wolf3d6` bare and under C4DOS. worker.js adds them to
  drive 0's `c4dos.dir`, so C4DOS lists them on A: and finds them in any
  case. Forget deletes the one file.

### 3.9 The disk the page boots

The user wants images of the page's own, with things like booting into
grin. `build-images.sh` gains a libjs set, `libjs/images/dos/`:

- `c4dos32.c4r`;
- `CONFIG.SYS` with CLOCK, RAMDISK, CONSOLE FLUSH, `DRIVES HOME=B:`,
  HEAP, and `REDIR TAP`;
- an `AUTOEXEC.BAT` that ends in `GRIN`;
- grin and its applications;
- the four games and their data.

`?system=c4dos` and the four game systems move onto it. Today's
`src/c4bb/images/games` is only there for the page, so it goes once
nothing refers to it. c4bb's own disks are untouched. The headless test
boots the same directory.

**As built in G2.** `build-images.sh` makes `libjs/images/dos` (ignored,
like every built disk) after its C4DOS step, when grin's objects are
there (`make grin-objs`, which `c4bb-images` now builds):

- `c4dos32.c4r`, and `grin.c4r` linked from `.grin/*.c4o` with the bare
  libc, as the games are;
- the games and their data, hard-linked from the games disk (copied
  where links fail), so the disk costs no space twice;
- `CONFIG.SYS`: CLOCK, RAMDISK (16 MB), CONSOLE FLUSH, DRIVES and HEAP.
  `HOME=B:` comes with G4 and `REDIR.SYS TAP` with G5;
- `AUTOEXEC.BAT`: the welcome, `VER`, the games on the disk, a line
  saying File > Exit grin comes back to A>, and `GRIN` last;
- `c4dos.dir`, `manifest.json` and `index.json`, as the games disk has.

`?system=c4dos` boots it (`app.js`). The four bare-machine game systems
still boot `src/c4bb/images/games`: they move with G4, which gives each
its own store.

**As built in G4.** The bare-machine game systems (`wolf3d`, `wolf3d6`,
`doom`, `tyrian`, `rott`) boot from `libjs/images/dos` too, each with its
store. The disk's `CONFIG.SYS` says `DRIVES.SYS HOME=B:`, and
`notepad.c4r` is on it. `src/c4bb/images/games` stays: build-images.sh
makes it and links the DOS disk's game files from it, and
test-c4dos-heap boots it; nothing on the page does. The games disk's `CONFIG.SYS` is now made from
`src/c4dos/fs/CONFIG.SYS` rather than copied from the shared disk, which
in a fresh tree is not there yet at that point (G1's note).

**As built in G5.** The disk's `CONFIG.SYS` has `DEVICE=REDIR.SYS TAP`,
and three DOS programs for redirection are on it: `ASKLINE` (`bare.c`,
reads a line: `src/grin/apps/askline.c`, linked with the bare libc
alone), `DOSFILT` (a c4cc filter, `src/tests/dosfilt.c`; `-r` reads with
`READ` and prints with `printf`, the tap's case) and `HELLO` (c4cc's
`printf`). The AUTOEXEC's ECHO is no longer cut at fourteen words.

## 4. The first applications

- **Program Manager** (built in) is the shell window. In G4 it shows one
  group at a time, with a group list. MDI group windows arrive in G7.
  - Its default groups:
    - **Main**: File Manager, DOS Prompt, Control Panel, Notepad;
    - **Accessories**: Clock, Calculator;
    - **Games**: Wolfenstein 3D, Doom, Tyrian, Rise of the Triad.
  - File > New and Properties edit an item, including its run mode.
  - File > Run... takes a command line.
  - File > Exit grin asks "This will end your grin session" and returns
    to `A>`.
- **File Manager** (built in) has a drive bar (A:, every drive that
  `DISK_COUNT` reports, and RAM:) over a file list.
  - **Listing.** Drives are listed from `c4dos.dir`, and the RAM disk
    from slots 9-12. A disk file's size is fetched when it is selected,
    since the disk has no size query short of reading the file.
  - **Double click:**
    - a `.c4r` runs (G6: as the Program Manager item that names it
      says, window or full screen, else full screen; it does not ask);
    - a text file (`.txt`, `.c`, `.bat`, `.ini`) opens in Notepad;
    - a `.bat` can also be run in a DOS Prompt.
  - **Copy and Delete** work through DOS, on the RAM disk and on
    writable drives.
- **Notepad** (`notepad.c`, shared) is the desktop's Text Editor buffer
  and editing. It adds Open and Save As through `wk_filebox`, Edit >
  Copy and Paste through the clipboard, and Search > Find.
- **Clock** (`clock.c`, shared) shows an analog or digital face, read
  from `GUI_CLOCK`. It is the smallest application and the one G3 proves
  the loader with.
- **Calculator** (`calc.c`, shared) is the desktop's Calculator, with its
  Integer, Single and Double modes.
- **Control Panel** (built in): **Display** (640x480 or 800x600, applied
  at once and saved), Colors (schemes), and Desktop (pattern and
  screen saver).
- **DOS Prompt** (built in, G5): 3.7.
- **Games**: Program Manager items, full screen from G5 and windowed
  from G6, their default since (`Doom=DOOM,game,window`).

## 5. Milestones

Each milestone ends with a headless test and a browser check.

**The headless suite** is `libjs/tests/test-grin.mjs`, modelled on
`test-desktop.mjs`. It boots the libjs DOS disk (3.9) with a
`GuiDevice` and a persist map, and clicks things by the text drawn on
them. It runs as `make test-grin` and joins `test-libjs.sh` once it is
stable. C4DOS's own changes are also tested in `make test-c4dos`, which
must stay green on all four of its legs.

**The browser check** uses `libjs/tests/cdp.mjs` against the user's
Chrome, in a new tab, on `.../libjs/web/?system=c4dos`. Strings are read
back with `display.recordText`, and a screenshot is checked by eye.
Until the branch is merged, the page is served from the branch's own
worktree.

**G0: C4DOS services and heap generations** -- done, branch `grin-g0`
(2026-10-24). `make test-c4dos` (every leg runs `src/tests/dosexec.c`)
and `make test-c4dos-heap` (libjs, HEAP.SYS, Doom five times: 0 bytes
of growth, against 14.8 MB a round without the line) pin it. The games
disk the page boots has `DEVICE=HEAP.SYS`.

- **Machine:** heap generations in `heap.js`, plus `HEAP_GEN` and
  `HEAP_RELEASE` in `devices.js`. `test-heap.mjs` is extended.
- **C4DOS:**
  - re-entrant RUN and `HEAP.SYS`;
  - slots 16-25, with the sinks;
  - `c4dos.h` and `docs/c4dos-design.md` brought up to date.
- **`bare.c`:** console output and input go through slots 16 and 17.
- **Headless tests:**
  - A test program calls `exec` three deep, and the innermost calls
    `exit(3)` from three calls down. Each level sees the right status,
    and the prompt still answers.
  - `load`/`start`/`unload` of a resident test image.
  - `catch` around a function that exits.
  - A RAM-disk file created by a child survives the child.
  - On the games disk at `A>`, Doom run five times with keys to quit
    leaves `HEAP_USED` within 1%. The leak is measured first without
    `HEAP.SYS`.
- **Also:** `make test-c4dos`, including the purity leg, and
  `test-c4bb-climb` stay green.
- **Browser:** at `A>`, run `DOOM`, quit, and repeat. `MEM` reports the
  same free memory each time.

**G1: the window kit**

- `src/winkit/` is extracted from the C4IX desktop: the class table,
  the looks, run-time screen size, and the review's bugs fixed on the
  way.
- The desktop is rebuilt on the kit, by both c4lc and `BUILD.B4K`.
- The desktop's Settings gains the Windows 3.1 look (the window chrome
  from `look31.c`) beside Flat and Classic. The taskbar stays, and
  saving goes to `desktop.cfg` like the other looks. The default does
  not change.
- **Headless tests:**
  - `test-desktop.mjs` stays green, with no visible change under the
    default look.
  - Settings > Windows 3.1: a window's control-menu box and centred
    title are drawn. The choice survives a restart of the desktop.
  - The desktop is told to resize to 640x480 and back, and stays
    usable.
  - The in-C4IX build (`make test-c4dos-c4ix32` and the desktop's
    in-machine build) still links.
- **Browser:** the C4IX desktop's D4 gate (`test-web.mjs --gui`), plus
  a screenshot compared by eye with one from before.

**G2: grin's shell**

- The libjs DOS disk (3.9), with `AUTOEXEC.BAT` ending in `GRIN`.
- `grin.c4r`:
  - the 3.1 look, minimised windows as icons, and the Task List;
  - Control Panel with Display, Colors and Desktop, saved in `GRIN.INI`;
  - File > Exit grin.
- **Headless tests:**
  - Boot to grin's desktop with no typing.
  - Ctrl+Esc lists the windows, and Switch To raises one.
  - Control Panel > Display > 800x600: `GUI_W` reads 800, and a
    maximised window fills it. Back to 640x480 the same way.
  - Exit grin returns to `A>`, and `VER` answers there.
- **Browser:** the page boots straight into grin. Switch the
  resolution, then exit to `A>`.

*Done, branch `grin-g2` (2026-10-24).* What was built:

- **`src/grin/`**, linked into `grin.c4r` with the kit's `draw`, `ui`,
  `wmcore`, `dialog`, `look95`, `look31` and `savers` (`make grin-objs`;
  the kit is compiled as the desktop compiles it, grin's own files with
  `-fno-builtin` so that their files go through libc and `bare.c`):
  - `grin.c`: the start (no display: "grin: this machine has no
    display, so grin cannot run here." and status 1), the loop (the
    desktop's, with `__c4_usleep` for the nap), the desktop's colour and
    pattern, Ctrl+Esc and a double click on the desktop for the Task
    List, `GRIN.INI`, the screen saver, and the kit's host functions
    (`wk_alloc` from libc's `malloc`; files with `open`/`read`/`write`,
    so C4DOS's RAM disk; `wk_readdir` lists nothing until G4). Exit
    closes every window, leaves "grin has ended" on the display, lets it
    go (`gui_detach`) and returns 0 to the batch file.
  - `progman.c`: the placeholder Program Manager: the Main group with
    Control Panel in it, File > Exit grin... and Help > About. Exit asks
    "This will end your grin session." with OK and Cancel; closing
    Program Manager asks the same.
  - `tasklist.c`: the Task List, a dialog-chrome window (modal, as 3.1's
    was): the windows that are not dialogs, Switch To, End Task
    (`wk_ask_close`), Cancel, Cascade, Tile and Arrange Icons. Cascade
    and Tile take maximised windows out of it and leave the icons' row
    clear. Keys: Up, Down, Enter, Esc.
  - `control.c`: Control Panel with Colors, Desktop and Display items.
    Each opens a dialog of radio buttons with OK and Cancel: Display
    (640 x 480, 800 x 600, applied at once through `wk_screen`), Colors
    (the kit's nine schemes), Desktop (five 8x8 patterns, tiled as a
    64x64 image, and the screen saver with its delay and a Test button).
  - `shell.h`, what they share. (`grin.h` stays the name of G3's
    application header.)
- **`GRIN.INI`**: `[grin]` then `width=`, `height=`, `scheme=`,
  `pattern=`, `saver=`, `wait=`, one a line. Read at start (RAM disk,
  then the disk), written on every change. On the RAM disk it lasts
  until power-off: exit grin and `GRIN` again keeps the choices, a
  reload does not, until G4's `HOME=B:`.
- **The kit** (compatible: the C4IX desktop does not ask for icons, and
  test-desktop is unchanged):
  - `wk_min_icons(on)`: minimised windows are drawn by `look31.c` as
    32x32 icons with their titles under them (broken onto two lines at
    the space nearest the middle when long), in places along the bottom
    of the area, under the windows. A click opens the control menu over
    the icon; a double click or the menu's Restore brings the window back
    as it was (maximised or not), and Maximize brings it back maximised.
  - `wk_minimize(w)`, `wk_arrange_icons()`; `struct win` gains `islot`.
  - `WK_OKCANCEL` message boxes (OK and Cancel; Y and N stay Yes/No's).
  - The control menu's Restore and Maximize bring back a minimised window.
- **Tests:** `libjs/tests/test-grin.mjs` (`make test-grin`, and in
  `test-libjs.sh`), 41 checks; `libjs/tests/web-grin.mjs`, the browser
  check, run by hand against a Chrome over CDP.

Deviations and findings:

- **Ctrl+Esc does not reach the page on Windows**: the system takes it
  for the Start menu before the browser sees it. The double click on the
  desktop is the way there; the browser check sends Ctrl+Esc through CDP.
- The headless test has to answer a `gui_size` as the page does
  (`display.js` sends a resize event back, which is what sets `GUI_W`).
- **C4DOS's `ECHO` prints at most 14 words** (`ARGVMAX` 16 in
  `parse_line`); the rest of the line is dropped silently. AUTOEXEC.BAT's
  lines are kept short. Not fixed here.
- The games stay on `src/c4bb/images/games` for the bare-machine
  systems; only `?system=c4dos` moved (3.9).

**G3: applications**

- `grin.h`, `grin/import.c`, and the kit's table in slot 25.
- Resident applications started by `load`/`start`/`unload`, with every
  call into them made through `catch`.
- Clock and Calculator from the shared sources.
- **Headless tests:**
  - Two Clocks both tick. Close one and the other ticks on. Close both
    and `HEAP_USED` returns to within a few hundred bytes of where it
    started.
  - Typing `12*12=` into Calculator shows `144`.
  - An application that calls `exit()` from a key callback is unloaded,
    and grin keeps running.
  - The C4IX desktop's Calculator still passes `test-desktop.mjs`.
- **Browser:** Clock and Calculator side by side, at both resolutions.

*Done, branch `grin-g3` (2026-10-25).* What was built (3.4's "As built
in G3" has the mechanism):

- **`src/grin`**: `grin.h` (the application header), `gkit.h` and
  `kitfns.h` (the table), `import.c` (the applications' half), and
  `apps.c` (grin's: the table in slot 25, proxies and trampolines,
  `grin_run`, `apps_reap`). `grin.c` calls `apps_init`, `apps_reap`
  after `wk_tick`, and `apps_end`.
- **The applications**, `src/grin/apps`: `clock.c` and `calc.c`, a
  `main` of a few lines each around the kit's `clock_open` and
  `calc_open`, and `exitkey.c`, test-grin's: a window whose key callback
  calls `exit(3)`, with 100 KB of its own allocated in `main`. `make
  grin-apps` compiles them, the kit's `clock.c` and `calc.c` and
  `import.c` into `.grinapp/`, all with `-D WK_IMPORT` (kept apart from
  `.grin/`, which build-images.sh links whole into `grin.c4r`).
  build-images.sh links `clock.c4r`, `calc.c4r` and `exitkey.c4r` onto
  `libjs/images/dos`, each with the bare libc, about 0.5 MB apiece.
- **Program Manager**: an Accessories group beside Main, with Clock and
  Calculator (double click, or Enter on the selected item; Left and
  Right move between the three items); File > Run..., an input box whose
  command line `grin_run` starts; Help > About says how many
  applications are running.
- **The Task List's Tile**: a window that cannot be resized
  (Calculator) keeps its size and the others in its row share what is
  left, so Clock and Calculator fit side by side at 640x480; before, it
  gave every window an equal column and pushed the Calculator off the
  screen.
- **Tests:** test-grin.mjs gains 29 checks (70 in all); web-grin.mjs
  gains the applications at both sizes (screenshots `grin-g3-640x480`
  and `grin-g3-800x600`).
  - Two Clocks (one image: About says 1 running), both ticking in
    figures (Settings > Digital); End Task on one, the other ticks on;
    End Task on both, the image is unloaded and `HEAP_USED` is back to
    the byte (4,928,561 before and after).
  - Calculator: `12*12=` typed shows `144`.
  - File > Run... `DOOM`: "DOOM is not a grin application", unloaded.
  - File > Run... `EXITKEY`; a key: its window is closed, it is
    unloaded, and `HEAP_USED` is back to the byte, its 100 KB with it;
    a Clock starts and closes after it.
  - A Clock left open when the session ends: at `A>`, `MEM` says
    `programs loaded: 0`, and `CLOCK` there says it needs grin (the
    table was taken back).

Deviations and findings:

- `-D WK_IMPORT` rather than `#define wk` before the include, and memory
  and files from the application's own libc rather than forwarded (3.4).
- The shell's part of `winkit.h` is not offered to applications: an
  application that called `wk_init` or `wk_tick` would break grin.
- The `setgen` bracket is in `import.c`'s stubs, one place, not in
  grin's table entries; drawing calls are bracketed too (two DOS calls
  each), for uniformity rather than need.
- `grin_run` starts applications only. A program that does not say
  hello is refused with a message box; DOS programs come with G5.
- grin's applications were about 0.5 MB each on the disk, the bare libc
  linked whole into each, as it was into grin and the games. Since
  2026-10-27 c4rlink drops the functions nothing reaches, and grin's
  links name libc's objects as library members (docs/c4dos-design.md
  "Linking a bare program"): Clock is 67 KB, Notepad 103 KB, Calculator
  245 KB, grin 548 KB.
- The headless Clocks read the host's time of day (`GUI_CLOCK` is
  `Date`), so test-grin waits for the ticks in host time, a second or
  two.

**G4: persistence and the shell applications** -- done, branch `grin-g4` (below)

- `DRIVES.SYS HOME=`, with writes, `unlink` and `c4dos.dir` upkeep.
- `bare.c` writes to drive 1 on the bare machine.
- A store per system on the page: `c4dos`, and `doom`, `wolf3d`,
  `tyrian`, `rott` for the bare-machine games.
- Program Manager, File Manager and Notepad.
- **Headless tests:**
  - Notepad saves `NOTE.TXT`, and File Manager lists it on B:.
  - Boot again with the persist map that came back: the file is still
    there, and so is the resolution chosen in Control Panel.
  - Delete removes it.
  - Doom on the bare machine saves a game, and the save is there when
    Doom boots again with the map that came back. Doom under C4DOS
    saves to B:, and its save is not in the bare game's map: the stores
    are kept apart.
  - Program Manager's items launch the right things.
- **Browser:**
  - Save a note and reload the page: it is still there.
  - `?persist=reset` forgets it.

*Done, branch `grin-g4` (2026-10-25), with smp 0f93fa2 merged.* What was
built (3.8 and 3.9 have the persistence; docs/c4dos-design.md, "The home
drive", C4DOS's):

- **Program Manager** (`progman.c`) reads `PROGMAN.INI` from the home
  drive, and writes it from the defaults when there is none: Main (File
  Manager, DOS Prompt, Control Panel, Notepad), Accessories (Clock,
  Calculator), Games (the four games). A line is
  `title=command line,icon,mode`, the mode `app`, `full` or `window`.
  One group is shown at a time, chosen from a list down its left. File >
  New... and Properties... open one dialog (description, command line, an
  icon stepped through with its button, the run mode as radio buttons);
  Delete asks; each writes `PROGMAN.INI` at once. Built-in windows start
  by command line, `FILEMAN` (or `WINFILE`), `CONTROL` and `PROGMAN`
  (`grin_start`, which File > Run... uses too). DOS Prompt (`DOSPRMPT`)
  is greyed and says it comes later; an item whose mode is `full` or
  `window` says it is a DOS program and to run it at A> (G5, G6).
- **File Manager** (`filemgr.c`): a drive bar of A:, every drive DOS
  reports (slot 29) and RAM:, the home drive marked "(files are saved
  here)"; a list from the drive's `c4dos.dir` or the RAM disk's slots; a
  status line with the selected file's size, read through when it is
  selected, or how many files; the list read again when the window comes
  to the front, and by File > Refresh and F5. A double click opens a text
  file (`.txt .c .h .bat .ini .cfg .md .doc .sys .dir .frm .lst .json .s`)
  in Notepad and starts a `.c4r` as an application. File > Copy...
  (`COPY` through exec, slot 19, offering the home drive, or RAM: from
  there) and Delete... (unlink, slot 18, after asking).
- **Notepad** (`apps/notepad.c`, `notepad.c4r`): the kit's `notepad.c`
  with `notepad_filebox` set, so Open and Save As are the kit's file box;
  a new document is offered `UNTITLED.TXT` on the home drive. The C4IX
  desktop leaves the switch off and keeps its typed paths
  (test-desktop.mjs unchanged).
- **The file box's folders are DOS's drives.** `disk.c` gives the kit
  `wk_readdir` (the root lists A:, B:, ... and RAM:; a drive lists its
  files) and the file functions; `dospath.h` maps the kit's
  `/B:/NOTE.TXT` to DOS's `B:NOTE.TXT` and back, for grin and for every
  application's `import.c`.
- New pictures: File Manager's cabinet, a DOS screen, a notepad, a
  joystick for the games, a window for any other application.
- **Tests:** test-grin.mjs gains 43 checks (113 in all), with a drive B:
  kept as the page keeps a store: the G4 list above, NOTE.TXT saved
  through the file box onto B: and into its `c4dos.dir`, File Manager's
  size and Copy, 800x600 in GRIN.INI on B:, then a second boot with what
  B: kept: grin at 800x600, NOTE.TXT opening in Notepad, and Delete
  taking it off B: and out of the store. test-persist.mjs (`make
  test-persist`, in test-libjs.sh): Doom saves a game on the bare machine
  and loads it back on the next boot (`-loadgame 0`); Doom under C4DOS
  saves to B: and loads it back; the bare game's store is untouched.
  test-c4dos gains a c4bb leg with `HOME=B:` and a writable B:.
  web-grin-g4.mjs is the browser check (a note saved, the page loaded
  again, `?persist=reset`).

Deviations and findings:

- **API v5** rather than more of v4: slots 28 and 29, for File Manager
  and Notepad to find the drives and the home drive.
- **A machine register and an info bit** the plan did not name:
  `DISK_UNLINK` (deletes on drives had no way to reach the medium) and
  `C4I_DRV` (bare.c's "INFO bits, checked once" needed a bit to check).
- **`DEL`/`ERASE`** at A>, beside `unlink`.
- **bare.c's rename and `ftell`**: needed for any game save to work; the
  games are unchanged.
- **Uploads** are one store for every system (3.8, "As built in G4").
- **Program Manager's groups are a list beside one group**, as planned
  for G4; File > New makes items only (no new groups yet).
- Doom keeps its saves as `doomsav0.dsg` (it writes `.savegame/temp.dsg`
  and renames it to a name without the folder, and finds it again by the
  drive's basename fallback), and this port writes no config when it
  quits.
- `src/c4bb/images/games` is not removed (3.9).

**G5: DOS programs, redirection, DOS Prompt**

- Full-screen launch, with the display handed back and `snd_reset`.
- `REDIR.SYS`, the console tap in `devices.js`, and DOS Prompt windows.
- **Headless tests, at `A>`:**
  - `DIR > LIST.TXT`, then `TYPE LIST.TXT`;
  - `HELLO > OUT.TXT`, with a c4cc program, through the tap;
  - a `bare.c` program that reads a line, given `< IN.TXT`;
  - `A | B` through a temporary file.
- **Headless tests, in grin:**
  - Doom full screen from Program Manager. Quit it with keys, and grin
    redraws.
  - Two DOS Prompt windows. `DIR` in one lands in that one. A `bare.c`
    program reading a line takes what is typed in its window, while a
    Clock keeps ticking.
- **Browser:** Wolfenstein 3D full screen, play a little, and quit back
  to grin. Then a DOS Prompt with `DIR | ...` and `TYPE`.

*Done, branch `grin-g5` (2026-10-26), with smp fb313d0 merged.* What was
built: 3.6 and 3.7 ("As built in G5"), 3.9, and `docs/c4dos-design.md`
("Redirection: DEVICE=REDIR.SYS", "The console tap", API v6).

- **C4DOS:** `REDIR.SYS` (`>`, `>>`, `<`, `|` through `RAM:PIPEn.$$$`,
  batch files too), opt-in, so a line without the device is parsed as
  before; the builtins, their errors and a batch file's echo print
  through `pf` and conwrite instead of `printf`; ECHO says the rest of
  the line; `TYPE /P` pauses through conread; the console tap used with
  `REDIR.SYS TAP`, and said once to be missing without it.
- **Machine:** the tap's four registers in `devices.js` (0x1d0-0x1dc,
  free until now), which c4bb's CLI and libjs share; libjs's `PRTF`,
  `PUTC` and `PUTS` reach them through `emit`.
- **grin:** the full-screen launch, DOS Prompt windows, `grin_pump`.
- **Tests:** test-c4dos's redirection legs (native, tower, and the tap on
  c4bb with `HOME=B:`); test-grin's G5 checks (37 more, 150 in all): at
  A>, `DIR > LIST.TXT` and `TYPE LIST.TXT` (on B:), `HELLO > OUT.TXT`
  through the tap, `ASKLINE < IN.TXT`, `DIR | DOSFILT` and `TYPE IN.TXT |
  DOSFILT -r`; in grin, File > Run... HELLO full screen; two DOS Prompts,
  `ECHO` in one and `DIR` in the other each in its own; `ASKLINE` reading
  a line in DOS Prompt 2 while a Clock ticks, Doom refused meanwhile and
  DOS Prompt 1 saying busy; `HELLO`, `DIR | DOSFILT` and `ECHO ... >
  B:RUNME.BAT` in a window; EXIT; File Manager running RUNME.BAT in a DOS
  Prompt; Doom full screen from Program Manager twice, quit with F10 and
  Y, grin back at 800x600, `HEAP_USED` the same to the byte.

Deviations and findings:

- **API v6, slot 30** (`drive()`), which the plan did not name: a DOS
  Prompt window keeps its own drive, and DOS's prompt drive is global.
- **A sink runs as the image that installed it** (C4DOS's `sink_call`):
  without that, Clock started while a DOS Prompt's program waited would
  have been that program's child and unloaded with it.
- **`DIR` with `HOME=B:` listed B:** at A> (G4's `dos_open` looks on the
  home drive first); it now names the prompt's drive's listing.
- **File Manager's File > Edit**, since a double click on a `.bat` now
  runs it.
- DOS Prompt windows are numbered, so that the Task List (and the test)
  can tell them apart; at 640x480 a window is wider than the screen.
- A full-screen DOS program's text goes to the page's console pane, as
  at A>.
- A c4cc program in a DOS Prompt reads end of input from fd 0 (the
  page's keyboard would be the console pane's, not the window's).
- **The disk's AUTOEXEC said `ECHO ... back to A>.`**, which REDIR.SYS
  took for a redirection into a file called `.` on B:; it says "the A
  prompt" now, and test-grin checks there is no such file. `DIR` leaves
  out the pipe files (`.$$$`).
- **Browser** (Chrome 151 on 9222, the page served from the worktree on
  port 8494 and loaded through https://8494.code.home.stargazer.onl, a
  secure context): `web-grin-g5.mjs`, all OK, no page errors. Wolfenstein
  3D from Program Manager full screen, a new game on floor 1 (moved,
  turned, one shot), Esc > Quit > Y, and grin drew Program Manager again;
  then DOS Prompt 1 with `DIR | DOSFILT`, `ECHO ... > B:G5.TXT`, `TYPE
  B:G5.TXT` and `HELLO`.

**G6: games in a window**

- A grin back-end in each bare platform (`plat-bare.c`, `dg-bare.c`,
  `rott-bare.c`, `tyr-bare.c`): the C4IX `*_win_*` trios re-pointed at
  `grin_fwin_*`, about 50 lines each.
- Window sizes 1x and 2x. Keys go to the game only while its window has
  focus. Pointer lock is granted on a click in the window and released
  on Esc.
- **Headless tests:**
  - Doom windowed: frames arrive in the window's rectangle, and the
    Clock's seconds advance meanwhile. That is the proof of cooperation.
  - Closing the window ends Doom, and grin carries on.
  - Wolf3D, Tyrian and ROTT each draw one frame in a window and close.
- **Browser:** Doom in a window beside Clock and Notepad, typing in
  Notepad while the demo plays. Frames per second are measured windowed
  and full screen for each game.

*Done, branch `grin-g6` (2026-10-27), on `grin-plan` afccbdb (smp fb313d0
already in it).* What was built: 3.6 ("As built in G6"), C4DOS API v7
(`docs/c4dos-design.md`), and each game's bare platform layer, on a
`grin-g6` branch of its own repository (wolf3d 1531fc2, doom 0de63a9,
tyrian be9c515, rott 244588f, each on its `c4` branch's commit).

- **grin:** `src/grin/fwin.c` (the window, its events, pointer lock,
  sizes, the windowed launch), grin.c's `pass(PASS_GAME)`, `win_run` and
  the table out of slot 25 during full screen, Program Manager's window
  items and `grin_dos`; the kit's `wk_menu_open()`.
- **C4DOS:** slot 35, `image(h)`, version 8. (G6 made it slot 31 and
  version 7; C4DOS's directories had taken both meanwhile, so the merge
  of the two, 2026-10-27, moved it.)
- **Games:** Doom, Tyrian and ROTT keep their C4IX-shaped window trios,
  pointed at grin; Wolf3D's `plat-bare.c` opens grin's window itself.
  50 to 80 lines each with the table's declaration. Full screen and the
  bare machine unchanged.
- **Tests:** test-grin's G6 checks (174 in all): Doom from Program
  Manager in a window, 320x200 frames at 2x in the window's rectangle at
  800x600, Program Manager drawn beside it, the Clock's seconds advancing
  meanwhile, Notepad (switched to) taking typing while Doom draws on;
  its control menu's Close ends Doom, `HEAP_USED` back where it was to
  the byte (5,973,954 both times) and again for a second round; Wolf3D
  from File Manager, Tyrian and ROTT each drawing a frame in a window and
  closed; File > Run... `DOOM -timedemo demo3 -show` in a window, its Run
  box opened meanwhile and left open over it while the timedemo plays
  its 2,134 frames to the end by itself, then CLOCK typed into that box
  starts a Clock and the Task List has no Doom (3.3's generation check).
  test-c4dos-heap's `dosexec image`. G5's full-screen Doom rounds set
  Doom's item to full screen by Properties first.
- **The gate**, on helios: `make test-grin` OK (174), `test-c4dos` OK,
  `test-c4dos-heap` OK, `test-persist` OK, `test-libjs` OK (558 OK
  lines), `test-c4ix-rebuild` OK, and `test-wolf3d`, `test-doom`,
  `test-tyrian`, `test-rott` OK with their timedemo checksums unchanged
  (d7358845, 01914fe7, 389b26f7, 3b083b33).
- **Browser** (Chrome 151 over CDP on 9222, the page served from the
  worktree on 8494, `web-grin-g6.mjs`, no page errors): Doom in a window
  at 800x600 beside a Clock and Notepad, typing in Notepad while the demo
  plays, Size > 1x and back, End Task closing it with the Clock and
  Notepad still there. Screenshots in the session notes' `grin-g6-shots/`.

**Frames per second** in the browser (Chrome 151 on the user's machine,
`?system=c4dos`, 15 s each, counted as the 8-bit frames the page drew;
each from a fresh boot at 640x480, so the windows are 1x, unless said):

| game, and what it was doing | window | full screen |
|---|---|---|
| Doom, its demo | 29.8 (1x); 29.7 at 2x and 31.6 at 1x at 800x600 beside Clock and Notepad | 42.1 |
| Wolfenstein 3D, floor 1, standing | 70 (its cap) | 69.9 |
| Tyrian, its opening | 10.9 | 11.2 |
| Rise of the Triad, its opening and demo | 11.3 | 10.7 |

Doom loses about 30% in a window: each frame is copied once more, and
grin's redraw draws the window's chrome and whatever else the damage
touches (a Clock's tick widens it) before the blit; the others are held
by their own pace, not by the drawing.

Deviations and findings:

- **The game-window calls are in the table, not in `grin.h`**, at words 5
  to 7 of version 2: the games are DOS programs and never link
  `import.c`, so they read slot 25 themselves. Putting them before the
  kit's functions moved those one word, once (every application is built
  with grin).
- **C4DOS v8, `image(h)`**, not in the plan: without it an application
  started from Program Manager while a game played was the game's child,
  and C4DOS unloaded it with the game while grin still had its windows.
- **`fwin_frame` copies the frame**; the plan had grin keep the game's
  pointer. A copy means a redraw for another window's sake always has a
  whole frame, whatever the game is doing to its buffer.
- **Pointer lock only for a game that steers with the mouse** (Tyrian,
  ROTT, `fwin_open`'s flag); Doom and Wolf3D take keys only, as they do
  full screen.
- **The table leaves slot 25 while a full-screen program runs**, so the
  game's bare layer needs no second test for "grin is here but has given
  the display up".
- **Programs started from File Manager or File > Run** follow Program
  Manager's item for the same program (window or full screen), as a PIF
  did; the plan's "asking full screen or window the first time" is not
  built. A windowed program that never opens a window gets a message box
  with its console's last line.
- **Games typed in a DOS Prompt** play in a window too, their console
  text in the DOS Prompt.
- **Program Manager's games default to `window`**; a PROGMAN.INI written
  before G6 keeps `full` until its Properties change it.
- **Doom's console message** under grin is "a program has the display;
  playing in the window" (dg-gui.c's, unchanged); grin keeps the windowed
  launch's console text, so no one sees it unless the game fails.
- At 640x480 a 2x window is 8 pixels wider than the screen, so 1x is the
  default there.
- **Found on the way:** the shell's `time` on helios (dash) prints no
  `real` line; test-grin's own helpers `endTask`/`inTasks` miss Task List
  rows drawn after its buttons in a partial frame (G6's checks use
  `endTask2`, which finds rows by place).

**G7: finishing**

- MDI group windows in Program Manager, with Cascade and Tile.
- The screen saver.
- The clipboard across applications.
- A startup chime on the OPL.
- `docs/grin.md` replaces this plan as the design and status document.
- **Tests:** a headless check for each feature.

**Rough sizes, by analogy with the C4IX desktop:**

| milestone | lines | notes |
|---|---|---|
| G0 | 900 | C4DOS 500, machine 150, tests |
| G1 | 1,500 | mostly moved code, plus the 3.1 look and relayout |
| G2 | 900 | |
| G3 | 600 | |
| G4 | 1,600 | |
| G5 | 900 | |
| G6 | 300 | |
| G7 | 1,000 | |

## 6. Order and dependencies

- **G0 and G1 are independent.** They touch different trees (C4DOS
  and the machine, against the C4IX desktop) and can run in parallel
  sessions.
- **G2 to G7 follow in order.**
- **G1 is the delicate one.** It moves code out of the C4IX desktop
  while that desktop is still being worked on (the "C4IX desktop
  environment improvements" line of work). It should land when nothing
  else is changing `src/c4ix/user/desktop/`, or as part of that work.

## 7. Risks

- **The run-frame trampoline, now used re-entrantly.** It depends on the
  exact frames c4lc emits, and `-O` has broken it before (the `invoke0`
  tail-call bug, `c4dos.c:225-232`). G0 tests three-deep nesting and
  exits from inside calls, with and without `-O`.
- **Heap generations misattributed.** If DOS or grin allocates while a
  child's generation is current, releasing the child frees memory that
  is still in use. Every allocating entry point brackets the generation,
  and G0, G3 and G6 test the cases (a RAM-disk file, a resident
  application, a dialog during a game).
- **Sharing with the C4IX desktop.** Shared code must compile under
  c4lc on the host and under c4sc inside C4IX (whose per-file budgets
  are tight: `edit.c` already needs a 4 MB stack to compile). The move
  must also not collide with work on the desktop now under way.
  Mitigations: G1 is a move with no visible change, pinned by the
  desktop's own tests, and the kit stays in small modules.
- **No protection.** An application or game that scribbles on memory
  takes grin down. This is accepted, as it was for Windows 3.0.
- **One shared 1 MB stack.** Nesting goes DOS, grin, then a DOS program.
  The games already share the stack with C4DOS, and grin adds a few
  hundred words.
- **Text widths are guesses.** The host's fonts cannot be measured from
  the guest. Mitigations, in order: generous fixed layouts, then
  fixed-advance text where alignment matters, and only if that is not
  good enough, a measuring register in `gui-device.js`. The worker does
  not see the page's fonts, so that last one needs thought.
- **c4cc programs in a DOS Prompt freeze grin.** grin has no hold on
  them, so it stops until they return, and their output appears all
  at once. This is how Windows 3.x standard mode behaved, and it is
  stated here so it is not a surprise.
- **Re-entrancy while a game pumps the loop.** An application callback
  can run in the middle of a game's frame. grin starts no DOS program
  from inside a pump, refuses Exit while one runs, and unloads closed
  applications only at top level.
- **Persistence size.** Every file closed on B: is posted to the page and
  kept whole in IndexedDB. Large saves, ROTT's especially, cost a copy
  each time. This is no different from C4IX today.
- **Keyboard focus is the browser's.** Keys reach the display only while
  the canvas has focus. Booting straight into grin avoids the console
  pane, but a first click on the canvas is still needed.

## 8. Decisions and open questions

**Decided by the user (2026-10-24):**

1. The look is Windows 3.1.
2. Both 640x480 and 800x600, switched in the Control Panel.
3. Share the code with the C4IX desktop instead of duplicating it. This
   led to `src/winkit/`, to the shared accessories, and to C4DOS
   providing loading and the command interpreter instead of grin
   copying them.
4. The heap, C4DOS and libc changes are acceptable.
5. Boot into grin from `AUTOEXEC.BAT`, on a disk set of the page's own.
6. Persistence for C4DOS, and for the bare-machine games too.
7. File redirection in C4DOS, opt-in through a `DEVICE=` line, so that
   DOS Prompt windows work in grin, cooperatively. (Built in G5:
   `DEVICE=REDIR.SYS`, `docs/c4dos-design.md` "Redirection".)
8. The command is `GRIN`.
9. One persistence store per system, not one shared store. The planned
   ladder system needs a store of its own (3.8).
10. The device lines are `HEAP.SYS`, `REDIR.SYS` (with `TAP`) and
    `DRIVES.SYS HOME=B:`.
11. The C4IX desktop also offers the Windows 3.1 look, as an option in
    its Settings (G1).
12. G0 and G1 run in parallel.

Nothing is open.
