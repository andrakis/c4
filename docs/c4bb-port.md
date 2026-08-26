# Porting the c4m and c4mp changes to c4bb

An investigation, not a plan to execute. It answers three questions:
what has changed on the VM side since c4bb was last touched, what each
change would cost c4bb, and whether now is the moment. It also records
what Homeward has to import, since c4bb is that project's oracle.

Everything below was measured or read on 2026-08-26 at `4e6c610`.
**`make test-c4bb` passes as things stand** — nothing here is a
regression report.

## Where c4bb is

- **66 opcode names, 65 implemented.** The names live in
  `src/c4bb/sim/devices.js:53-59` (`OPNAMES`, five bytes each) and the
  numbers are derived from that string in `src/c4bb/sim/machine.js:20-22`.
  It is a verbatim mirror of `c4m.c`'s enum truncated at 66, so c4bb
  stops at `DBG` and knows nothing after it.
- **`_BLT` (46) is named but has no microcode**, so it dispatches to
  `TRAP_ILLOP`. That gap predates all of this.
- **`src/c4bb/hw/microcode.uc` is 549 lines / 319 microsteps.** One line
  is one microstep is one bus transfer; the assembler enforces it
  (`src/c4bb/sim/ucode.js:216-218`). A typical opcode is 2-6 steps.
- **32-bit, and the parity oracle is `c4m32`, not `c4m`.** The loader
  refuses anything else (`src/c4bb/sim/loader.js:24-25`).
- **No golden files.** `src/c4bb/tests/golden/` exists and is empty:
  every parity claim is a live differential against a native 32-bit c4m
  at test time. That is the important structural fact for everything
  below — c4bb cannot drift from c4m silently **on the images the suite
  runs**, and can drift freely on the ones it does not.

## What has changed since c4bb was last touched

Four commits touch `c4m.c`, `src/c4mp/` or `load-c4r.c` after c4bb's
last change (`36b58b8`):

| commit | what | does c4bb need it? |
|---|---|---|
| `63e274a` | c4m: RALC fixed; a side table records guest block sizes | **No — but see the divergence below** |
| `13a369d` | the B5c opcode probe | No. Measurement tooling; it is what chose the opcode set |
| `958eeb1` | the fused opcode set and everything that must agree about it | Optional, and the interesting one |
| `bbb9ec3` | the set cut from 26 to 10, split three in c4m and seven in c4mp | Optional |

### RALC: c4bb got there first, and differs in one case

c4m's fix exists because c4m allocates with the host's `malloc` and had
no record of block sizes, so `realloc` could not know how much to copy;
it now keeps an open-addressed table (`c4m.c:1119-1160`).

**c4bb never had that problem.** Its allocator is its own firmware, and
`fw_ralc` (`src/c4bb/fw/fw.c`) reads the size out of the block header it
already keeps. No port is needed.

They do not agree on one case, and it is worth writing down:

```
c4bb  fw_ralc:   if (n >= size) return ptr;    // shrinking keeps the pointer
c4m   c4_realloc: always malloc, copy min(old,n), free      // always moves
```

A program that shrinks an allocation and compares the pointer gets
different answers on the two machines. **The suite cannot see it**:
`src/tests/test_realloc.c` — the test written for c4m's fix — is not in
c4bb's image list (`src/c4bb/tests/test-c4bb.sh:63-66, 89-90`). Adding
it there is a one-line change and would either prove parity or print the
difference. That is the cheapest useful thing in this document.

### The fused opcodes, and why the obvious three are the wrong three

`docs/fused-opcodes.md` already costs this and its numbers hold up
against the survey above:

- **No new circuitry.** Every fused opcode is a concatenation of
  transfers the board already performs — same registers, same ALU, same
  bus, and both control patterns it needs are already in use.
- **No second engine.** `src/c4bb/sim/turbo.js` compiles the assembled
  step table generically (`turbo.js:71-119`), so microcode written once
  gets the fast path free, and the lockstep test pins the two together.
- **c4m's three (LDL, STL, POPA): ~12 microsteps, +4%.**
- **c4mp's seven: ~34 more.**

The part worth deciding on is which. From the probe table in
`docs/fused-opcodes.md`, as a share of instructions actually executed:

    c4m's three        LDL   0.35% (c4cc)   3.20% (c4sp)
                       STL, POPA — not measured; they exist for c4th's
                       native backend, not for compiler output
    c4mp's seven       PSHG 19.46%   IMMP 7.52%   ADDL 3.41%
                       PSHL  2.76%   LIP  0.59%   LDG  0.55%
                       LEAP  0.43%          -> ~34.7% of c4cc
                       (and ~29% of c4sp, with the weights swapped)

**So if the goal is compile time on the breadboard, c4m's three buy
almost nothing and c4mp's seven buy nearly all of it.** The three were
put in c4m because a Forth's native backend needs frame access, not
because a compiler does.

**And c4bb can take the seven without becoming a multiprocessor.** The
numbering is append-only: the fused opcodes are 79-88 and c4mp's SMP set
is 66-78, which are independent. c4bb would name 0-88, implement 0-65
and 79-88, and leave 66-78 named-but-trapping — exactly the pattern
`c4m.c:277-284` already uses for the same opcodes. `docs/fused-opcodes.md`
frames the seven as "the c4mp extension", which is true of the VMs and
need not be true of the board.

### What taking all ten would touch in c4bb

Read off the survey, in order of size:

1. `hw/microcode.uc` — ten `op` blocks, ~46 microsteps total. 319 → ~365,
   **+14%**, which on a breadboard is ROM depth, i.e. chips.
2. `sim/devices.js:53-59` — the name table grows from 66 entries to 89
   (13 of them named-but-unimplemented). Mandatory: `machine.js:57-59`
   throws if microcode names an opcode the table does not.
3. **The opcode-name ROM at 0x200 grows** with it — 66x5 = 330 bytes
   today (ending 0x349, not the 0x034F `docs/c4bb-design.md` claims),
   89x5 = 445 bytes ending 0x3BD. `sim/loader.js:94-98` copies it and
   `_OPC`/`OPSL` serve from it. Nothing sits between there and
   `MEM_BASE` at 0x1000, so there is room — but the constant is what
   `__opcode()` and `__c4_ops_list()` answer from, and a guest that
   feature-tests by name will start getting numbers for opcodes the
   board still traps. c4m has exactly this property and documents it.
4. `sim/machine.js:22` `INS_SIZE = 66`, plus two hardcoded `i < 66`
   guards in `web/board.js:20` and `web/panels.js:7`.
5. Nothing in `sim/turbo.js`, because no new control signal is involved.

Nothing outside `src/c4bb/` changes. c4bb executing an opcode is
independent of anything emitting it.

## Is now a good time?

**For the RALC differential, yes and it is one line** — it closes a real
unknown for the cost of adding an image to a list.

**For the fused opcodes, the blocker named in `docs/fused-opcodes.md` is
gone.** That document deferred c4bb "until this is proven on the main
toolchains" and asked that hardware work "follow a decision to turn the
flag on by default rather than precede it". The first half is now true
three ways over: c4lc emits them, c4opt fuses them, and c4fc emits both
`-mfuse` and `-mcisc` byte-identically to c4lc with `tests.c` running
under c4mp while c4m and plain c4 name the opcode they lack.

The second half is still open, and it is the real question: **nothing
turns the flag on by default, so a c4bb that implements the fused
opcodes runs exactly the same images at exactly the same speed until
something does.** The order that costs least is therefore:

1. add `test_realloc` to c4bb's corpus (one line, closes an unknown);
2. decide whether `-mfuse` becomes the default for the images c4bb
   boots — that is a toolchain decision, measurable today with
   `sh src/c4th/bench/fuse-probe.sh`, and it is what makes the hardware
   work worth doing;
3. only then write the microcode, and write the **seven**, not the
   three, if the reason is compile time.

## What Homeward has to import

Homeward vendors `vendor/c4bb/{hw,sim}` and imports `machine.js`,
`turbo.js`, `ucode.js` and `arena.js` (`scripts/opcensus.ts:27-30`).
Provenance is a one-line header on each file naming the commit it came
from — currently `af9f0af`.

**It is stale by exactly one commit.** `hw/microcode.uc` and five of the
seven `sim/*.js` are byte-identical to c4bb today; `devices.js` and
`loader.js` moved on in `36b58b8` (the HOMEWARD ladder commit). So an
import is a clean re-copy with the header line re-applied — there is no
drift to reconcile, which is the good case and worth keeping that way.

Two things would break Homeward if the fused opcodes land:

- `OP` and `INS_SIZE` are imported directly, and `scripts/opcensus.ts`
  counts per opcode. A table growing from 66 to 89 entries changes what
  a census means, and 13 of the new names would be permanently zero.
- The opcode-name ROM moving from 330 to 445 bytes changes the memory
  map any Homeward content that inspects it depends on.

Neither is hard; both want to be noticed before rather than after.
