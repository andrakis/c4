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
  - A blocking READ returns -2: rewind pc by one word and stop with `input`.
  - USLP stops with `sleep`.
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
| 0x430-0x438 | FB, FBPITCH, FBFLIP | | reserved for the framebuffer |

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
- [ ] `c4m.js`, `bus.js`, `heap.js`, `printf.js`, `boot.js`, `cli.js`
- [ ] `tests/test-printf.mjs`, `tests/test-heap.mjs` green
- [ ] `tests/test-libjs.sh`: every image's output matches its oracle.
      `./c4m32 load-c4r.c --` is the oracle except for the images where c4m32
      misbehaves, which use 64-bit `./c4m`. Exit status pinned.
- [ ] Speed measured with `cli.js -s --fast` on a C4IX boot and recorded
      here (c4bb turbo: 18.5M inst/s; target at least 40M)
- [ ] Old `libjs/c4.js`, `libjs/simplest/` and `src/c4cc/asm-js.c`
      removed; README, `docs/internals.md` and `package.json` updated

### M1: both kernels boot in Node
- [ ] C4KE32: banner, `C4SH - The C4 SHell`, two commands, clean shutdown
- [ ] C4IX32: `C4IX booting`, protected mode and preemption on,
      `c4ix-ps.c4r` runs, vfsload loads (nearly) every entry
- [ ] `bb_pit` and `bb_mbox` behave as they do on c4bb
- [ ] Interactive `cli.js -i`: `ps`, `top`, `spin` then Ctrl-C

### M2: Worker and page
- [ ] `worker.js`, `page.js`, `keys.js`, `web/`, `serve.mjs`
- [ ] CDP gate: the C4IX shell prompt appears, typed `ps` lists tasks,
      Ctrl-C interrupts `spin`, no page errors, and the machine reports
      sleeping or blocked at an idle prompt

### M3: the display device and the standalone demo
- [ ] `gui-device.js`, `display.js`, `guest/gui.h`, `guest/gui-demo.c`
- [ ] `tests/test-gui.mjs`: a MOVE and a KEYDOWN go in; RECT/TEXT/PRESENT
      frames come out; the key is echoed
- [ ] Native `c4m32` prints `gui: not fitted` for the demo
- [ ] CDP gate: the canvas changes between presents and follows the mouse

### M4: `gui` from the C4IX shell
- [ ] `src/c4ix/user/gui.c` and a sleep syscall for frame pacing
- [ ] Wired into the Makefile, `build-images.sh` and `c4ix.vfs.txt`
- [ ] `make test-c4ix` still green
- [ ] CDP gate: `gui` draws, `ps` in the terminal lists it while it runs, `q`
      returns to the prompt

### M5: polish
- [ ] Performance pass
- [ ] SharedArrayBuffer display path behind `crossOriginIsolated`
- [ ] Display interrupts (HIRQ 3)
- [ ] Framebuffer mode
- [ ] STRC with symbol names from the `.c4r` symbol section
- [ ] Media panel (c4bb's `store.js` and `drives.js`)
