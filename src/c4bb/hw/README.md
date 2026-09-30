# c4bb/hw — the microcode and the board

What the machine does, and what it looks like. The two files are
deliberately separate: behaviour comes only from the microcode, and the
board layout is only a picture of it, so the picture cannot disagree with
what ran.

| File | What it is |
|---|---|
| `microcode.uc` | The CPU. Assembled by [../sim/ucode.js](../sim/ucode.js); both engines (`sim/machine.js` and `sim/turbo.js`) execute the tables it produces. |
| `board.hwd` | The physical layout: modules, nets, connections, and which control signals belong to each module. Parsed by [../sim/netlist.js](../sim/netlist.js) for [../web/board.js](../web/board.js) and nothing else. |

## microcode.uc

One line is one microstep is one bus transfer. The file holds:

- `const` lines naming device registers and low-memory latches (UART,
  clock, disk, power, the trap latches, the firmware vectors at 0x20–0x30,
  the opcode-name ROM at 0x200).
- `routine fetch:` (required) and `routine trap:`, the trap machinery that
  builds c4m's trap frame.
- One `op NAME:` routine per implemented opcode: 75 in all. That covers c4m's
  0–65 except `_BLT` (a hole in c4m's own table) and all ten fused opcodes,
  79–88 (`LDL`..`POPA`). c4mp's 66–78 are absent.
- Eight of those are `jsop`: `ITH C4CF SIGH SIGI _TRP DBG C4IV FLT` run as a
  single black-box microstep implemented in `execJsop()` in
  [../sim/machine.js](../sim/machine.js), with semantics taken from c4m.c.

The syntax is documented in the header of [../sim/ucode.js](../sim/ucode.js).
In short:

    const NAME 0x123          named constant for C= references
    routine fetch:            a control routine
    op LEA operand:           an opcode; 'operand' prefixes the operand fetch into OPR
    op C4CF: jsop             handled by the system controller
    .label:                   local label
        SIG SIG ...           one microstep: signals asserted together
        br az .label          conditional next step (flags az, tz, mz; ! negates)
        goto .label           unconditional next step
        dispatch              next step chosen by IR (fetch/OPCD only)

A routine that falls off its end returns to fetch. The ALU computes
`left OP right`, with left = B (or A with `ALU_LA`) and right = A (or T with
`ALU_RT`, or MDR with `ALU_RM`), mirroring c4's `a = *sp++ OP a`.

## Adding an opcode

1. Give it a name and number in `OPNAMES` in [../sim/devices.js](../sim/devices.js)
   (five bytes per name, c4m's order, append only). The opcode number is the
   name's position in that string; the ROM is the encoding. Raise `INS_SIZE`
   in [../sim/machine.js](../sim/machine.js) if the table grows past it.
2. If it carries an operand word, add it to `OPERAND_NAMES` in
   `sim/machine.js`. If protected mode must trap it, add it to
   `PM_GATED_NAMES` there.
3. Write the `op NAME:` routine here (`op NAME operand:` for an operand).
   Both engines pick it up; there is no second implementation.
4. Put it on a rung in `RUNGS` in [../tools/opscan.mjs](../tools/opscan.mjs).
   `make test-c4bb-rungs` runs `opscan -machine`, which fails when the
   microcode and the top rung disagree.
5. Add a differential test against native c4m (or c4mp) in
   [../tests/test-c4bb.sh](../tests/test-c4bb.sh), and run
   `node src/c4bb/tools/lockstep.js` on an image that uses it.

The opcode also has to exist in every other place the numbering is repeated
(c4m.c, load-c4r.c, c4mp, oisc4, c4cc, c4lc); see
[docs/fused-opcodes.md](../../../docs/fused-opcodes.md), "Where it has to be
mirrored".

## board.hwd

    module NAME kind=K at=X,Y size=W,H label="..." sigs=A,B,C leds=STYLE
    net NAME width=N route=X1,Y1 X2,Y2 ...
    connect MOD -> NET

`STYLE` is how the module shows its value: `hex`, `op`, `aluop`, `upc`,
`mode`, `run`, `tx` or `none`, as used in the file today. Coordinates are
board units; the renderer scales them. A module lights when
one of its `sigs` is asserted in the current microstep. Changing this file
changes the picture only.
