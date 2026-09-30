# libjs — c4m in JavaScript

A direct interpreter for the c4m virtual machine, written the way `c4m.c`'s
dispatch loop is: one `switch` over an `Int32Array`. It boots the 32-bit
C4KE and C4IX images in Node or in a browser Web Worker, with a terminal and
a display device that a guest program draws into.

It shares its memory map, device registers, `.c4r` loader, disk controller,
keyboard, clocks and mailbox with the c4bb breadboard computer
([../src/c4bb/](../src/c4bb/README.md)) by importing them. The difference is
the CPU: c4bb runs microcode and keeps malloc and printf in guest firmware;
libjs runs opcodes, and MALC/FREE/RALC, PRTF and STRC happen on the host
side, as they do in native c4m.

## Build

Nothing to build in libjs itself. The images it runs are the ones c4bb
builds:

    make c4bb-images

## Run

From the repo root:

    node libjs/cli.js [-m MB] [-c cycles] [-s] [-d dir] [-w dir] [-i] [-hz MHz] [--fast] [--mbox BYTES] program.c4r [args...]

| Flag | Meaning |
|---|---|
| `-m MB` | Arena size (default 32). |
| `-c cycles` | Cycle budget (default unlimited); running out exits with status 9. |
| `-s` | Print machine stats to stderr on exit. |
| `-d dir` | A read-only drive: every file under `dir`, by relative path. |
| `-w dir` | A writable drive: files the guest writes land in `dir`. |
| `-i` | Interactive: stdin streams to the keyboard. Without it, piped stdin is fed up front with end-of-file set. |
| `-hz MHz` | How fast the machine claims to be (default 20). |
| `--fast` | Interactive runs go as fast as the host can instead of pacing to the clock. |
| `--mbox N` | Fit a mailbox of N bytes (c4bb's `MBOX_*` registers). |

The flags are c4bb's `cli.js` flags, so a test can swap one runner for the
other. Examples:

    node libjs/cli.js src/c4bb/images/hello32.c4r
    node libjs/cli.js -i -d src/c4bb/images/disk src/c4bb/images/c4ix32.c4r
    printf 'ps\nexit\n' | node libjs/cli.js -d src/c4bb/images/disk src/c4bb/images/c4ix32.c4r

In a browser:

    make serve-libjs       # node libjs/serve.mjs 8472 --isolate

then open http://localhost:8472/libjs/web/. The server serves the repo root,
because the Worker imports `src/c4bb/sim/` and the page loads images from
`src/c4bb/images/`. `--isolate` adds the COOP/COEP headers, which make the
page cross-origin isolated so the display can use shared memory
(`shared.js`); without them it falls back to posting messages.
`LIBJS_PORT=NNNN` moves it.

URL parameters (from `web/app.js`):

| Parameter | Meaning |
|---|---|
| `?system=c4ix\|c4ke\|gui-demo\|fb-demo\|raycast` | Which machine to boot (default `c4ix`). The last three are bare programs on the display. |
| `?image=URL&disk=URL` | Any other image and disk. |
| `?mhz=N` | Paced speed (default 100). |
| `?fast` | Unpaced. |
| `?mem=MB` | Arena size (default 64). |
| `?shared=0` | Use the message path even when isolated. |

The running machine is `window.c4m` on the page, so the console or a test
can drive it (`c4m.write('ps\n')`).

## Test

Neither is in `make test`.

| Target | What it checks |
|---|---|
| `make test-libjs` | Unit tests (printf, heap, display, shared memory, desktop), then every corpus image byte for byte against native `c4m32` (or 64-bit `c4m` where `c4m32` cannot be trusted), the fused-opcode differential, STRC with symbol names, exit status, the PIT, and C4KE and C4IX booting with grep assertions. Builds `c4bb-images` first. |
| `make test-libjs-web` | The page in the user's own Chrome over raw CDP (`LIBJS_CDP`, else ports 9224 then 9222), then the display legs with `--gui`. It never launches a browser; if none answers it says so. |

## Layout

| File | What it is |
|---|---|
| `c4m.js` | `Machine`, the interpreter: registers, the dispatch loop, traps, syscalls. No DOM, no Node. |
| `bus.js` | Addresses below 0x1000: 0x100–0x1FF go to c4bb's `Devices`, 0x400 up to the display. |
| `heap.js` | The host-side allocator behind MALC/FREE/RALC, with a size side table as native c4m keeps. |
| `printf.js` | PRTF: glibc's format rules over 32-bit words. |
| `boot.js` | Builds the machine, loads an image with c4bb's loader, and starts `main` through a c4mp-style trampoline. |
| `gui-device.js` | The display registers and its two rings (c4bb's mailbox format), host side. |
| `shared.js` | The display's SharedArrayBuffer path, used when the page is cross-origin isolated. |
| `keys.js` | What a keypress means to the machine, cooked or raw. Copied from `src/c4bb/web/app.js`, which cannot be imported. |
| `worker.js` | The Web Worker: arena, devices, machine, run loop, messages. |
| `page.js` | The main-thread API, `C4M.boot(...)`. |
| `display.js` | Replays command frames onto the canvas; turns DOM events into event frames. |
| `cli.js` | The Node runner. |
| `serve.mjs` | A static server for the repo root, with optional COOP/COEP headers. |
| `web/` | The page: terminal on the left, display on the right. |
| [guest/](guest/README.md) | `gui.h`, the display API for guest C, and two demos. |
| `tests/` | `test-libjs.sh`, the unit tests, the CDP gate (`cdp.mjs`, `test-web.mjs`, `test-web-gui.mjs`), and `src/strc.c`. |
| `package.json` | Marks the directory as ES modules. |

Reused from c4bb: `src/c4bb/sim/arena.js`, `devices.js`, `loader.js`,
`mbox.js`, and `src/c4bb/web/panels.js` (`Terminal`). libjs imports these
and never edits them.

## Hosts

libjs is itself the host: Node for `cli.js`, a Web Worker for the page. It
runs 32-bit `.c4r` images only; the 64-bit images at the repo root do not
load. The display demos (`gui-demo`, `fb-demo`) print "not fitted" and exit
on native c4m and c4bb, where 0x400 is ordinary memory; that is how
`test-libjs` compares them against `c4m32` (the CLI fits no display).

## Limits and known issues

- 32-bit only.
- No SMP: c4mp's opcodes 66–78 are named but not implemented. All ten fused
  opcodes (79–88) are.
- C4IX's vfsload manifest race carries over from c4bb, so the C4IX leg pins
  "most entries loaded", not all.

## Docs

[docs/libjs-design.md](../docs/libjs-design.md): decisions, the interpreter,
the Worker protocol, the display device, and the milestone tracker. Its
Layout table omits `shared.js` and `guest/fb-demo.c`. The comment at the top
of `serve.mjs` still describes the shared-memory path as future work.
