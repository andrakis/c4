# The UART block write

## Why

Measured on the board, `printf` costs **74-87 instructions per emitted
byte** — every one of them in `src/c4bb/fw/fw.c`'s formatter, which
emits a byte at a time through `__fw_putc`:

| workload | uart bytes | instructions | per byte |
|---|---:|---:|---:|
| `hello32` | 6 | 522 | 87.0 |
| `test_printf` | 184 | 13,678 | 74.3 |
| `test_printloop` | 219 | 18,396 | 84.0 |

`__fw_putc` itself is 13 instructions and 80 microsteps per character —
`LEA;LI;LC;PSH;JSR` at the call site, `ENT;IMM;PSH;LEA;LI;SI;LEV` in the
callee, `ADJ` on the way back — where the store it exists to perform is
one `SI`. But inlining it only recovers 8.3% of that run, because the
per-character *loop* around it costs about three times what the call
does: reload `*f`, test `'%'`, `++count`, `++f`, each its own
`LEA;LI` / `LEA;PSH;…;SI` sequence.

The bytes are already contiguous in memory. A `%s` argument, a converted
number in the formatter's scratch buffer, and a literal run of the format
string are each a pointer and a length that the formatter already holds.
Handing those to the hardware in one store leaves the loop with nothing
to do per character.

## Why registers and not an opcode

`PUTS` (opcode 40) already writes a whole string in one instruction, and
raycast's `RC_KE` build uses it — 80,417 of 80,438 bytes leave that way.
It cannot serve the formatter, for two reasons:

- it appends `\n`, and
- a literal run is a **slice** of the format string, not a
  NUL-terminated string, and `%.*s` needs a length regardless.

So the thing needed is length-counted, which is the shape the disk write
head already has (`docs/c4bb-storage.md` M2), and its reasoning applies
here verbatim:

> *"Deliberately the same shape as the read side, so a guest that can
> read a file can write one by learning four addresses and no new
> opcode — fw.c already drives DEV_INTERVAL this way."*

Registers cost no opcode number, no microcode, no ROM depth and no
compiler change, and they work at the **base-c4 rung**: a stock-c4
program with no extended opcode set can drive them with ordinary stores.
That last point is not incidental — `raycast`'s `RC_DOS` build
(`make raycast-dos32.c4r`) is restricted to stock c4 plus `TIME` and
emits its frame with `printf("%s\n", b)`, so it is the one build that
pays the per-byte cost in full, and an opcode outside the stock set is
exactly what it could not use.

    0x1AC UART_TXADDR   w: buffer address
    0x1B0 UART_TXLEN    w: emit N bytes from it -> N written; r: result

`0x1A4`/`0x1A8` are the provenance port, so `0x1AC` is the first free
slot. `UART_TXADDR` does not auto-advance, matching `DISK_WADDR`.

## Ladder

- [x] **U1** `UART_TXADDR`/`UART_TXLEN` in `sim/devices.js`. Every byte
      still goes through the same `onByte` sink and still bumps
      `uartActivity`, so the CLI's buffering, the web terminal and the
      board's TX lamp are unchanged.
- [x] **U2** A sim test: N bytes from the address, exactly, including
      the zero-length and past-the-arena cases.
      `src/c4bb/tests/test-uart-block.mjs`, 17 assertions, green.
- [x] **U3** `__fw_write` in `fw/fw.c`, used for the three contiguous
      runs the formatter already has: the literal run between
      directives, the prefix, and the converted/`%s` body. The padding
      loops generate their bytes and stay per-character.
- [x] **U4** Firmware rebuilt (all four stages); `test-c4bb.sh` green
      end to end including the c4m differential and the C4KE/C4IX/C4DOS
      boots, `test-firmware.sh` green. Output verified byte-identical
      before and after on `raycast-dos` (22,041 bytes), `test_printf`,
      `test_printloop`, `tests`, `hello32` and `mandel` — mandel's only
      difference is its own `rendered in 354ms` -> `350ms`, which is
      the speedup rather than a divergence.
- [x] **U5** Measured; see below.
- [x] **U6** Device map in `docs/c4bb-design.md`.

## Results

`raycast`'s C4DOS build — the one that emits its frame through
`printf("%s\n", b)` — 5 frames at 80x25, 22,041 bytes:

| | instructions | in the firmware | per emitted byte |
|---|---:|---:|---:|
| before | 3,311,952 | 1,128,846 (34.1%) | 51.2 |
| after | 2,453,492 | 270,386 (11.0%) | 12.3 |

**The formatter's emit cost fell 4.2x**, and with it a quarter of the
whole program: 662,390 instructions per frame down to 490,698.

The short-output workloads move much less, because what is left is not
emission at all — it is the fixed cost of `fw_prtf`, `__fw_strlen` and
the directive parser, paid per `printf` call rather than per byte:

| workload | before | after | |
|---|---:|---:|---:|
| `hello32` | 522 | 467 | −10.5% |
| `test_printf` | 13,678 | 10,586 | −22.6% |
| `test_printloop` | 18,396 | 15,582 | −15.3% |

The rule that falls out: **this pays in proportion to run length.** A
program that prints a screen at a time gets a quarter of its
instructions back; one that prints "ok\n" gets almost nothing, and
would want the per-call overhead attacked instead.

## Still on the table

`__fw_putc` is untouched and still costs 13 instructions and 80
microsteps where the store it wraps is one `SI`. It now only carries the
padding loops and `%c`, so inlining it is worth much less than it was
before this change — but it is also still free to do.
