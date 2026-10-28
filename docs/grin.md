# grin: a graphical shell for C4DOS

Status: **G0 to G7 are built** (2026-10-24 to 2026-10-28). G8, a system
monitor, and G9, two games at once, are planned and not built (section
9). This document is grin's design and status: what grin is, how it is
built and how it works, each milestone as it was built and where it left
the plan, and what is left. [grin-plan.md](grin-plan.md) is the plan it
was built from, kept whole: its sections 3.2 to 3.9 ("As built in ...")
and its milestones have every particular this document summarises, with
the reasons and the measurements.

`grin` is to C4DOS what Windows 3.1 was to MS-DOS: a program started from
`AUTOEXEC.BAT` that takes over the screen and gives you overlapping
windows, Program Manager, File Manager, a Task List, Control Panel and a
few accessories. It runs C4DOS's own programs -- full screen, in a window
of their own (the four games), or in a DOS Prompt window -- and hands the
`A>` prompt back when you exit it.

## 1. Where it runs

- **The page.** `libjs/web/?system=c4dos` boots C4DOS from the page's own
  disk, `libjs/images/dos`, whose `AUTOEXEC.BAT` ends with `GRIN`. File >
  Exit grin returns to `A>`; `GRIN` there starts it again.
- **Under C4KE**, in the Build Ladder (`?system=c4dos-ladder`,
  docs/c4dos-ladder.md): grin is a C4KE task, and its DOS Prompt is a C4KE
  Prompt whose lines run as tasks.
- **Not** on c4bb or native c4m, which have no display: grin says so and
  returns status 1.

## 2. How it is built

| what | where | built by |
|---|---|---|
| grin itself, `grin.c4r` | `src/grin/*.c`: `grin.c` (start, loop, GRIN.INI, the chime, the desktop), `progman.c`, `filemgr.c`, `tasklist.c`, `control.c`, `dosprmpt.c`, `fwin.c` (games in a window), `apps.c` (the applications), `disk.c` (C4DOS's files) | `make grin-objs` into `.grin/`, linked by `build-images.sh` with the bare libc |
| the window kit | `src/winkit/`: `draw.c`, `ui.c`, `wmcore.c`, `dialog.c`, `look95.c`, `look31.c`, `savers.c`, `textgrid.c`, and the accessories `calc.c`, `notepad.c`, `clock.c` | the same objects, `wk-*.c4o`; shared with the C4IX desktop, which c4lc builds on the host and c4sc inside C4IX (`BUILD.B4K`) |
| grin's applications | `src/grin/apps/*.c` (a `main` each) with the kit's class and `src/grin/import.c` | `make grin-apps` into `.grinapp/`, with `-D WK_IMPORT`; `clock.c4r`, `calc.c4r`, `notepad.c4r`, `exitkey.c4r` (test-grin's) |
| the disk | `libjs/images/dos`: C4DOS, grin and its applications in `GRIN\`, the games in `GAMES\NAME\`, `CONFIG.SYS` with CLOCK, RAMDISK, CONSOLE FLUSH, `DRIVES.SYS HOME=B:`, HEAP.SYS and `REDIR.SYS TAP` | `make c4bb-images` (`build-images.sh`) |

Tests: `make test-grin` (`libjs/tests/test-grin.mjs`, headless, about ten
minutes); `make test-c4dos`, `test-c4dos-heap` and `test-persist` for
C4DOS's side; the `web-grin*.mjs` browser checks, by hand over CDP.

## 3. How it works

### 3.1 Three kinds of program

| kind | what | how it runs |
|---|---|---|
| built in | linked into `grin.c4r` | called from grin's loop: Program Manager, File Manager, Task List, Control Panel, DOS Prompt, dialogs |
| grin application | a `.c4r` whose `import.c` says hello to grin | loaded by DOS (`load`, `start`) and kept while it has a window; any number at once, cooperatively; every call into it under DOS's `catch` and in its own heap generation |
| DOS program | anything else | run by C4DOS's own interpreter (`exec`), one at a time: full screen, in a window of grin's (a game whose bare layer finds grin's table), or in a DOS Prompt |

This is Windows 3.x's split. C4DOS does the loading (its RUN was made
re-entrant in G0, API v4 and on: grin-plan.md 3.2); grin has no loader.

### 3.2 Memory

The machine keeps a generation on each heap block (`HEAP_GEN`,
`HEAP_RELEASE`, `DEVICE=HEAP.SYS`). DOS gives each image a generation and
releases it when the image is unloaded, so a program's memory goes with
it. grin runs the kit in its own generation: every stub in `import.c`
does `setgen(grin's)` around the call, and every call into an application
does `setgen(the application's)` around its `catch`. So a window or dialog
grin opens for an application is grin's, and what an application
allocates goes when it is unloaded (grin-plan.md 3.3; G3's checks: the
heap back to the byte after a Clock, Calculator, EXITKEY, Doom).

**The clipboard is grin's memory** by that same rule (G7): an application's
`wk_clip_set` reaches grin's through its `import.c` stub, in grin's
generation, and the kit copies the text with grin's `malloc`. The text
outlives the application that copied it; test-grin pastes Notepad's text
into Calculator after Notepad has been unloaded.

### 3.3 The kit's table

grin publishes `struct grin_kit` (`gkit.h`) in C4DOS's slot 25: a magic,
a version, grin's generation, the kit's `wk_env` (screen, look, colours),
`hello`, the game window's three calls, then the kit's functions in
`kitfns.h`'s order, one line each. Version 3 (G7) added the clipboard's
keys (`ui_key_copy`, `ui_key_cut`, `ui_key_paste`) at the end. An
application refuses a grin whose table is older than it was built for;
the games need version 2.

### 3.4 The loop

`pass()` in `grin.c`: the display's events, `wk_tick` (timers, idle
hooks), the screen saver, a redraw of the damage, and a nap until the next
thing due. At the top of the loop it also reaps dead or windowless
applications and runs one waiting DOS program. A DOS program waiting in
grin pumps the same pass: a DOS Prompt's program reading a line, a game
polling its window (no nap, no saver, its frame drawn at once).

### 3.5 DOS programs

- **Full screen**: grin gives the display up (`gui_detach`), takes its
  table out of slot 25, and runs the line with `exec`; then takes the
  display back at its size, quiets the sound card, and draws everything.
- **In a window** (the games, G6): the game's bare platform layer finds
  the table and calls `fwin_open`, `fwin_frame`, `fwin_poll`; grin copies
  each frame and blits it in its next redraw, 1x or 2x; keys go to the
  game while its window has the focus; Tyrian and ROTT get pointer lock.
- **In a DOS Prompt** (G5): C4DOS's interpreter with its console sink
  pointed at a text window, so builtins, batch files and redirection work
  as at `A>`. A `bare.c` program's read pumps grin's loop until a line is
  typed; a c4cc program's output comes through the console tap when it
  ends.
- One at a time: grin refuses a second one with a message box.
- Program Manager's item says which way (its mode, as a PIF said), and File
  Manager and File > Run follow the item that runs the same program.

### 3.6 Persistence

`DRIVES.SYS HOME=B:` makes B: the home drive, which the page keeps in
IndexedDB, a store per system (`c4dos`, and each bare game's). grin keeps
`GRIN.INI` (screen size, colours, pattern, saver, wait, chime) and
`PROGMAN.INI` (the groups, each with its window's place since G7, and
their items) there.

### 3.7 Program Manager

Groups as windows inside Program Manager (MDI, G7): each with a title
bar, control-menu box, minimise and maximise buttons and a sizing border;
overlapping, the active one in front. A minimised group is an icon along
Program Manager's bottom; a maximised one fills it and Program Manager's
title names it, `Program Manager - [Main]`. The Window menu has Cascade
(Shift+F5), Tile (Shift+F4), Arrange Icons, and the groups by number;
Ctrl+F6, Tab and PgDn go to the next group. Each group's line in
PROGMAN.INI carries its place: `[Main] 6,6,340,190,0,3,0` is x, y, width,
height, state (0, 1 minimised, 2 maximised), its place in the order and
its icon's slot. A PROGMAN.INI from before G7 has the bare `[Main]`: Main
then opens over the icons' row and the other groups are icons, as 3.1
first showed them. Items: double click or Enter starts one; File > New,
Properties and Delete edit them in the active group.

### 3.8 The clipboard

One for every window (the kit's `wk_clip_set` and `wk_clip_get`), grin's
memory (3.2). Who copies and pastes (G7):

| where | copy | cut | paste |
|---|---|---|---|
| Notepad | Edit > Copy, Ctrl+C, Ctrl+Ins: the selection | Edit > Cut, Ctrl+X, Shift+Del | Edit > Paste, Ctrl+V, Shift+Ins: at the caret, over the selection |
| every text field (the Run box, the file box, Program Manager's Properties) | Ctrl+C, Ctrl+Ins: the whole field | Ctrl+X, Shift+Del: the whole field | Ctrl+V, Shift+Ins: the clipboard's first line, at the caret |
| Calculator | Ctrl+C, Ctrl+Ins: the display | -- | Ctrl+V, Shift+Ins: the text typed in, a key a character |
| DOS Prompt | Edit > Copy, Ctrl+Ins: the screen's text, as Print Screen took a DOS program's in standard mode | -- | Edit > Paste, Ctrl+V, Shift+Ins: typed in, up to the first line that runs a command (a running program's input takes it all) |

Notepad's selection: Shift with the moving keys, a drag, or Shift+click;
Edit > Select All (Ctrl+A); typing replaces it, Delete and Backspace take
it out. Ctrl+C stays ^C in a DOS Prompt.

### 3.9 The screen saver

`savers.c`'s five (Starfield, Polygons, Beziers, Marquee, Digital Rain),
chosen in Control Panel's Desktop with its delay in minutes, kept in
GRIN.INI as `saver=` and `wait=`; built in G2. It starts when there has
been no input for the delay (not while a game plays in a window), the
first key or movement ends it, and the Desktop dialog's Test runs it at
once. Under grin Marquee says "grin -- C4DOS in the manner of Windows 3.1"
(`saver_marquee`, G7; the C4IX desktop keeps its own line).

### 3.10 The startup chime

Four notes rising, C E G C, struck 150 ms apart on OPL channels 0 to 3
and left to ring for 1.4 s, in a bell's voice (a modulator three times
the note over the carrier, no sustain). `grin_chime()` plays it once
grin's first frame is up, only where the machine announces a sound card
with an OPL (`snd_present`): the card's stream is stamped, so the whole
chime is written at once and plays in its own time while grin carries
on. GRIN.INI's `chime=0`, from Control Panel's Sound (No chime), leaves it
out; Sound's Test plays it.

## 4. Milestones as built

| | built | branch, date | what (grin-plan.md 5 has each in full) |
|---|---|---|---|
| G0 | C4DOS services and heap generations | `grin-g0`, 2026-10-24 | re-entrant RUN, API v4 slots 16-27, HEAP.SYS; Doom five times at A> with 0 bytes of growth |
| G1 | the window kit | `grin-g1`, 2026-10-24 | `src/winkit` out of the C4IX desktop, the class table, the Windows 3.1 look, run-time screen size |
| G2 | grin's shell | `grin-g2`, 2026-10-24 | the DOS disk booting into grin, the 3.1 look, icons, the Task List, Control Panel, GRIN.INI, Exit grin |
| G3 | applications | `grin-g3`, 2026-10-25 | the kit's table in slot 25, `import.c`, load/start/unload under catch, Clock and Calculator |
| G4 | persistence and the shell applications | `grin-g4`, 2026-10-25 | `HOME=B:` (API v5), a store per system, Program Manager from PROGMAN.INI, File Manager, Notepad |
| G5 | DOS programs | `grin-g5`, 2026-10-26 | full-screen launch, REDIR.SYS and the console tap (API v6), DOS Prompt windows |
| G6 | games in a window | `grin-g6`, 2026-10-27 | `fwin.c`, the games' bare layers, `image()` (API v8), sizes, pointer lock |
| G7 | finishing | `grin-g7`, 2026-10-28 | MDI groups with Cascade and Tile; the clipboard in Notepad, the fields, Calculator and the DOS Prompt; the startup chime and Control Panel's Sound; the screen saver checked; this document |

### G7, as built

- **MDI group windows** (`progman.c`, section 3.7). The plan's "MDI group
  windows in Program Manager, with Cascade and Tile".
- **The clipboard across applications** (3.8). Checked first: the text was
  already grin's memory (3.2), so nothing there changed. What was missing
  was any copying at all: no application called `wk_clip_set` or
  `wk_clip_get`. Notepad gained a selection and an Edit menu; `ui.c`'s
  fields, Calculator and the DOS Prompt gained copy and paste.
- **The screen saver**: built in G2 (3.9). G7 checks it headless and lets
  grin say its own line in Marquee.
- **The startup chime** (3.10), and Control Panel's Sound item.
- **Tests**: test-grin.mjs's G7 checks (section 5), and `web-grin-g7.mjs`.
- **The gate**, on helios: `make test-grin` OK (225 checks, 51 of them
  G7's), `test-libjs` OK but for one test-desktop check, "the picture is
  stretched to it again", which failed while the browser check ran beside
  it and passed when test-desktop ran alone (187 OK, nothing failed);
  `test-c4dos` OK, `test-c4dos-heap` OK (Doom five times, 0.000% growth),
  `test-c4ix-rebuild` OK (the kit built inside C4IX too),
  and `test-wolf3d`, `test-doom`, `test-tyrian`, `test-rott` OK with their
  timedemo checksums unchanged (d7358845, 01914fe7, 389b26f7, 3b083b33).
- **Browser** (Chrome 151 over CDP on 9222, the page served from the
  worktree on 8494, `web-grin-g7.mjs`, all OK, no page errors): the chime
  counted on the page's sound card at boot, the groups restored, tiled,
  cascaded, Main maximised by a double click on its title and back;
  12*12= selected with Shift+Home in Notepad, Ctrl+C, Notepad ended,
  Ctrl+V in Calculator showing 144; a DOS Prompt's screen copied and
  pasted into Notepad with Shift+Ins; Marquee's Test; Sound's Test.
  Screenshots in the session notes' `grin-g7-shots/`.

Deviations and findings:

- **The kit's table is version 3**: the clipboard's keys, shared by the
  kit's classes that applications link (Notepad, Calculator), are kit
  functions now (`ui_key_*`), added at the end of `kitfns.h`.
- **A maximised group keeps its own title bar**, with the restore button;
  3.1 merged it into Program Manager's menu bar, which a window's class
  cannot draw into here. Program Manager's title names it as 3.1's did.
- **A group's control-menu box has no menu**: a double click minimises the
  group (3.1's Close did that to a group), a click only activates it.
- **No new groups**: File > New makes items, as in G4.
- **Calculator has no Edit menu**: a menu bar would make its fixed window
  taller and move its keys; Ctrl+C and Ctrl+V do it.
- **The DOS Prompt window has a menu bar now** (Edit), 20 px taller.
- **Control Panel's Sound** is new (3.1 had one, for its system sounds):
  the chime on or off, and a Test.
- The screen saver's wait is in the machine's milliseconds (`__time`), so
  headless it is checked through Test, not by waiting a minute.

## 5. Tests

`make test-grin` boots the page's DOS disk headless with a display, a
drive B: kept as the page keeps a store, and (for G7) a sound card, and
drives grin by clicks and keys, finding things by the text drawn. Its
checks, by milestone: G2's shell (the Task List, icons, Control Panel,
800x600, Exit grin), G3's applications (two Clocks, Calculator's
`12*12=`, EXITKEY's exit, the heap to the byte), G4's persistence and
shell applications (PROGMAN.INI, Notepad's NOTE.TXT through the file box,
File Manager, a second boot), G5's redirection at A> and DOS Prompts,
G6's games in a window, and G7's:

- the chime: four key-ons on OPL channels 0 to 3, 150 ms apart on the
  card's stream, none written before grin's first frame; Sound's Test
  plays it; No chime is kept in GRIN.INI, and the next boot plays none;
- the groups: Window > Cascade puts the three 22 px apart, Tile side by
  side with none over another; the Window menu lists them numbered; Games
  minimised to an icon (and minimised in PROGMAN.INI), restored by a
  double click with its items; Main maximised (Program Manager's title
  says `Program Manager - [Main]`, Main fills the client area) and
  restored; Accessories dragged by its title bar, its place in
  PROGMAN.INI and the same after another boot; Ctrl+F6 to the next group;
- the clipboard: Notepad's Shift+Home, Edit > Cut and Paste; Ctrl+A and
  Ctrl+C, and Notepad closed and unloaded (the heap back); Ctrl+V in
  Calculator then shows 144; its Ctrl+C, Calculator closed, Shift+Ins in
  the Run box's field; a line typed there copied with Ctrl+C; DOS Prompt's
  Edit > Paste typing it and Enter running it; its Edit > Copy, and Ctrl+V
  in Notepad giving the screen's text;
- the screen saver: Desktop's Test runs Marquee with grin's line, a key
  ends it and everything is drawn again.

## 6. Decisions

The user's, 2026-10-24 (grin-plan.md 8): the Windows 3.1 look; 640x480
and 800x600; one body of GUI code shared with the C4IX desktop; the heap,
C4DOS and libc changes; booting into grin on a disk of the page's own;
persistence for C4DOS and the bare games, a store per system; redirection
opt-in by `DEVICE=REDIR.SYS`; the command `GRIN`; the device lines
`HEAP.SYS`, `REDIR.SYS` (with `TAP`), `DRIVES.SYS HOME=B:`; the 3.1 look
offered in the C4IX desktop too. Agreed with the grin planning session:
G8 and G9 below.

## 7. Risks that stay

- **No protection**: an application or game that scribbles on memory takes
  grin down, as one could take down Windows 3.0.
- **A c4cc program in a DOS Prompt freezes grin** until it ends (standard
  mode's behaviour).
- **Text widths are guesses**: the host's fonts cannot be measured from
  the guest; layouts are generous.
- **Keyboard focus is the browser's**: Ctrl+Esc does not reach the page on
  Windows (the desktop's double click opens the Task List).

## 8. What is left (not planned as a milestone)

- New program groups (File > New makes items only), and deleting groups.
- 3.1's merged title for a maximised group, and a control menu on each
  group's box.
- Asking "full screen or window" the first time a DOS program runs that no
  Program Manager item names (grin-plan.md 3.6's plan; full screen now).
- An Edit menu in Calculator; Notepad's Undo, Find, word wrap.
- Marking a region of a DOS Prompt to copy (3.1's Edit > Mark), rather
  than the whole screen.
- A bitmap font of grin's own, printing, OLE and DDE (out of scope).

## 9. Planned, not built

### G8: a system monitor

What the C4IX desktop's System Monitor is to C4IX (docs/c4ix-desktop.md),
for grin, in the manner of the Windows 3.1 resource meters (and
Windows 95's System Monitor): a built-in window, `SYSMON`, in Program
Manager's Main group, and Ctrl+Shift+Esc.

- **Memory**: the heap's size, in use and free (`HEAP_USED`, and C4DOS's
  `MEM`), with a scrolling history; how much each image holds: grin, each
  application, the DOS program running. A generation's bytes are not
  readable today: the machine needs a register for them (`HEAP_GEN_USED`,
  next to `HEAP_RELEASE` at 0x1c8, read after writing a generation), or
  C4DOS a slot that asks.
- **Applications**: grin's loaded images (`apps.c`'s table): name,
  windows, generation, memory, and dead or alive; End Task from here as
  from the Task List.
- **The DOS program**, when one runs: its name, how it runs (full screen,
  window, DOS Prompt), and for a game in a window its frames per second.
- **The machine**: cycles per second against the clock it was given
  (`__c4_info`, the page's `mhz`), the share of grin's loop spent napping
  (an idle measure, as C4IX's idle task gives it), and the sound card's
  stream ahead.
- **Tests**: test-grin opens it, checks the memory figure against
  `vm.heap.used`, starts a Clock and sees it listed with its bytes, and
  closes it.

### G9: two games at once

Windows 3.x ran DOS programs two ways. **Standard mode**, which grin is
today: one DOS program at a time, which has the machine while it runs (a
DOS program in a window here keeps grin's applications alive only because
its polls run grin's loop). **386 Enhanced mode**: each DOS program in a
virtual machine of its own, several in windows at once, the processor
time-sliced between them and Windows, and each program's PIF saying
whether it runs in the background and how much of the time it gets. G9 is
that mode for grin's games: two games, each in its window, both playing.

What stands in the way, and how G9 would meet it:

- **One stack.** A DOS program runs on the stack DOS and grin share, and
  only comes back to grin at its polls; a second game started from the
  first one's poll would run on top of it, and the first could not go on
  until the second ended. So each game needs a **stack of its own**: a
  C4DOS slot that starts a program on a new stack (a *task*), and a switch
  between stacks at the polls -- `fwin_poll` (and a DOS Prompt program's
  `conread`) becomes the place one game yields to the next. That is
  cooperative, at the polls, which every game makes many times a frame,
  so it time-slices well enough; a preemptive switch would need the
  machine's timer interrupt, which C4DOS (no traps) does not take.
- **DOS's per-program state.** The run frames (`g_run_bp`, `g_in_trans`),
  the image DOS counts as running (slot 35), the current heap generation,
  the console's sink and the open files are global now; each must be
  saved and restored with the stack switch, as a VM's state was.
- **The display** is already shared: each game blits into its own window.
- **The sound card** has one OPL and eight sample channels. Two games
  writing the OPL would mix their music into noise: the game whose window
  has the focus gets the OPL and the speaker; samples could be shared by
  giving each game four channels (`snd.h`'s channel numbers mapped), or
  the background game muted, as an enhanced-mode PIF's "Exclusive" did.
- **Memory**: Doom and ROTT take about 15 and 17 MB, so two fit in the
  page's 64 MB; the heap generations already keep each game's apart and
  free it when it ends.
- **The rule "one DOS program at a time"** becomes "one at a time full
  screen, and any that play in a window"; Program Manager's item gains
  3.1's 386 Enhanced settings: Background (keeps running when its window
  is not in front) and a share of the time.
- **Tests**: Doom's timedemo and Wolfenstein 3D's both in windows, each
  making frames while the other does, and Doom's checksum the same as
  alone; then both closed, and the heap where it was.

Of the two, G9 is the bigger: C4DOS's run frames and the stack switch
are the risk, as the run frames were in G0.
