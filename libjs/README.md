# libjs — c4m in JavaScript

A direct interpreter for the c4m virtual machine, written the way `c4m.c`'s
dispatch loop is: one `switch` over an `Int32Array`. It boots the 32-bit
C4KE and C4IX images in Node or in a browser Web Worker, with a terminal, a
display device that a guest program draws into, and a sound card (an AdLib
OPL, eight channels of 8-bit samples and the PC speaker).

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

    node libjs/cli.js [-m MB] [-c cycles] [-s] [-d dir] [-w dir] [-i] [-hz MHz] [--fast] [--mbox BYTES] [--wav FILE] program.c4r [args...]

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
| `--wav FILE` | Fit a sound card and write what it played to FILE, 44.1 kHz 16-bit stereo, on the machine's own clock, so the same session makes the same file. Without it there is no card and programs run silent. |

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
(`shared.js`); without them it falls back to posting messages. The page
always has the sound card; a browser lets it play only after the first key
or click.
`LIBJS_PORT=NNNN` moves it.

URL parameters (from `web/app.js`). The page's Configuration button shows them as a form, and OK reloads the page with the ones changed:

| Parameter | Meaning |
|---|---|
| `?system=c4ix\|c4ke\|c4dos\|wolf3d\|wolf3d-c4ix\|doom\|tyrian\|rott\|gui-demo\|fb-demo\|snd-demo\|raycast` | Which machine to boot (default `c4ix`). `c4dos` is C4DOS on the games' disk, with Wolfenstein 3D, Doom, Tyrian and Rise of the Triad; `wolf3d`, `doom`, `tyrian` and `rott` are those games on the bare machine, and `wolf3d-c4ix` Wolfenstein 3D on C4IX's whole display (docs/c4ix-wolf3d.md, src/doom/README.md, src/opentyrian/README.md, src/rott/README.md; Tyrian's data only where it was there to copy). The last four are bare programs on the display or the sound card. |
| `?image=URL&disk=URL` | Any other image and disk. |
| `?mhz=N` | Paced speed (default 100). |
| `?fast` | Unpaced. |
| `?realtime=0` | When the host cannot keep up with `?mhz`, the machine's clock slows with it. By default it keeps the wall's time (`worker.js`), so a game drops frames rather than playing in slow motion. |
| `?mem=MB` | Arena size (default 64). |
| `?shared=0` | Use the message path even when isolated. |

The running machine is `window.c4m` on the page, so the console or a test
can drive it (`c4m.write('ps\n')`).

## The Code Explorer

`/libjs/web/explore.html`, from the same server: any image, object or
library in the repository (the list is every one under
`src/c4bb/images`; a path, or a file dropped on the page or opened from
this computer, does as well).

- **Functions, globals and strings**, filtered by a regex. A body with
  no symbol (a static, a helper the compiler made) is listed by where it
  starts, `<code@0x…>`. A string is any the code takes the address of.
- **The code** of the one chosen, with every operand named: calls and
  `&function` link to the function, branches to labels, data to the
  global (and offset) or the string literal, `[bp+n]` to the parameter
  when the source is found, and anything still to link (in a `.c4o` or
  `.c4l`) by its symbol. Fused opcodes are green, the machine's
  syscalls pink.
- **Its source**, matched by name: each `.c` in the directories named
  (guessed from the image; the guess is kept per image) is scanned for
  function definitions, and the function's lines are marked. Names in
  the source that are functions in the image link back to them.
- **Refs**: who calls it, or takes its address, and from where; what it
  calls; the globals it uses; its instructions by opcode.
- **Data**: a global's bytes, the pointers in it (patched at load), and
  who uses it.
- **Go to** a name, a code word (`0x1f4`), or a machine address
  (`@0x1a3f0`, against the image's base: 0x1000 on the bare machine).
  The URL keeps the image and function (`#img=…&fn=…`), and Alt+Left and
  Alt+Right go back and forward.

Matching source by name is a stopgap: line by line needs a source map
in the image, track S's `SMAP` chunk (docs/c4-explorer.md 2), which no
compiler writes yet. `c4r.js` already reads one (its test builds one),
and the code pane bands its instructions by row when an image has it;
the source pane follows the rows once S1 gives the page real ones.

## Test

Neither is in `make test`.

| Target | What it checks |
|---|---|
| `make test-libjs` | Unit tests (printf, heap, display, shared memory, desktop, the explorer's `c4r.js` over every image), then every corpus image byte for byte against native `c4m32` (or 64-bit `c4m` where `c4m32` cannot be trusted), the fused-opcode differential, STRC with symbol names, exit status, the PIT, and C4KE and C4IX booting with grep assertions. Builds `c4bb-images` first. |
| `make test-libjs-web` | The page in the user's own Chrome over raw CDP (`LIBJS_CDP`, else ports 9224 then 9222), then the display legs with `--gui`. It never launches a browser; if none answers it says so. |

## Layout

| File | What it is |
|---|---|
| `c4m.js` | `Machine`, the interpreter: registers, the dispatch loop, traps, syscalls. No DOM, no Node. |
| `bus.js` | Addresses below 0x1000: 0x100–0x1FF go to c4bb's `Devices`, 0x400–0x47F to the display, 0x480–0x4FF to the sound card. |
| `heap.js` | The host-side allocator behind MALC/FREE/RALC, with a size side table as native c4m keeps. |
| `printf.js` | PRTF: glibc's format rules over 32-bit words. |
| `boot.js` | Builds the machine, loads an image with c4bb's loader, and starts `main` through a c4mp-style trampoline. |
| `gui-device.js` | The display registers and its two rings (c4bb's mailbox format), host side. |
| `shared.js` | The display's SharedArrayBuffer path, used when the page is cross-origin isolated. |
| `snd-device.js` | The sound card's registers: each write becomes an event with a time on it. |
| `snd-mix.js`, `snd-worklet.js` | The mixer that turns those events into samples, in the page's AudioWorklet or for `cli.js --wav`. |
| `opl.js`, `opl-tables.js` | The AdLib's OPL2/OPL3: DOSBox's dbopl, ported line by line (GPL v2 or later). |
| `keys.js` | What a keypress means to the machine, cooked or raw. Copied from `src/c4bb/web/app.js`, which cannot be imported. |
| `worker.js` | The Web Worker: arena, devices, machine, run loop, messages. |
| `page.js` | The main-thread API, `C4M.boot(...)`. |
| `display.js` | Replays command frames onto the canvas; turns DOM events into event frames. |
| `cli.js` | The Node runner. |
| `c4r.js` | The Code Explorer's reader: a C4R image, object or library at either word size, every segment and symbol, decoded with the machine's own opcode table, operands resolved, cross-references both ways. |
| `serve.mjs` | A static server for the repo root, with optional COOP/COEP headers, and a directory's listing as JSON with `?ls`. |
| `web/` | The page: terminal on the left, display on the right. `explore.html`, the Code Explorer. |
| [guest/](guest/README.md) | `gui.h` and `snd.h`, the display and sound card APIs for guest C, and their demos. |
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
