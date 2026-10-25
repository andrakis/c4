# grin: a graphical interface for C4DOS (plan)

Status: written 2026-10-24 against `smp` at 6295e18, and revised the
same day with the user's answers (section 8). **G0 and G1 are built**
(branches `grin-g0` and `grin-g1`, merged here on 2026-10-24), and
**G2 is built** (branch `grin-g2`, 2026-10-24). Sections 3.2 and 3.3
say what G0 built where it differs from the first draft, 3.4 shows the
window kit's API as G1 built it, and 3.9 and the G2 milestone say what
G2 built. The rest is plan.

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

- **Built** by c4lc and linked whole with libc's `bare.c` layer.
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
comes with the DOS programs (G5), the game-window calls with G6.

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
its own store. The games disk's `CONFIG.SYS` is now made from
`src/c4dos/fs/CONFIG.SYS` rather than copied from the shared disk, which
in a fresh tree is not there yet at that point (G1's note).

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
    - a `.c4r` runs, asking full screen or window the first time;
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
- **DOS Prompt** (built in): 3.7.
- **Games**: Program Manager items, full screen from G5 and windowed
  from G6.

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
- grin's applications are about 0.5 MB each on the disk, the bare libc
  linked whole into each, as it is into grin and the games.
- The headless Clocks read the host's time of day (`GUI_CLOCK` is
  `Date`), so test-grin waits for the ticks in host time, a second or
  two.

**G4: persistence and the shell applications**

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
   DOS Prompt windows work in grin, cooperatively.
8. The command is `GRIN`.
9. One persistence store per system, not one shared store. The planned
   ladder system needs a store of its own (3.8).
10. The device lines are `HEAP.SYS`, `REDIR.SYS` (with `TAP`) and
    `DRIVES.SYS HOME=B:`.
11. The C4IX desktop also offers the Windows 3.1 look, as an option in
    its Settings (G1).
12. G0 and G1 run in parallel.

Nothing is open.
