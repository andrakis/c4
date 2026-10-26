# libjs: c4m in JavaScript, in a Web Worker, with a display the guest draws into

Status tracker and design for the new `libjs/`. Tick a box only when the gate
under it has passed, and write down the evidence next to it.

Status, 2026-09-30: M0-M4 done, M5 done except the performance pass and the
media panel (both left open on purpose, below). The display has grown a pixel
framebuffer, display interrupts and a SharedArrayBuffer path since the
decisions below were taken. The biggest program drawn on it is the C4IX
desktop, tracked in [c4ix-desktop.md](c4ix-desktop.md); since 2026-10-20 the
page boots C4IX with `--desktop`, straight to the desktop with `top` on the
console (`libjs/tests/test-boot-desktop.mjs`).

## Why

The old `libjs/` ran **plain c4** only, in Node only. It got its programs from
a C-to-JS backend (`src/c4cc/asm-js.c`) instead of `.c4r` images, and only
knew opcodes 0-38. It could not run C4KE.

`src/c4bb/` already runs c4m in JavaScript, but as a microcoded breadboard: a
step engine, a compiled "turbo" engine (about 18.5M instructions/s) and a
guest firmware for malloc and printf, all on the page's main thread.

The new `libjs/` is a **direct interpreter** in the style of `c4m.c`. It runs
the 32-bit C4KE and C4IX images in a **Web Worker** and gives the page a
JavaScript API onto the running machine. The page follows jor1k's layout:
**a terminal on the left and a display on the right that guest code draws
into**, so a GUI can run inside the machine.

Out of scope for now: c4mp multiprocessing.

## Decisions (2026-09-23)

- **Core**: a new direct interpreter, a switch loop over an `Int32Array`.
  MALC/FREE/RALC/PRTF/STRC run host-side in JS as they do in native c4m. There
  is no guest firmware.
- **32-bit words.** libjs runs the images `make c4bb-images` builds
  (`src/c4bb/images/c4ke32.c4r`, `c4ix32.c4r`, `disk/`). The 64-bit `.c4r`
  files at the repo root do not load.
- **GUI channel**: a draw-command ring to a `<canvas>` now. A pixel
  framebuffer mode is reserved for later. *(Built since, in M5: FB/FBPITCH/
  FBFLIP.)*
- **First demo**: a standalone `.c4r`, then a C4IX program run from the shell
  with the terminal still live.

## Ground rules

- `src/c4bb/sim/*` and `src/c4bb/hw/*` are vendored verbatim into an embedder.
  libjs **imports them by relative path and never edits or moves them**.
  Anything that needs changing is copied into `libjs/` with a note saying
  where it came from.
- Serve from the repo root, as c4bb is: the worker imports
  `../src/c4bb/sim/...`. `file:` URLs do not work.
- Browser gates run over CDP against the real browsers (9224 = the 3060 Ti
  desktop, 9222 = the Chromebook), never headless.

## Layout

| File | Role |
|---|---|
| `libjs/c4m.js` | `Machine`: the interpreter. No DOM, no Node. |
| `libjs/bus.js` | Addresses below 0x1000: 0x100-0x1FF go to c4bb's `Devices`, 0x400-0x47F to `GuiDevice`, 0x480-0x4FF to `SoundDevice`. |
| `libjs/heap.js` | The host-side allocator behind MALC/FREE/RALC, with a size side table. |
| `libjs/printf.js` | A pure PRTF formatter. |
| `libjs/boot.js` | `parseC4r`/`loadImage` (reused), argv, and a c4mp-style trampoline. |
| `libjs/gui-device.js` | The display registers and the two rings, host side. |
| `libjs/snd-device.js` | The sound card's registers: what the guest writes, as timed events. |
| `libjs/snd-mix.js` | The events made into samples: the OPL, eight sample channels and the speaker, mixed to stereo. Shared by the worklet and `cli.js --wav`. |
| `libjs/snd-worklet.js` | The mixer in the page's AudioWorklet (on the page's own thread where there is none). |
| `libjs/opl.js`, `opl-tables.js` | The AdLib: DOSBox's dbopl, ported to JavaScript (GNU GPL v2 or later), with its tables as its C++ makes them. |
| `libjs/keys.js` | Cooked and raw keyboard handling, copied from `src/c4bb/web/app.js`. |
| `libjs/worker.js` | The Worker: arena, devices, machine, run loop, messages. |
| `libjs/page.js` | The main-thread API, `C4M.boot(...)`. |
| `libjs/display.js` | Draws command frames on the canvas and turns DOM events into event frames. |
| `libjs/cli.js` | The Node runner, with c4bb's flags. |
| `libjs/shared.js` | The display's SharedArrayBuffer path: a command ring and a framebuffer triple buffer, used when the page is cross-origin isolated. |
| `libjs/serve.mjs` | A static server with optional COOP/COEP headers. |
| `libjs/web/` | The page: terminal left, canvas right. |
| `libjs/guest/gui.h`, `gui-demo.c` | The guest C API and the standalone demo. |
| `libjs/guest/fb-demo.c` | The framebuffer demo: 320x240, every pixel computed by the guest, events by interrupt. |
| `libjs/guest/snd.h`, `snd.c`, `snd-demo.c` | The sound card's guest API, and a tune on the bare machine. |
| `libjs/tests/` | The differential suite, unit tests, the GUI test and the CDP gate. |
| `src/c4ix/user/gui.c` | `c4ix-gui.c4r`. |

Reused from `src/c4bb/sim/`: `arena.js`, `devices.js`, `loader.js`
(`parseC4r`, `loadImage` and `initRom` only), `mbox.js`
(`HostMailbox`), and `web/panels.js` (`Terminal`).

## The interpreter

- **Registers** `pc sp bp a` are byte addresses. `cycle`, `mode`,
  `trapHandler`, `cycleInterval`, `cycleHandler` and `trapRestoresInterval`
  keep the property names c4bb's `Devices` reads.
- **Boundary checks** keep c4bb's order (`machine.js:189-242`):
  1. every 4096 cycles, the TLEV wedge check;
  2. the PIT, on the wall clock, raises HIRQ(1);
  3. the mailbox raises HIRQ(2);
  4. the display raises HIRQ(3);
  5. the cycle interval raises HIRQ(0), except when the instruction at `pc` is TLEV;
  6. otherwise a pending signal is delivered, unless the kernel has opted
     into `TRAP_RESTORES_INTERVAL` and the interval is zero.

  The PIT and the mailbox are also held from a trap that enters a handler
  to the TLEV that ends it (`irqHold`, counted for nested traps), as on
  c4bb (`machine.js irqHold`, docs/c4bb-design.md). A tick due meanwhile
  comes after the TLEV, its deadline kept. Without the hold, a tick could
  land between C4KE's re-arm of the PIT and its TLEV, after it had
  switched tasks, and leave the next task looping on its TLEV.
  `bb_pit.c`'s third pass checks it. The display IRQ and IPIs are not
  held: C4IX waits for a kernel lock in CWAI inside its trap handler.
- **The trap frame** is `c4m.c:1445-1516` byte for byte. TLEV is
  `c4m.c:2209-2236`. A trap with no handler is silent and continues.
- **Opcodes**: all 89 names.
  - MUL uses `Math.imul`. DIV and MOD by zero give 0.
  - All ten fused opcodes are implemented with c4mp's semantics, because the
    disk's `c4sp.c4r` is built with `-mfuse`.
  - A blocking READ returns -2 (c4bb's convention): pc goes back one word
    so the READ runs again. Running it again changes nothing, so instead
    of spinning the clock jumps to the next cycle interrupt, the state a
    spin would reach. With no interrupt armed the run stops with `input`
    and the host parks until a key arrives.
  - USLP stops with `sleep`, so the host can pace.
  - STRC prints a stack trace, naming each frame from the image's symbol
    section (`boot.js` parseSymbols; below).
  - INFO includes `C4I_C4MJS` (0x8) and never `C4I_C4` (0x1). C4IV does
    nothing.
- **The ROM words are read-only to a guest**: 0x00-0x1F (the null guard,
  TLEV at 0x10, the constructor-return word, CPUH at 0x18) and the
  opcode-name ROM at 0x200. A store there from SI, SC, MSET, MCPY or a
  READ is dropped, `romStores` counts it, and the first one is named on
  the console with its value and pc. Every trap returns through TLEV, so
  one wild store there used to end every task on the machine. c4bb's
  firmware latches at 0x20-0x3F stay RAM. Stack pushes and STL are not
  checked. `tests/test-rom.mjs`.
- **A jump into the null guard stops the machine.** Its four words hold
  `NULL_GUARD_OP` (`c4m.js`), which only an instruction fetch sees: a
  data read below 0x10 goes through the bus, which answers 0 as before,
  and the marker's low byte is 0, so the string at 0 is still empty.
  Fetched, it reaches `sys()` as an unknown opcode, and `nullJump`
  names the jump, the registers and, when the top of the stack is a
  return address, a stack trace from the caller, then halts with -102.
  The usual way there is a call through a word nothing linked -- a C4IX
  program run without its libraries -- which before ran the ROM round
  and round, printing nothing. It costs no instruction anything: a check
  at every fetch cost an eighth of the speed, one at every jump that
  takes pc from data about a twentieth. A jump elsewhere below 0x1000
  is not caught.
- **The heap** keeps no headers in guest memory. Sizes live in a `Map`,
  small blocks come from size classes, large ones from a first-fit list with
  coalescing, and once the bump pointer is at the end a small block is cut
  from a free range too. RALC follows `c4m.c:1381-1398`.
- **printf** takes its argument count from the operand of the ADJ that
  follows it; more than 7 arguments prints "Too many arguments to printf!" and
  exits -1. The `l`, `ll`, `h` and `z` modifiers are ignored: one word per
  argument.
- **Boot** writes the ROM, loads the image at `MEM_BASE`, places argv, then
  assembles a trampoline after the image: constructors, `main(argc, argv)`,
  destructors, EXIT.

- **Speed** is V8's to give, and `run()` is written for it (2026-10-24).
  - The dispatch is never a loop. OPCD used to go round the switch again
    with the opcode from the stack. C4IX's syscalls are all OPCDs, and
    that one live inner loop made V8 compile every case worse. Any C4IX
    program ran 1.7 times slower for it, and so did a bare one run after
    C4IX in the same process. Now a syscall or custom opcode falls through
    to `default`, and one of the switch's own goes to `opcd()`.
  - The boundary checks are `boundary()`, out of the loop. They are due
    only at the next tick or PIT look (`nextChk`), and one compare per
    instruction (`cycle > lim`) covers both them and the slice's end.
    Anything that can make a check pending sooner runs through a path
    that resets `lim`.
  - ROTT's demo 4 (3.5e9 cycles) under C4IX in Node went from 27 s to
    17.5 s; the bare game takes 14 s. In Chrome on the desktop at 400 MHz,
    from 73-87M inst/s to 133M; the bare game runs at 156M.
  - `--trace-deopt` and the experiment of running the bare game after
    C4IX in one process found it. Perf on helios has only cpu-clock (a
    Hyper-V guest has no PMU), and it put the time on the loop head,
    which was not where it went.

Known deviations from native c4m: division by zero gives 0 instead of SIGFPE;
the seven fused opcodes c4m lacks are implemented; FREE of an unknown pointer
is ignored; stores to the ROM words are dropped.

## Worker protocol

The baseline is postMessage only, so python's `http.server` is enough.
When the page is `crossOriginIsolated` (`serve.mjs --isolate`), the display
uses SharedArrayBuffer instead (`shared.js`, M5); `?shared=0` forces the
message path.

- **Page to worker**: `boot`, `key`, `input`, `signal`, `eof`, `gui`,
  `pause`, `resume`, `reset`, `peek`, `poke`.
- **Worker to page**: `out`, `kbd`, `display`, `gui`, `snd` (the sound
  card's events, when `boot` asked for one with `snd: true`), `persist`
  (a file closed on drive 1), `status`, `exit`, `error`, `reply` (the
  answer to a `peek` or `poke`).
- **Drive 1.** `C4M.boot({ persist: NAME })` gives the machine a writable
  host drive 1, whose files the page keeps in IndexedDB under NAME (c4bb's
  `src/c4bb/web/store.js`, as one medium) and passes back at the next
  boot. C4IX keeps its changed files there (docs/c4ix-design.md,
  "Persistence"). The C4IX page uses `c4ix`. `?persist=0` gives no drive,
  and `?persist=reset` forgets what was kept first. The page's **Reset
  Files** button (`vm.resetFiles()`) forgets it and boots again: IndexedDB
  is emptied, then the worker empties drive 1 and boots in one step
  (`reset-files`, answered by `files-reset`), and a file the old machine
  sent in between is dropped rather than kept.
- **The run loop** runs a slice (about 1M instructions, capped near 8 ms),
  flushes output, then:
  - on `sleep`, waits the requested time;
  - on `input`, polls every millisecond when a timer is armed, otherwise parks
    until a message arrives;
  - on `budget`, paces against simulated time.

## The display device

It sits at **0x0400-0x047F**. Guests check `__c4_info() & C4I_GUI` (0x4000)
before touching it.

| Address | Register | Access | Meaning |
|---|---|---|---|
| 0x400 | CAPS | r | bit0 command ring, bit1 events, bit2 framebuffer, bit3 text |
| 0x404 | W | r/w | width |
| 0x408 | H | r/w | height |
| 0x40C | RINGLEN | w | region length in bytes; write before RING |
| 0x410 | RING | w | region base; 0 detaches |
| 0x414 | BELL | w | 1 = flush, 2 = present. Read: events pending |
| 0x418 | EVMASK | r/w | 1 move, 2 buttons, 4 keys, 8 resize/focus, 16 wheel, 32 relative motion |
| 0x41C | IRQ | r/w | 0 = poll; 1 = HIRQ(3) when an event arrives |
| 0x420 | MOUSE | r | (x << 16) \| y |
| 0x424 | BUTTONS | r | button bitmask |
| 0x428 | TICKS | r | host milliseconds |
| 0x42C | DROPPED | r | events lost to a full ring |
| 0x430 | FB | r/w | framebuffer base, 0x00RRGGBB pixels in guest memory |
| 0x434 | FBPITCH | r/w | bytes per row (0 = width * 4) |
| 0x438 | FBFLIP | w | (w << 16) \| h: copy, scale to the display, present |
| 0x43C | FBMODE | r/w | 0 row-major, 1 column-major |
| 0x440 | CLOCK | r | host local time, seconds since midnight |

The rings use exactly the c4bb mailbox format (`src/c4bb/sim/mbox.js`). The
first half of the region carries events to the guest, the second half
commands to the host.

- **Commands** (colours are `0x00RRGGBB`): 1 CLEAR, 2 RECT, 3 RECTO, 4 LINE,
  5 CIRCLE, 6 TEXT, 7 PIXEL, 8 PRESENT, 9 SIZE, 10 IMGDEF, 11 IMG, 12 CLIP,
  13 NOCLIP, 14 TEXT2, 15 CURSOR (`GUI_PTR_*`, the pointer's shape).
- **Events**: 1 MOVE, 2 DOWN, 3 UP, 4 KEYDOWN, 5 KEYUP, 6 RESIZE, 7 FOCUS,
  8 WHEEL, 9 REL.
- **The mouse locked to the display.** The pointer shape `GUI_PTR_LOCK`
  (8) asks for it. The page then hides the pointer, and the next click on
  the display takes the browser's Pointer Lock, which only a click may
  start; Esc lets it go, as browsers require. While it is locked:
  - the pointer moves by the mouse's motion and stays inside the
    display, so DOWN, UP and MOVE carry a position as before;
  - a guest whose mask has 32 (`GUI_M_REL`) gets REL (dx, dy, buttons)
    instead of MOVE: the motion itself, which the display's edge does not
    stop.

  Any other shape releases it. Tyrian asks for it, so the mouse that
  steers its ship cannot wander off the display.

## The sound card

It sits at **0x0480-0x04FF**, beside the display, and guests check
`__c4_info() & C4I_SND` (0x8000) before touching it. It is an AdLib (an
OPL2, with the OPL3's second bank), eight channels of unsigned 8-bit
samples, and the PC speaker: what a DOS game expects to find.

| Address | Register | Access | Meaning |
|---|---|---|---|
| 0x480 | CAPS | r | bit0 OPL, bit1 samples, bit2 speaker, bit3 OPL3 |
| 0x484 | OPL | w | (reg << 8) \| val; reg 0x100 and up is the OPL3's second bank |
| 0x488 | WAIT | w | the stream moves n CLOCK units on |
| 0x48C | AHEAD | r | microseconds the stream is ahead of now |
| 0x490 | CLOCK | r/w | WAIT's units per second (default 1000000) |
| 0x494-0x4A0 | PCM_ADDR, LEN, RATE, VOL | r/w | the next sample: where, how long, at what rate; VOL is left \| (right << 8) |
| 0x4A4 | PCM_PLAY | w | play it on that channel, now; the bytes are copied now |
| 0x4A8 | PCM_STOP | w | that channel, or -1 for all |
| 0x4AC | PCM_SETVOL | w | PCM_VOL becomes that channel's volume, as it plays |
| 0x4B0 | PCM_BUSY | r | a bit for each channel still playing |
| 0x4B4 | SPK | w | the PIT divisor of the speaker's tone; 0 is quiet |
| 0x4B8 | RESET | w | everything quiet, the OPL zeroed |

- **The machine makes no samples.** Each write becomes an event with a
  time on it, and whoever listens mixes them (`snd-mix.js`). An
  interpreter that made 44,100 FM samples a second would have little
  time left for the game.
- **The stream.** An OPL write is stamped with the stream's time, which
  WAIT moves on. A player writes the way a sound card's interrupt would:
  some registers, wait a tick, more registers. Each write then plays when
  it says, however unevenly the program got round to it. The program keeps
  the stream ahead of now by reading AHEAD and making more while it is
  short, and a stream that falls behind is brought up to now. Samples,
  the speaker and the rest are stamped now.
- **Two clocks.** Now is the machine's own clock in `cli.js`, so a session
  makes the same file every time. In the Worker it is the page's real
  clock: the AudioContext plays in real time whether or not the machine
  keeps up, and the stream has to stay ahead of what the listener hears.
- **The page.** Each slice's events go to the page (`snd`) and on to an
  AudioWorklet (`snd-worklet.js`), which plays them 60 ms after the
  Worker's clock said they happened. When the two clocks drift more than
  50 ms apart (a hidden tab, a stalled machine), the worklet starts again
  from the next batch rather than bunching up what was late. A page that
  is not a secure context (plain http from another host, as the LAN
  address is) has no AudioWorklet, and there the same mixer runs on the
  page's thread in a ScriptProcessorNode, 120 ms behind. A browser
  keeps an AudioContext silent until the page is used, so the first key or
  click starts it. Until then the OPL's registers are kept and sent with
  every note off, so music already playing comes in with its instruments.
  `?snd=0` leaves the card out.
- **`cli.js --wav FILE`** fits the card and writes everything it played
  to FILE at 44.1 kHz. Without `--wav` there is no card, so every test that
  runs a game runs it silent, as before.
- **The OPL** is `opl.js`, DOSBox's dbopl (the emulator Wolf4SDL uses),
  ported line by line. The tables dbopl makes with `sin` and `pow` are
  `opl-tables.js`, as its own C++ makes them, so no browser's `Math`
  changes a bit. `test-libjs` builds the C++ (`libjs/tests/src/oplref`),
  renders random register streams through both, and requires the same
  samples. It renders about 20 times faster than real time.

## Processors

`cli.js --cpus N` (and the page's `?cpus=N`) gives the machine N
processors: c4mp's instruction set (`src/c4mp/README.md`), simulated
(`smp.js`, docs/c4ix-smp.md S1).

- **Each CPU is a `Machine`**, with its own registers, cycle count, trap
  handler, interval and mode, over the same arena, devices and heap. CPU
  0's `run` hands over to `Smp.run`, which runs each runnable CPU for a
  quantum (`-q`, default 1,000) in CPU order, as c4mp's `c4_smp_run`
  does. The interleaving is c4mp's quantum for quantum: c4mp's `smp0` and
  `smp1` guests print its pinned transcripts, cycle counts included.
- **Opcodes 66-75:** `CPUI`, `CPUN`, `CPUS`, `CPUH`, `CAS`, `XCHG`,
  `FADD`, `CWAI`, `CWAK`, `IPI`. They are unguarded in protected mode, as
  in c4mp. An instruction cannot be split, so the atomics are plain
  read-modify-writes. `IPI` arrives as `HARD_IRQ` with c4mp's sub-code 1,
  after the tick and regardless of the preemption mask; the PIT's
  sub-code is 1 too, and no SMP guest runs the PIT.
- **Halting:** `EXIT` on CPU 0 ends the machine. On another CPU, `EXIT`
  or `CPUH` halts that processor only. Nothing runnable with some CPU
  parked is a deadlock: each CPU is named with the word it waits on, and
  the machine exits 255.
- **Time** is the machine's: the round robin advances it by the most any
  CPU ran in each round (`devices.js` `timeSource`), and `TIME` and the
  pacing go by it. `C4CY` is each CPU's own count.
- **`USLP` sleeps one CPU** while another can run: it is `sleep` until
  the machine's time reaches the end of its nap, or an `IPI` comes. Only
  when no CPU can run does the machine nap, to the first sleeper's end.
  (C4IX's CPU 0 naps in its idle task while tasks run elsewhere.)
- **The CPU-local window**, 0x800-0x8FF: 64 words of each CPU's own,
  through `ld32`/`st32`. It is for what must differ by CPU at one
  address, such as a shared library's data slot (docs/c4ix-smp.md, 3.4).
- **`C4I_SMP` (0x100)** is in `INFO` only when the machine was given
  CPUs, even one. Without `--cpus` nothing changes.

`--parallel` (`?parallel`) puts each CPU on a thread of its own instead
(`par.js`, `ap.js`, docs/c4ix-smp.md S2). The engine needs a
`SharedArrayBuffer`; without one, the simulated engine runs.
- **CPU 0** stays where the machine always ran: the page's worker, or
  cli.js's main thread. It keeps the devices and the console.
- **The heap is every CPU's** (`sheap.js`): heap.js's rules, with its
  books in a SharedArrayBuffer of their own and a spinlock, so `MALC`,
  `FREE` and `RALC` run on the CPU that executes them (docs/c4ix-smp.md,
  after S4).
- **Every other CPU** is a worker running an `ApMachine` on the shared
  arena. What it cannot do itself, it asks CPU 0 through its request
  slot: the host syscalls but the heap's, and device registers. The sound
  card's writes that need no answer it posts instead, to a queue of its
  own that CPU 0 applies in order (par.js `POSTED`, docs/c4ix-smp.md). Everything else of
  memory it reads and writes as CPU 0 does, through arena.js: unaligned
  words, low memory, the arena's ends.
- **The atomics** are `Atomics`. `CWAI` blocks a worker in
  `Atomics.wait`. On CPU 0 it parks the loop, which waits blocked on the
  doorbell a few ms at a time (`parkSync`), answering each request as it
  comes, and takes the page's messages between. A `CWAK` for CPU 0's word
  rings the doorbell (`K.WOKE`).
  - It waited in `Atomics.waitAsync` before 2026-10-24, and each request
    then waited a trip through the event loop. CPU 0 parks on C4IX's
    kernel lock whenever another CPU holds it, and `top` on CPU 1 held it
    for every byte it printed, each a `PUTC` request. Doom on CPU 0
    stalled once a second, the page showing `parked` and 40M inst/s.
  - ROTT's demo 4 on two CPUs beside `top`, in Node: 25-143 s before,
    against 16 s alone.
  - Now it takes 17 s either way. C4IX also writes a task's console
    output a run at a time (`con_write_task`, one `PRTF` of up to 256
    bytes), so `top` asks 17k times where it asked 140k.
- **CPU 0's nap** (`USLP`) while other CPUs run is real time, ended early
  by an `IPI`. CPU 0 spends it blocked in `Atomics.wait` on its doorbell,
  a few ms at a time (`sleepSync`), and answers each request at once.
  Between turns the host takes its own events.
- **The guests' answers** are the same as on the simulated engine. The
  interleaving, and so the cycle counts, are the host's.

## Tracker

### Before M0
- [x] This document committed on `pm-investigation`.

### M0: interpreter and CLI, output identical to native
- [x] `c4m.js`, `bus.js`, `heap.js`, `printf.js`, `boot.js`, `cli.js`
- [x] `tests/test-printf.mjs` (22 formats, expectations from glibc in a
      `gcc -m32` build) and `tests/test-heap.mjs` green
- [x] `tests/test-libjs.sh`: every image's output matches its oracle.
      19 images against `./c4m32 load-c4r.c --` (including `cycles`,
      `raycast`, `bb_customop`, `bb_preempt`, `bb_pm`, `bb_mbox`); factorial,
      test_malloc, test_args and test_printf against 64-bit `./c4m`; exit
      status pinned. (2026-09-23)
- [x] Speed, `cli.js -s` (2026-09-23, Node 22, same host, same images):

      | Workload | libjs | c4bb turbo |
      |---|---|---|
      | C4IX boot, 20M cycles | 106M inst/s | 19.3M inst/s |
      | raycast, 200 frames | 190M inst/s | 19.7M inst/s |
      | mandel | 78M inst/s | 18.5M inst/s |

- [x] Old `libjs/c4.js`, `libjs/simplest/` and `src/c4cc/asm-js.c`
      removed; README, `docs/internals.md` and `package.json` updated;
      `make test-libjs` added

### M1: both kernels boot in Node
- [x] C4KE32: banner, `C4SH - The C4 SHell`, two commands, clean shutdown,
      106/106 filesystem entries, a task killed on a bad opcode leaves the
      shell alive (all in `test-libjs.sh`, 2026-09-23)
- [x] C4IX32: `C4IX booting`, protected mode and preemption on,
      `c4ix-ps.c4r` runs, vfsload loaded 44/44 (2026-09-23)
- [x] `bb_pit` (both passes) and `bb_mbox` behave as they do on c4bb
- [x] Interactive `cli.js -i` through a real pty (2026-09-23): C4IX `ps`,
      `spin` cancelled by Ctrl-C ("interrupt: cancelling task"), `top`, a
      command afterwards, `exit` with a clean shutdown; C4KE `ps`,
      `hello.c4r`, `\q` to "clean shutdown"

### M2: Worker and page
- [x] `worker.js`, `page.js`, `keys.js`, `web/`, `serve.mjs`, and `tests/cdp.mjs`
      (raw CDP, no Playwright)
- [x] CDP gate, `node libjs/tests/test-web.mjs`, green on the 3060 Ti
      (Chrome 156, 9224) through the code-server proxy, 2026-09-23: C4IX boots
      to its shell in the Worker, typed `ps` lists tasks, idle at the prompt
      reads `sleeping` at 0% CPU, Ctrl-C cancels `spin`, the shell answers
      afterwards; C4KE boots to `c4sh>` and runs `hello.c4r`; no page errors.
      Chrome delivers CDP key events only to the tab in front, so the gate
      brings its own tab forward.

### M3: the display device and the standalone demo
- [x] `gui-device.js`, `display.js`, `guest/gui.h`, `guest/gui-demo.c`;
      `gui-demo.c4r` built by `build-images.sh`
- [x] `tests/test-gui.mjs` (in `make test-libjs`): a move, a click and two
      keys go in; CLEAR/RECT/LINE/TEXT/PRESENT frames come out, the text
      follows the mouse to (300,200), the click and keys reach the program,
      q ends it, nothing dropped (2026-09-23)
- [x] Native `c4m32` and the display-less CLI both print `gui: not fitted`
      (pinned in `test-libjs.sh`)
- [x] CDP gate, `test-web.mjs --gui`, green on the 3060 Ti (2026-09-23): the
      canvas changes between presents, a real click and key on the canvas
      reach the program, q halts the machine with status 0, no page errors.
      Screenshot checked by eye.

### M4: `gui` from the C4IX shell
- [x] `src/c4ix/user/gui.c` (the demo's own source, with printf, malloc and
      the frame delay mapped onto libc4ix) and a sleep syscall:
      `SYS_SLEEP` (220, `SYS_TOP` moves to 221) parks the task on the clock
      the way C4KE's OP_USER_SLEEP already did; `umsleep(ms)` in libc4ix.
      `ps` and `top` now name that state `sleep` instead of `?`.
- [x] Wired into `C4IX_PROGS`, `build-images.sh` and `c4ix.vfs.txt`
      (vfsload now 47/47)
- [x] `make test-c4ix` and `make test-c4ix-c4ke` green; natively
      `c4ix-gui.c4r` prints `gui: not fitted`. **The x5 pin was re-pinned**:
      its ping/pong order was an interleaving set by preemption timing
      (ping 0, ping 1, pong 0, ...), which one more branch in the syscall
      dispatcher shifted. The new order is the strict alternation the
      demo's own comment promises; nothing else in the pin changed.
- [x] CDP gate, `test-web.mjs --gui`, green on the 3060 Ti (2026-09-23):
      `gui &` from the shell draws; `ps` typed in the terminal while it runs
      lists `c4ix-gui.c4r` as `sleep`; a click and a key on the canvas reach
      it; q ends it and the shell carries on. The machine sat at 2% CPU
      while it drew.

### M5: polish
- [ ] Performance pass. Not done, not needed yet: 106-190M inst/s in Node
      and 74M in the browser for the framebuffer demo, which is paced to the
      clock it claims. Revisit if a guest is CPU-bound in the browser.
      2026-10-24: the games are, under C4IX. Its 1.7x penalty is gone (the
      interpreter's Speed, above).
- [x] SharedArrayBuffer display path (`shared.js`, asked for 2026-09-23).
      When the page is cross-origin isolated (`make serve-libjs` and the gate
      send COOP/COEP; the code-server proxy passes them through), commands go
      through a shared ring the page reads once per animation frame, and
      framebuffer frames through a lock-free triple buffer, with only a marker
      in the ring. A batch the ring cannot take is kept and retried, never
      dropped; a hidden page keeps emptying the ring on a timer. `?shared=0`
      forces the message path. `tests/test-shared.mjs` covers ring order across
      wraps and a slow reader, the triple buffer's newest-frame and
      never-the-same-slot rules, and fb-demo end to end. CDP gate green on the
      3060 Ti, isolated, both paths.
      **Measured, fb-demo unpaced:** both paths equal (63 guest frames/s):
      the guest computing pixels is the bottleneck.
      **Measured, raycast `-G 640x480` unpaced** (the heavy load: each column
      is three memcpy calls out of a column-major framebuffer, FBMODE 1, so a
      frame is ~518k instructions and 1.2 MB of pixels):
      - Node, headless, 600 frames: shared **421 f/s**, messages 252 f/s
        (737 MB through the device queue on the message path, none shared).
      - Browser, 3060 Ti: shared 280-288 f/s, messages 295-324 f/s. The
        Worker's own costs are lower on the shared path (flip copies ~30 vs
        ~95 ms a second, hand-off ~1 vs ~80 ms), but its interpreter ran ~25%
        slower per second of running (148M vs ~180M cycles/s). Keeping the
        page's main thread half busy lifted the shared path to 335 f/s,
        ahead of messages at 325: an idle-looking renderer gets its Worker
        scheduled slower (on this Windows box; likely efficiency cores or
        power throttling), which the message path avoids only because its
        page is busy converting 300 frames a second. Not worked around.
      - The page coalesces on the shared path: each batch is drawn once from
        its last full-screen frame and presented once, so it converts about
        30 frames/s instead of every one the guest makes.
- [x] Display interrupts: `gui_irq(1)` raises HARD_IRQ(3) through the cycle
      handler when an event arrives. `fb-demo` takes its events that way;
      pinned in `test-gui.mjs` and the CDP gate (2026-09-23)
- [x] Framebuffer: FB/FBPITCH/FBFLIP at 0x430-0x438, `gui_fb`/`gui_flip` in
      gui.h. A flip copies the pixels into the same ordered stream as the
      commands, so text can be drawn over a frame; an unread frame with
      nothing after it is replaced rather than queued. `fb-demo.c4r`
      (320x240, every pixel computed by the guest): pixels checked exactly
      in `test-gui.mjs`; in the browser on the 3060 Ti at the page's 100 MHz
      for this demo, 74M inst/s, about 29 frames a second, 30% of a core
- [x] STRC names each frame from the image's symbol section
      (`boot.js` parseSymbols); pinned by `libjs-strc.c4r` in the suite
- [x] Sound card (2026-10-22): the AdLib, eight sample channels and the
      speaker at 0x480, as timed events mixed by an AudioWorklet
      ("The sound card"). `snd-demo.c4r` plays a tune; `cli.js --wav`
      writes a session's sound to a file; `opl.js` makes dbopl's samples
      bit for bit (`test-libjs`). Wolfenstein 3D plays its music, AdLib
      effects and digitized sounds through it
- [ ] Media panel (c4bb's `store.js` and `drives.js`). Not done: C4IX's
      filesystem lives in memory, so nothing yet needs writable drives kept
      in the browser. The worker already takes c4bb's drive list, so it is a page
      change when it is wanted.
