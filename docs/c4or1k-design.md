# C4OR1K — jor1k ported to C, compiled by c4lc

`src/c4or1k` is an OR1000/OpenRISC CPU emulator, capable of booting a
real Linux kernel in a console, written in C and compiled by c4lc. It
is a port of [jor1k](https://github.com/s-macke/jor1k) (a JavaScript
OR1000 emulator originally built for the browser) to the C4 VM
toolchain — the same toolchain that carries C4KE and C4IX. Console
only: no framebuffer, no keyboard device. Terminal raw mode is an
external concern (`run-c4or1k.sh`), not something the VM provides —
see that script's header comment and "Why fd 0 needs a wrapper, not a
VM change" below.

    make c4or1k-m0                          # build + run M0

## Why this is tractable

jor1k's core CPU (`jor1k/js/worker/or1k/safecpu.js`, in the sibling
`jorconsole` repo) is about 1,100 lines: a plain per-instruction
switch on the primary 6-bit opcode, sub-switches on function-code
fields, ~100 instruction forms total. c4lc already carries two
multitasking OSes (C4KE, C4IX) and a hardware-breadboard
reimplementation of c4m itself (C4BB) — a device-emulation program an
order of magnitude simpler than either is a reasonable single-person
port, provided the C4 VM is fast enough (see M0 below) and c4lc's C
dialect covers what's needed (it does — see "Language notes").

The plan (all milestones, risks, and the jor1k/toolchain findings that
informed it) lives in the session that designed this; this doc tracks
results as milestones land.

## Language notes: what bit c4lc's dialect specifically

c4lc supports everything this project needs — structs, bitwise ops,
function pointers, switch, a real preprocessor (`-P`) — with one sharp
edge worth calling out because it produced a real bug in M0, not a
hypothetical one:

**`int` is host-pointer-width (64-bit under the default c4sp/c4m), and
there is no unsigned type.** The classic C sign-extension idiom
`(v << (32 - bits)) >> (32 - bits)` relies on the left shift
*truncating* at bit 31 — true on a real 32-bit register, false on
c4lc's 64-bit `int`, where nothing is discarded and the shift-left/
shift-right pair just reconstructs the original value unchanged. M0's
first build compiled and ran (no error, no crash reported until
deep into the loop) but a `l.j` back-branch landed the guest PC at
`0x4000003` instead of 4 instructions back, and the VM eventually hit
unmapped memory. `cpu.c`'s `sext(v, bits)` fixes this with a
width-independent mask + XOR/subtract form instead of shift/truncate —
correct at any word width, not just 32. Every subsequent milestone
that extracts a signed field from an instruction word must use it, not
the shift trick.

Other dialect notes from the toolchain investigation, relevant from M1
onward: `>>` is arithmetic (signed) only — OR1000's `l.srl` (logical
shift right) needs the same kind of explicit masking, not a shift
trick. No multi-dimensional arrays — guest RAM and the SPR groups are
flattened to 1-D, which is how jor1k indexes them internally anyway.
No struct-by-value params/returns — not needed here.

## Why fd 0 needs a wrapper, not a VM change

No `ioctl`/`termios` facility exists anywhere in c4/c4m: `READ`/
`WRITE` are thin wrappers over the host's `read()`/`write()`, so the
guest just inherits whatever termios state fd 0 already has.
`run-c4or1k.sh` puts the terminal in raw mode before exec'ing the
emulator and restores it on exit via a trap — the only viable
approach, and the same division of responsibility C4IX's `con.c` uses
(cooked tty assumed, worked around at the OS layer). Console I/O
(M3) will also reuse `con.c`'s non-blocking-second-fd pattern for
stdin, since a blocking `read()` on fd 0 would freeze the whole
single-threaded VM process while waiting for a keystroke.

## Milestones

**M0 — throughput sanity check + decode skeleton. Done.**
`main.c` hand-assembles a decrement-and-branch loop (`tools/asm_m0.py`
generates the encodings, read directly from safecpu.js's opcode
numbers and field positions, not the OR1000 spec) over 8 real OR1000
opcodes, using the same pc/nextpc delay-slot scheme safecpu.js uses,
and measures wall-clock guest-instructions/sec on the real c4/c4m VM.

**Result: ~1.37M guest instructions/sec** (6,000,006 instructions in
~4.4s; `make c4or1k-m0`), against a pre-M0 worst-case estimate of only
50K–200K/sec. VM cycle count for the same run was ~1.21B, i.e. ~200
VM "cycles" per dispatched guest instruction (c4m's cycle counter,
`__c4_cycles()`, does not appear to be 1:1 with dispatched VM bytecode
ops — see caveat below).

**Caveat, read before treating this as the answer for M1+:** this
number is a best case. It's a syscall-free, trap-free, memory-array-
free tight loop over 8 opcodes with a 2-way branch — no byte-addressed
RAM access with endian composition + masking (M1), no SPR/exception
overhead (M2), no per-instruction-batch host polling (M3+). The real
ISA's ~100-form switch and the added per-access work will bring this
down; M1 should re-measure on the full decoder before trusting a boot-
time projection. That said, the gap between 1.37M/sec and the 50K/sec
floor that would have made this impractical is wide enough that
**the project is a clear go** — M1 is worth building.

**M1 — full ISA decode. Done.** `cpu.c`/`cpu.h`/`mem.c`/`mem.h` (split
out of M0's single `main.c`, linked with c4rlink the way `src/c4mp`
does it) implement every non-privileged OR1000 integer instruction
safecpu.js has: arithmetic, shifts, `ff1`/`fl1`, `mul`/`div`/`divu`,
all register and immediate compares, branch-delay-slot-correct
`l.j`/`l.jal`/`l.jr`/`l.jalr`/`l.bf`/`l.bnf`, every load/store width
including `l.lwa`/`l.swa`'s reservation mechanism. No MMU, no SPRs, no
exceptions yet — `l.mfspr`/`l.mtspr`/`l.rfe`/`l.sys`/`l.trap` fall
through to an explicit "unimplemented, dump state and halt" path
(M2's job). `main.c` now loads a flat binary of assembled words at
guest address 0 and runs it as a test-program harness rather than
M0's fixed throughput loop.

**Verification: bit-for-bit match against jor1k.** `tools/asm.py` (a
small two-pass assembler with labels) and `tools/or1k-oracle.js` (a
headless Node driver that requires jor1k's actual `safecpu.js` — not
a transcription of it) let `tests/m1_test.s` run through both
implementations and diff every register, `SR_F`/`SR_CY`/`SR_OV`, and
a 64-word RAM window every test result is written into (`make
c4or1k-m1-check`). Current state: **exact match**, covering every
instruction form at least once.

Three real bugs surfaced building this, each worth keeping in mind
for M2 onward:

1. **c4lc's enum initializers must be a literal, not an expression.**
   `enum { RAM_SIZE = 1 << 20 };` fails to compile ("bad enum
   identifier") — c4lc's enum parser only accepts a `Num` token.
   Write the literal (`0x100000`) instead.
2. **c4lc's data segment is capped at 256KB** (`g:DMAX` in
   `c4lc-gen.lisp`, shared across every global in the whole linked
   program) — a `char ram[RAM_SIZE]` static array fails to compile
   ("data segment full") at any size that matters, and M4 needs room
   for a 5.7MB `vmlinux.bin` regardless. `mem.c` allocates `ram` with
   `malloc()` instead (`mem_init()`), which comes from the VM's
   heap, not the fixed data segment. This wasn't a workaround for a
   one-off problem — any future large guest-RAM-shaped buffer needs
   the same treatment.
3. **OR1000's mandatory branch delay slot is not optional to get
   right, including in test code.** The first draft of
   `tests/m1_test.s` placed real instructions directly after
   `l.bf`/`l.j` with no `nop`, so the delay-slot instruction (which
   *always* executes, taken or not) silently overwrote every
   compare's result — all ten compares read back "true" regardless of
   the actual condition. cpu.c's decode was correct; the test program
   wasn't. Caught immediately by the oracle diff, not by inspection.

The oracle itself needed two rounds of fixes before it was trustworthy
(both are `or1k-oracle.js`'s problem, not cpu.c's, but they cost real
time misdiagnosing cpu.c first): jor1k's `SafeCPU` aliases the
register file onto heap byte 0, so real RAM has to start at heap
offset `0x100000` (`system.js`'s `ramoffset`) or a loaded test program
silently clobbers `r0`-`r9` before executing a single instruction; and
jor1k's "Big" memory accessors are not big-endian byte composition at
all — `Read32Big`/`Write32Big` are plain native `int32mem[addr>>2]`
access, and `Read8Big`/`Read16Big` reach the same native storage via
XOR'd addressing (`addr^3`, `addr^2`) rather than physically
byte-swapped storage. An oracle that invents its own "obviously
correct" big-endian formula instead of reading `ram.js` will produce
self-consistent-looking wrong answers, not a crash.

**M2 — SPRs, exceptions, MMU, real TLB-miss vectors. Done.** `cpu.c`
gained: the full SR flag register (`cpu_set_flags`/`cpu_get_flags`,
packing/unpacking all 17 flag bits exactly as safecpu.js's SetFlags/
GetFlags do); `cpu_get_spr`/`cpu_set_spr` (group 0 general SPRs +
SR, group 1/2 DTLB/ITLB match+translate registers, group 9 PIC,
group 10 tick timer -- TTMR/TTCR are stored but nothing fires
`EXCEPT_TICK` automatically yet, see below); `cpu_exception` (EPCR/
EEAR/ESR save, the EXCEPT_SYSCALL-vs-everything-else EPCR-offset
distinction, the flag resets, the vector jump); `l.mfspr`/`l.mtspr`/
`l.rfe`/`l.sys`/`l.trap`, all real now; and `dtlb_lookup`/`fetch_ins`,
which check the current DTLB/ITLB match register and permission bits
and raise `EXCEPT_DTLBMISS`/`EXCEPT_ITLBMISS`/`EXCEPT_DPF`/
`EXCEPT_IPF` on a miss or fault -- **not** a page-table walk. Per the
plan, that's deliberate: real OR1000 software (the guest kernel) owns
its own page tables and installs the resulting TLB entry with
`l.mtspr` from its own miss handler; this function's only job is to
notice there's no entry and raise the vector, exactly as safecpu.js's
`DTLBLookup`/`GetInstruction` do.

**Verification: bit-for-bit match against jor1k, again.**
`tests/m2_test.s` exercises: a general-SPR read/write round-trip; the
SR flag round-trip through `l.mtspr`/`l.mfspr` (including SR_FO being
force-set on every write, matching `cpu_set_flags`); `l.sys` and
`l.trap` each firing their real vector, with tiny handlers planted at
the *actual* vector addresses via a new `.org` assembler directive
(`tools/asm.py`) rather than inline with the code that triggers
them -- that's genuinely where the CPU jumps; and a full DTLB-miss
round trip: an `l.lwz` faults with no TLB entry installed, the
handler at `EXCEPT_DTLBMISS`'s vector installs a matching entry via
two `l.mtspr`s and returns with `l.rfe`, which (per real OR1000
semantics, `EPCR` pointed *at* the faulting instruction, not past it)
retries the same `l.lwz` -- which now succeeds and reads back the
value pre-stored at that physical address. `make c4or1k-m2-check`:
**exact match**, first real run.

Two things worth flagging, one a real fidelity note and one a
deliberate scope cut:

- **`EXCEPT_SYSCALL` and every other exception type save `EPCR`
  differently, and it matters for how a handler must behave.**
  `EXCEPT_SYSCALL` saves `(pc<<2)+4`, so a plain `l.rfe` naturally
  continues at the instruction *after* the trap. Everything else
  (`EXCEPT_TRAP` included) saves `(pc<<2)` — pointing *at* the
  faulting/trapping instruction — so a handler that wants to return
  past it (as `tests/m2_test.s`'s trap handler does) must bump `EPCR`
  by 4 itself before `l.rfe`, and a TLB-miss handler that does
  *nothing* to `EPCR` gets the retry-on-return behavior automatically.
  Both are exercised in the test program precisely because they're
  easy to get backwards.
- **TLB entry LRU bits are not checked at all** (real jor1k aborts —
  crashes the whole worker — if a guest ever sets them; nothing in
  this project's test programs does either). Not a page-table walk
  either, per above — that's the guest kernel's job starting at M4.
  Automatic tick-timer interrupt delivery is also still deferred:
  `TTMR`/`TTCR` are readable/writable SPRs now, but nothing fires
  `EXCEPT_TICK` on a schedule yet, since that needs the main-loop
  batching M3/M6 add.

**M3 — UART + launch script + console I/O.** Not started.
`run-c4or1k.sh` already exists (M0); M3 wires it to a real UART device
and the `con.c`-style non-blocking stdin pattern.

**M4 — boot loader.** Not started. `jorconsole/jor1k-sysroot/or1k/
vmlinux.bin` is already a decompressed raw flat binary (jorconsole's
own postinstall unpacked it) — no bzip2 or ELF parsing needed.

**M5 — 9P root filesystem (basefs.json only).** Not started.

**M6 — IRQ/timer tuning to reach a shell prompt.** Not started.

**M7 — stretch.** `c4lc -O`, `fs.json` overlay, optional virtio-block/
ATA. Framebuffer/keyboard stay out of scope.

## Related future work (not this project)

c4mp (the multiprocessor VM, `src/c4mp`) has no networking support.
Adding it would matter here too: a full Linux boot benefits from a
working network device, and jor1k itself emulates one (`ethmac.js`,
out of scope for c4or1k's own M0–M6 per the "headless console" plan
above). Noted for later, not blocking anything above.
