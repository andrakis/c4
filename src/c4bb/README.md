# c4bb — the c4m breadboard computer

c4bb is the c4m virtual machine rebuilt as hardware, in the spirit of Ben
Eater's breadboard computer: a microcoded 32-bit CPU where one microstep is
one bus transfer, every control line can be watched, and the whole board is
drawn in the browser. It is simulated in JavaScript and checked instruction
for instruction against a native 32-bit build of c4m (`c4m32`).

It runs the real systems. C4DOS, C4KE and C4IX all boot on it, with
preemption, protected mode, a keyboard, drives that can be written, and a
BIOS that boots whatever medium it finds. The same machine carries the
ladder, where one OS builds the next (C4DOS → C4KE → C4IX): C4DOS compiles
C4KE on the board, and C4KE or C4DOS goes on to build C4IX. See
[docs/climbing-the-ladder.md](../../docs/climbing-the-ladder.md).

The machine is 32-bit. It loads only `.c4r` images stamped WordBits=32,
which the 32-bit toolchain (`c4cc32`, and c4lc running under `c4sp32`)
produces. The 64-bit images at the repo root do not load.

## Build

Run from the repo root.

| Target | What it produces |
|---|---|
| `make c4bb-32bit` | The 32-bit native toolchain: `c4cc32`, `c4m32`, `c4sp32`, `c4rlink32`. |
| `make c4bb-images` | Everything under `src/c4bb/images/` (generated, gitignored) and the four firmware stages in `fw/`, via [tests/build-images.sh](tests/build-images.sh). Needs `./cpp`. |
| `make c4bb-web` | Everything the browser loads: builds the `c4dos-c4ix32` climb disk first (the slow half; it builds c4sc), then `c4bb-images`. |
| `make c4dos-c4ix32` | The C4DOS disk that climbs to C4IX. `make c4bb-images` remakes it after the images (it carries their C4IX userland) and copies it into `images/climb` and `libjs/images/ladder` again (`tests/climb-disks.sh`). `build-images.sh` run by hand only copies it, and warns when the copy is older than its sources. |

`c4bb-images` makes four disks: `images/disk` (the shared disk: C4DOS, C4KE
and C4IX side by side, which `test-c4bb` boots), `images/dos-recovery`,
`images/c4ke-root`, and `images/climb` (a copy of `c4dos-c4ix32`).

There is nothing to install. No `node_modules`, no bundler; Node and a
browser are enough.

## Run

In a terminal:

    node src/c4bb/sim/cli.js [-m MB] [-c cycles] [-s] [-d dir] [-w dir] [-i] [-hz MHz]
                             [--fast] [--step] [--whowrote] [-fw STAGE] [program.c4r [args...]]

| Flag | Meaning |
|---|---|
| `-m MB` | Arena size (default 32). |
| `-c cycles` | Cycle budget (default unlimited). |
| `-s` | Print machine stats to stderr on exit. |
| `-d dir` | Attach `dir` as a read-only drive. Drives are numbered in the order given. |
| `-w dir` | Attach `dir` as a writable drive. |
| `-i` | Interactive: raw stdin feeds the keyboard. Without it, piped stdin is fed up front with end-of-file set, which is how scripted sessions run. |
| `-hz MHz` | How fast the machine claims to be (default 20). Guests time themselves against it. |
| `--fast` | Run as fast as the host can instead of pacing to the machine's clock. |
| `--step` | Use the microstep engine instead of the compiled turbo engine. |
| `--whowrote` | Record which PC last wrote each word, answerable through a device port. |
| `-fw STAGE` | Firmware: `hello`, `ram`, `drives` or `bios` (default), or a path to a `.c4r`. |

With no program the firmware boots from the first drive that has one, as a
machine with only a BIOS would.

    node src/c4bb/sim/cli.js -i -d src/c4bb/images/disk src/c4bb/images/c4ix32.c4r
    printf 'ps\nexit\n' | node src/c4bb/sim/cli.js -d src/c4bb/images/disk src/c4bb/images/c4ke32.c4r
    node src/c4bb/sim/cli.js -m 16 -fw drives -d src/c4bb/images/climb   # sees the disk, cannot boot it
    make run-c4dos-c4ix32                                                 # C4DOS on the climb disk, interactive

In a browser:

    make c4bb-web
    make serve-c4bb        # or: npm start, from this directory

then open http://localhost:8471/src/c4bb/web/index.html. `serve-c4bb` is
`python3 -m http.server` from the repo root. The page fetches `../hw/`,
`../fw/` and `../images/`, so serving `web/` on its own gives an empty
drives panel. `C4BB_PORT=NNNN` moves it. Pick `c4ix32` or `c4ke32`,
press Turbo, click the terminal and type; or pick the BIOS and let it boot a
drive. The drives panel says what each medium holds, and New makes a blank
writable medium that survives a reload.

## Test

None of these are in `make test`.

| Target | What it checks | Cost |
|---|---|---|
| `make test-c4bb` | The main suite: every corpus image against native `c4m32`, the fused opcodes, the trap tests, both kernels booting, step-vs-turbo lockstep, the mailbox, the terminal and drive logic, the `arena-whowrote.js` fork check. | Builds `c4bb-images` first. |
| `make test-c4bb-storage` | Drives and media: what one session writes, a later one reads. | Needs the climb disk. |
| `make test-c4bb-firmware` | The four firmware stages each do their job and refuse the next one's. | |
| `make test-c4bb-climb` | Three power-ons, three systems, each booted from a medium the previous one wrote. `test-c4bb-install` is the same target. | |
| `make test-c4bb-rungs` | Which opcodes each image uses, against the rung it belongs to, and that `hw/microcode.uc` implements exactly the top rung. `test-c4bb-baseops` is the old name. | |
| `make test-c4bb-web` | The page in a real browser over CDP. Skips and passes when no browser answers. | |
| `make test-c4dos-build32` | C4DOS on the board builds C4KE from source and boots it. | Under fifteen seconds, per the Makefile. |
| `make test-c4dos-ladder32` | LADDER.BAT: the self-built compiler is a fixed point and builds the same kernel. | |
| `make test-c4dos-c4ix32` | C4DOS on the board compiles and links C4IX, then boots it. | About four minutes of simulated hardware; not in any default chain. |
| `make test-c4th-bb` | c4th (32-bit) runs on the board. | The cheap half: c4fc on c4th compiling a C4IX module on the board takes five and a half minutes and is not a routine test. |
| `make test-c4sc-board` | The compiled c4lc (`c4sc32-fused.c4r`) compiles one C4IX module on the board, byte-identical to c4lc. | One module (`boot.c`), the smallest. |
| `make test-ladder` | The DOS → C4KE handover and in-memory compile/link. Its script lives in `tests/`, but it runs on native 64-bit c4m, not on c4bb. | Minutes. |

[tests/README.md](tests/README.md) lists the scripts behind these.

## Layout

| Path | What it is |
|---|---|
| [hw/](hw/README.md) | The microcode and the board layout. The microcode is the source of truth for what the CPU does. |
| [sim/](sim/README.md) | The engines, memory, devices, loader and the Node runner. No DOM. |
| [fw/](fw/README.md) | The firmware: allocator, printf, and the BIOS, in four stages from one source. |
| [web/](web/README.md) | The board in a browser. |
| [fs/](fs/README.md) | VFS manifests: the file trees C4KE and C4IX build at boot. Source, not build output. |
| [tools/](tools/README.md) | The lockstep prover, the opcode and memory censuses, `reboot.c`. |
| [tests/](tests/README.md) | `build-images.sh`, the test scripts, and the board's own trap tests. |
| `images/` | Generated by `make c4bb-images`. Gitignored; safe to delete. |
| `package.json` | `npm start` and `npm test`, which call the two make targets above. |

## Hosts

- **Node**: `sim/cli.js`, used by every test.
- **Browser**: `web/`, same engines, same devices.
- **libjs** ([../../libjs/README.md](../../libjs/README.md)) imports
  `sim/arena.js`, `sim/devices.js`, `sim/loader.js`, `sim/mbox.js` and
  `web/panels.js`. It replaces the microcode with a direct interpreter; the
  memory map, devices and disk format are shared.

Time on the board comes from the cycle counter, not the host clock: runs are
repeatable, and none of the `/proc/uptime` issue that plain c4 has applies.

## Limits and known issues

- 32-bit images only.
- c4mp's opcodes 66–78 (`CPUI`..`TRAW`) are named in the opcode ROM but not
  implemented. All ten fused opcodes (79–88) are implemented.
- `STRC` prints nothing (the firmware has no symbols). `%p` prints arena
  offsets. `RALC` works, unlike native c4m's. printf runs in guest firmware,
  so absolute cycle counts after the first printf differ from native c4m.
- `test-c4ke-mbox`, run inside `test-c4bb`, fails about one run in four with
  "Custom opcode not found" in the mbpair message; it fails as often with the
  older C4KE source ([docs/c4ix-desktop.md](../../docs/c4ix-desktop.md),
  "Found on the way").
- C4IX's vfsload sometimes loses one manifest entry at boot, which looks like
  a preemption race; the tests require most entries, not all
  ([docs/c4bb-design.md](../../docs/c4bb-design.md), "The C4IX filesystem").

## Docs

- [docs/c4bb-design.md](../../docs/c4bb-design.md): the machine, memory map,
  devices, microcode, trap machinery, firmware and filesystems. It has
  drifted: it does not mention the fused opcodes, says the firmware's printf
  emits through PUTC (it now uses the UART block write), and calls the
  machine 1 MHz (it is 20 MHz by default).
- [docs/c4bb-storage.md](../../docs/c4bb-storage.md): drives, media, firmware
  stages, the climb, and the opcode rungs.
- [docs/c4bb-uart-block.md](../../docs/c4bb-uart-block.md): the UART block write.
- [docs/fused-opcodes.md](../../docs/fused-opcodes.md): the ten fused opcodes;
  its "What this costs c4bb" section predates c4bb implementing them (F7 in
  that document's tracker records it).
- [docs/compiler-on-the-board.md](../../docs/compiler-on-the-board.md): why
  the build floppy carries c4cc and not c4fc.
- [docs/c4bb-port.md](../../docs/c4bb-port.md): a dated investigation
  (2026-08-26) of porting c4m and c4mp changes to c4bb. Its "where c4bb is"
  counts are out of date.
- [docs/climbing-the-ladder.md](../../docs/climbing-the-ladder.md).
