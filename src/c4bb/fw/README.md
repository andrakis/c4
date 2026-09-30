# c4bb/fw — the firmware

`fw.c` is the only tracked file. It supplies, in guest code, what native c4m
borrows from the host's libc, and it is the BIOS that boots a drive. The
`.c4r` files that appear here (`fw.c4r`, `fw-hello.c4r`, `fw-ram.c4r`,
`fw-drives.c4r`) are built by [../tests/build-images.sh](../tests/build-images.sh)
(`make c4bb-images`) and are not tracked. `sim/cli.js` loads `fw.c4r` unless
`-fw` says otherwise.

## What it provides

| Function | Job |
|---|---|
| `fw_malc`, `fw_free`, `fw_ralc` | malloc/free/realloc: an address-ordered free list with coalescing. Heap bounds come from the `DEV_HEAP_BASE`/`DEV_HEAP_END` device registers, which the loader sets after placing every image. |
| `fw_prtf` | printf, adapted from the `vsnprintf` in `src/c4lm/include/stdio.h`. Output goes to the UART's block-write registers rather than into a buffer. |
| `fw_strc` | A stub that returns 0: stack traces need symbols the firmware does not have. |
| `fw_init` | A constructor that fills the vector latches at 0x20–0x30 with the addresses above. |

## How the CPU reaches it

The microcode for `MALC`, `FREE`, `RALC`, `PRTF` and `STRC` is a synthesized
`JSR` through the vector latch for that opcode (`VEC_MALC` = 0x20 up to
`VEC_STRC` = 0x30 in [../hw/microcode.uc](../hw/microcode.uc)). A syscall
opcode's arguments are already on the stack where a function's would be, so
after the `JSR` these are plain C functions. No trap is raised, so the
firmware cannot collide with a kernel's trap handler.

## The four stages

One source, four images, chosen by `-D` flags to c4lc:

| Image | Flags | What it can do |
|---|---|---|
| `fw-hello.c4r` | `FW_STAGE=0` | Print a banner. |
| `fw-ram.c4r` | `FW_STAGE=1 FW_RAM=1` | + probe memory. |
| `fw-drives.c4r` | `FW_STAGE=2 FW_RAM=1 FW_DRIVES=1 FW_SEEONLY=1` | + see a medium in a drive, but not load it. |
| `fw.c4r` | `FW_STAGE=3 FW_RAM=1 FW_DRIVES=1 FW_BOOT=1` | + load and boot it, with a retry loop: the BIOS. |

The missing parts are absent from an earlier stage's image, not skipped at
run time. `make test-c4bb-firmware` checks that each stage does its job and
refuses the next one's. `cli.js -fw hello|ram|drives|bios` picks a stage.

## Gotcha

c4lc turns its preprocessor on only when at least one `-D` is passed. With
none, both sides of every `#ifdef` in this file are compiled. That is why
every stage passes `FW_STAGE`: `fw.c` never tests it, but it gives
`fw-hello` a `-D`.

## Building by hand

    ./c4sp32 -c 64000000 src/c4sp/lisp/c4lc.lisp -O -D FW_STAGE=3 -D FW_RAM=1 \
        -D FW_DRIVES=1 -D FW_BOOT=1 src/c4bb/fw/fw.c src/c4bb/fw/fw.c4r

That is the command `build-images.sh` runs; `c4sp32` comes from
`make c4bb-32bit`.
