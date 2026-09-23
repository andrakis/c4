# libjs: c4m in JavaScript, in a Web Worker, with a display the guest draws into

Status tracker and design for the new `libjs/`. Tick a box only when the gate
under it has passed, and write down the evidence next to it.

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
  framebuffer mode is reserved for later.
- **First demo**: a standalone `.c4r`, then a C4IX program run from the shell
  with the terminal still live.

## Ground rules

- `src/c4bb/sim/*` and `src/c4bb/hw/*` are vendored verbatim into Homeward.
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
| `libjs/bus.js` | Addresses below 0x1000: 0x100-0x1FF go to c4bb's `Devices`, 0x400-0x43F go to `GuiDevice`. |
| `libjs/heap.js` | The host-side allocator behind MALC/FREE/RALC, with a size side table. |
| `libjs/printf.js` | A pure PRTF formatter. |
| `libjs/boot.js` | `parseC4r`/`loadImage` (reused), argv, and a c4mp-style trampoline. |
| `libjs/gui-device.js` | The display registers and the two rings, host side. |
| `libjs/keys.js` | Cooked and raw keyboard handling, copied from `src/c4bb/web/app.js`. |
| `libjs/worker.js` | The Worker: arena, devices, machine, run loop, messages. |
| `libjs/page.js` | The main-thread API, `C4M.boot(...)`. |
| `libjs/display.js` | Draws command frames on the canvas and turns DOM events into event frames. |
| `libjs/cli.js` | The Node runner, with c4bb's flags. |
| `libjs/serve.mjs` | A static server with optional COOP/COEP headers. |
| `libjs/web/` | The page: terminal left, canvas right. |
| `libjs/guest/gui.h`, `gui-demo.c` | The guest C API and the standalone demo. |
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
  4. the display raises HIRQ(3) (later);
  5. the cycle interval raises HIRQ(0), except when the instruction at `pc` is TLEV;
  6. otherwise a pending signal is delivered, unless the kernel has opted
     into `TRAP_RESTORES_INTERVAL` and the interval is zero.
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
  - STRC prints nothing, as on c4bb: a stack trace needs symbols.
  - INFO includes `C4I_C4MJS` (0x8) and never `C4I_C4` (0x1). C4IV does
    nothing.
- **The heap** keeps no headers in guest memory. Sizes live in a `Map`,
  small blocks come from size classes, large ones from a first-fit list with
  coalescing. RALC follows `c4m.c:1381-1398`.
- **printf** takes its argument count from the operand of the ADJ that
  follows it; more than 7 arguments prints "Too many arguments to printf!" and
  exits -1. The `l`, `ll`, `h` and `z` modifiers are ignored: one word per
  argument.
- **Boot** writes the ROM, loads the image at `MEM_BASE`, places argv, then
  assembles a trampoline after the image: constructors, `main(argc, argv)`,
  destructors, EXIT.

Known deviations from native c4m: division by zero gives 0 instead of SIGFPE;
the seven fused opcodes c4m lacks are implemented; FREE of an unknown pointer
is ignored.

## Worker protocol

The baseline is postMessage only, so python's `http.server` is enough.
SharedArrayBuffer is an optimisation for later, behind `crossOriginIsolated`.

- **Page to worker**: `boot`, `key`, `input`, `signal`, `eof`, `gui`,
  `pause`, `resume`, `reset`.
- **Worker to page**: `stdout`, `echo`, `kbdmode`, `display`, `gui`,
  `status`, `exit`, `error`, `log`.
- **The run loop** runs a slice (about 1M instructions, capped near 8 ms),
  flushes output, then:
  - on `sleep`, waits the requested time;
  - on `input`, polls every millisecond when a timer is armed, otherwise parks
    until a message arrives;
  - on `budget`, paces against simulated time.

## The display device

It sits at **0x0400-0x043F**. Guests check `__c4_info() & C4I_GUI` (0x4000)
before touching it.

| Address | Register | Access | Meaning |
|---|---|---|---|
| 0x400 | CAPS | r | bit0 command ring, bit1 events, bit2 framebuffer, bit3 text |
| 0x404 | W | r/w | width |
| 0x408 | H | r/w | height |
| 0x40C | RINGLEN | w | region length in bytes; write before RING |
| 0x410 | RING | w | region base; 0 detaches |
| 0x414 | BELL | w | 1 = flush, 2 = present. Read: events pending |
| 0x418 | EVMASK | r/w | 1 move, 2 buttons, 4 keys, 8 resize/focus, 16 wheel |
| 0x41C | IRQ | r/w | 0 = poll; 1 = HIRQ(3) when an event arrives (later) |
| 0x420 | MOUSE | r | (x << 16) \| y |
| 0x424 | BUTTONS | r | button bitmask |
| 0x428 | TICKS | r | host milliseconds |
| 0x42C | DROPPED | r | events lost to a full ring |
| 0x430 | FB | r/w | framebuffer base, 0x00RRGGBB pixels in guest memory |
| 0x434 | FBPITCH | r/w | bytes per row (0 = width * 4) |
| 0x438 | FBFLIP | w | (w << 16) \| h: copy, scale to the display, present |

The rings use exactly the c4bb mailbox format (`src/c4bb/sim/mbox.js`). The
first half of the region carries events to the guest, the second half
commands to the host.

- **Commands** (colours are `0x00RRGGBB`): 1 CLEAR, 2 RECT, 3 RECTO, 4 LINE,
  5 CIRCLE, 6 TEXT, 7 PIXEL, 8 PRESENT, 9 SIZE, 10 IMGDEF, 11 IMG, 12 CLIP,
  13 NOCLIP.
- **Events**: 1 MOVE, 2 DOWN, 3 UP, 4 KEYDOWN, 5 KEYUP, 6 RESIZE, 7 FOCUS,
  8 WHEEL.

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
      **Measured, fb-demo unpaced:** both paths deliver 63 guest frames/s and
      125 page draws/s, and the slowest page frame is 17 ms either way. The
      Worker is 100% busy computing pixels, so the guest is the bottleneck, not
      the display path. What the shared path buys: no message or allocation per
      batch, and stale framebuffer frames are never drawn. Its advantage should
      only show with a display load heavier than this demo.
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
- [ ] Media panel (c4bb's `store.js` and `drives.js`). Not done: C4IX writes
      to its RAM filesystem, so nothing yet needs writable drives kept in the
      browser. The worker already takes c4bb's drive list, so it is a page
      change when it is wanted.
