# c4bb/sim — the simulator core

The machine without a screen: CPU engines, memory, devices, the `.c4r`
loader and the Node runner. Nothing here touches the DOM, so the same modules
run under Node, in the page ([../web/](../web/README.md)), and inside
[libjs](../../../libjs/README.md), which imports `arena.js`, `devices.js`,
`loader.js` and `mbox.js`.

| File | What it is |
|---|---|
| `ucode.js` | Assembler for [../hw/microcode.uc](../hw/microcode.uc). Its header documents the microcode syntax and signal names. |
| `machine.js` | The step engine: registers, one microstep at a time, an event per step for the board renderer. Holds the opcode tables derived from the ROM (`OP`, `hasOperand`, the protected-mode set) and `execJsop()`, which implements the eight `jsop` opcodes. |
| `turbo.js` | The fast engine: the same assembled step tables compiled to one JS function per opcode. Same transfers and cycle counts as `machine.js`, without per-step interpretation. |
| `arena.js` | The flat byte-addressed memory and the low-memory map (null guard, TLEV ROM word, firmware vectors at 0x20, device window at 0x100–0x1FF, opcode-name ROM at 0x200, images from 0x1000). |
| `arena-whowrote.js` | A fork of `arena.js` that also records which PC last stored to each word, for `cli.js --whowrote`. [../tests/check-fork-arena.sh](../tests/check-fork-arena.sh) fails if it stops being `arena.js` plus its marked blocks. |
| `devices.js` | The device registers at 0x100–0x1FF: UART (and its block write), the deterministic clock, `USLP`, the disk read and write heads and drive selection, power, heap bounds, `INFO`, the trap latches, soft reset, the real-time clock and PIT, the mailbox registers, the `whowrote` port. Also `OPNAMES`, the opcode-name ROM, which is the opcode encoding. |
| `loader.js` | A port of `c4l.c` for 32-bit images: places the firmware, then the program, runs constructors, and enters `main`. |
| `mbox.js` | The host side of the mailbox: two rings of i32 frames in guest RAM. The guest side is `include/c4bb_mbox.h`; the two must agree word for word. |
| `netlist.js` | Parser for [../hw/board.hwd](../hw/board.hwd). Layout only. |
| `cli.js` | The Node runner. Flags are listed in [../README.md](../README.md#run). |

## Contracts

- **One CPU definition.** Both engines run the tables `ucode.js` builds from
  `hw/microcode.uc`. [../tools/lockstep.js](../tools/lockstep.js) proves them
  register-identical at every instruction boundary; `test-c4bb.sh` anchors
  both against native `c4m32`.
- **Determinism.** `TIME_MS` is derived from the cycle counter
  (`CYCLES_PER_MS`, 20000 by default), so the same image and input give the
  same run. `RTC_MS` and `PIT_MS` are the registers that follow the host's
  clock instead, and a guest has to check `INFO` before using them.
- **The ROM is the encoding.** Opcode numbers come from the position of each
  name in `OPNAMES`. `setOpcodeNames()` in `machine.js` swaps the ROM and
  rebuilds every derived table.
- **Capabilities are announced.** A guest asks `INFO` before touching an
  optional device (PIT, mailbox, `whowrote`); on native c4m those addresses
  are not devices.
