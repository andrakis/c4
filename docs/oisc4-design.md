# OISC4 — the One Instruction Set Computer for C4

`src/oisc4/oisc4.c` runs `.c4r` images — anything compiled by c4cc **or c4lc**
— on a virtual machine with exactly one instruction:

```
add [addr:Source], [literal:Add], [addr:Dest]
```

Each instruction reads a word from `Source`, adds the literal `Add`, writes
the result to `Dest`, and latches three flag registers (`EQ0`, `LT0`, `GT0`).
Control flow, arithmetic, calls, memory, and IO all reduce to that.

```
gcc -O2 -o oisc4 src/oisc4/oisc4.c
./oisc4 src/tests/hello.c4r                # yello
./oisc4 c4.c4r src/tests/hello.c           # the c4 compiler, compiling and
                                           # running hello.c, inside OISC
```

This replaces the 2023-era experiment in `src/tests/oisc-min.c` /
`oisc-c4.c` / `oisc-asm.c`. That attempt forked the c4.c *frontend* and
emitted OISC inline next to every C4 opcode, using absolute host pointers as
OISC addresses. It never reached full compatibility: the fork had to re-fix
every frontend bug forever, several branch-patch offsets were marked
`TODO: ensure offsets correct`, and host-pointer addressing made images
non-relocatable and SIMD-hostile. The rewrite keeps the two good ideas from
that experiment — the word-mode flags (`WR`/`WW`) for byte access, and
flag registers holding a *jump displacement* rather than a boolean — and
changes everything else.

## 1. The design in one paragraph

Do not touch the compiler. Translate **compiled `.c4r` images** instead:
load code + data, walk the code segment twice (sizes, then emission), and
expand every C4/c4m opcode into a fixed 1–11 instruction OISC sequence.
The `.c4r` patch table says *exactly* which operand words are code
addresses, which are data addresses, and which are plain integers, so
translation needs no guessing and no symbolic assembler. Everything c4cc
or c4lc can compile — including the self-hosted compilers themselves —
then runs on OISC unchanged, with c4lc's C99 support and `-O` optimizer
for free.

## 2. The machine

One flat byte arena; **every OISC address is a byte offset into it**. There
are no host pointers anywhere in VM state — a deliberate requirement for
the SIMD/GPU investigation in §7 (N tasks = N self-contained arenas), and
the property whose absence sank the old attempt.

| Range | Contents |
|---|---|
| 0 – 127 | registers (word slots): `Z PC EQ0 LT0 GT0 WR WW A SP BP T0 R0 R1 R2` |
| 256 – 511 | ports: `PUTC`, `SYSCALL`, math device `X Y OP V` |
| 4096 – | startup stub, translated code, data segment, argv, heap |
| top | stack, growing down (4 MB reserved) |

Cycle semantics (`vm_run`):

```
src,add,dst = M[pc], M[pc+8], M[pc+16];  PC = pc + 24
v = (src is port) ? port_read(src) : (WR ? signed byte : word) at src
v += add
(dst is port) ? port_write(dst, v) : (WW ? byte : word) store at dst
EQ0/LT0/GT0 = (v==0 / v<0 / v>0) ? 24 : 0
```

Details that matter:

* **Registers are ordinary memory.** Reading/writing them needs no special
  case; a write to `PC` (offset 8) *is* the jump, because the fetch loop
  rereads `PC` each cycle. `PC` is advanced before the write phase so a
  jump wins over the increment.
* **`PC = 0` halts.** Register `Z` lives at 0 and is never a valid
  instruction address; the `EXIT` syscall just writes 0 to `PC`.
* **Flags hold 24** — one instruction size — not 1. So a conditional
  branch is a single add: `EQ0 + L -> PC` lands on `L` (a jump to the
  fall-through) or `L+24` (a jump to the target).
* **`WR`/`WW` word-mode registers** select byte access for `LC`/`SC`.
  The translated sequence sets the mode, does the one dereference, and
  clears it. Flags are clobbered by the clear, which is safe: C4 never
  reads OISC flags — each `BZ` expansion re-observes `A` itself.
* **Self-modifying code is the addressing mode.** OISC has no indirect
  operand, so "load through a pointer" is two instructions: the first
  writes the pointer's value into the source/dest *field* of the second.
  Every placeholder is written at runtime immediately before use, so the
  code is re-entrant in practice (each call re-patches before executing).

## 3. Devices

* **PUTC (256)** — writes emit the low byte to stdout.
* **SYSCALL (264)** — the written value is a *c4m opcode number*; results
  are read back from the same port. The translated code snapshots `SP`
  into `R0` first, so the device reads arguments straight off the VM
  stack, exactly like c4m's syscall opcodes do. `PRTF` gets its argument
  count in `R1` (the translator peeks at the operand of the `ADJ` that
  always follows `PRTF`, the same trick `c4m.c` uses at runtime).
* **MATH (272–296)** — write `X`, write `Y`, write `OP` (computes), read
  `V`. Sixteen operations, numbered `c4_opcode - OR`. This is the escape
  hatch for everything an adder can't do cheaply (mul/div/shift/compare);
  a "purer" OISC could synthesize these from adds, at enormous cost, but
  the point here is running real programs.

Pointers crossing the device boundary are arena offsets and get translated
(`vm_m + off`) exactly there and nowhere else. `PRTF` therefore cannot
forward to host `printf` (its `%s` arguments are offsets), so the device
implements the format walk itself, mirroring native c4m/glibc behavior:
conversions without an `l` modifier see their low 32 bits, `%s` translates,
`%p` prints the arena offset (the one visible output difference from c4m).
`malloc`/`free`/`realloc` are a first-fit free list inside the arena —
which also fixes c4m's broken `RALC` (docs/internals.md Part 3.6) for
programs running under OISC. `__opcode("NAME")` (`_OPC`) does the real
name-table lookup, same 4-char case-insensitive match as c4m.

Two guards in the store path turn silent corruption into diagnostics:

* **Null writes halt.** A guest storing through a null pointer would land
  on register Z and quietly break the "Z is always zero" invariant that
  every expansion depends on (c4m segfaults on the same bug; found the
  hard way when c4m.c4r's jailbreak wrote to address 0).
* **The code-write gate.** Guest stores into the code region cannot work
  in general — the code there is OISC, not C4 words — but the one
  self-patching idiom the repo actually uses,
  `*f++ = __opcode("JMP"); *f = (int)&target;` (mandel.c,
  test_timekeeping.c, load-c4r.c), is emulated: the JMP-opcode write is
  latched, the target write rewrites the instruction at `f` into a real
  OISC jump `Z + target -> PC`. Both `&f` and `&target` arrive already
  OISC-mapped via the patch table, so this composes exactly. Anything
  else warns once and is ignored. Distinguishing guest stores from the
  translator's *own* self-patch stores (which also target code) is by
  distance: legitimate self-patches always write within a few
  instructions of the writer (≤ 72 bytes; the gate cuts at 240).

## 4. Translation

Two passes over the code segment (which is 1-based: c4's `*++e` emitter
never uses word 0):

1. **Size pass** — every opcode has a fixed expansion size, so this just
   fills `tr_map[c4_word_index] -> arena address` and totals the code size.
2. **Layout** — data, argv and heap are placed after the code; the
   data-resident patches (`DCODE` for switch jump tables and function
   pointers in globals, `DDATA` for data-to-data pointers) are applied
   *through the map*, so a jump table entry becomes a translated-code
   address directly.
3. **Emit pass** — expansions reference `tr_map` for code targets
   (`JMP`/`BZ`/`BNZ`/`JSR`/`IMM &func` — all identified by `CODE`
   patches) and rebased data addresses (`DATA` patches).

A startup stub is emitted before the translated code: it calls each
constructor with a 0 argument, pushes `argc`/`argv` (strings copied into
the arena), calls the entry point, runs destructors, then pushes the
return value and issues `EXIT` — the same frame `load-c4r.c` fabricates.

Representative expansions (sizes in OISC instructions):

| C4 | n | scheme |
|---|---|---|
| `LEA k` | 1 | `BP + 8k -> A` |
| `IMM v` | 1 | `Z + v -> A` |
| `JMP L` | 1 | `Z + map[L] -> PC` |
| `BZ L` | 4 | observe `A`; `EQ0 + here -> PC`; jump fall-through; jump `L` |
| `PSH` | 3 | `SP-8`; patch dst of next; `A -> [SP]` |
| `JSR L` | 4 | push return address (self-patched), `Z + map[L] -> PC` |
| `JSRI g` | 4 | push return; `M[g] + 0 -> PC` (the global *is* the source operand) |
| `LI` | 2 | patch src of next; `[A] -> A` |
| `LC` | 4 | as `LI` bracketed by `WR=1` / `WR=0` |
| `SI` | 5 | pop target via `T0`, patch dst, `A -> [T0]` |
| `SC` | 11 | as `SI` in byte mode, then reload `A` as signed byte (c4's `a = *(char*)... = a`) |
| `ENT n` | 5 | push `BP`; `BP=SP`; `SP -= 8n` |
| `LEV` | 7 | `SP=BP`; `BP=[SP]`; `T0=[SP+8]`; `SP+=16`; `T0 -> PC` |
| `OR..MOD` | 6 | pop into math `X`, `A` into `Y`, write `OP`, read `V` |
| syscalls | 2–4 | `SP -> R0` (+argc to `R1` for PRTF); opcode to port; read result |

Measured expansion: ×5–7 code size in words; hello runs in 75 cycles,
`tests.c4r` (the full c4m test suite) passes bit-identically.

The extended c4m opcodes follow `c4l.c`'s compatibility contract
(docs/internals.md §6.5): `JSRI JSRS JMPA _JMP _ADJ PUTC PUTS MCPY RALC
STRC TIME C4CY OPCD` are real; `INFO` returns 0 — *no capabilities* — so
well-written images (which probe `__c4_info()` before using traps,
signals, or floats) stay on paths OISC supports; `OPCD` leaves its
argument in the accumulator (the missed-probe convention). Trap machinery
(`ITH`/`TLEV`/cycle interrupts) is stubbed to 0: C4KE cannot run, which
is expected — even plain c4 can't run it.

## 5. Verification and measurements

`make test-oisc4` compares against `./c4m load-c4r.c -- <image>` with
c4m's bare-metal trap chatter filtered (`Trap type ... / missed a trap`,
which bare c4m prints when a constructor probes for a kernel):

* Bit-identical: `hello factorial multifun test-order test-ptrs test_basic
  test_malloc test_static test_continue test_args test_exit cycles
  test_fread test_printloop test_crash test_vprintf test_float` (3365
  float cases) `test_switch tests.c4r rps` (with input) and `mandel`
  (modulo the measured milliseconds in its final line).
* `test_printf`: identical except `%p` (arena offset vs host address).
* `fun_with_ptrs`: **c4m and c4-via-c4l themselves segfault** on this
  image (rc 139, it plays with raw addresses); OISC4 survives further.
* `test_customop` / `test_signal` / `test_timekeeping`: need real
  traps/signals/kernel opcodes — out of contract, same as c4l.
  (test_timekeeping spins probing opcode 128 under bare c4m too; that
  probe loop is what produced 1.3 GB of trap chatter in five minutes
  during testing.)
* The acid tests:
  * `./oisc4 c4.c4r src/tests/hello.c` — the self-hosted c4 compiler
    compiles hello.c and executes it on its own inner VM, entirely on
    the one-instruction machine.
  * `./oisc4 -m 256 c4sp.c4r src/c4sp/lisp/c4r-roundtrip.lisp
    src/tests/hello.c4r` — the whole c4sp Lisp interpreter (GC and all)
    runs under OISC and reports `roundtrip identical`.

`make test-oisc4-nested` pins the nesting chains (`oisc4-lc.c4r` is the
c4lc `-O` build of oisc4 itself):

```
c4m   -> oisc4-lc.c4r -> hello.c4r      # OISC as a c4m guest
c4    -> c4l.c -> oisc4-lc.c4r -> hello # OISC under plain c4
oisc4 -> oisc4-lc.c4r -> hello.c4r      # OISC on OISC
```

Speed (this machine, one run, c4sp roundtrip workload): native oisc4
0.81s vs native c4m 0.14s — **5.8x slower** than a mature two-operand
interpreter, for a machine with one instruction. hello is 75 OISC
cycles; mandel ~5.7M.

### Known limitation: the C4 jailbreak

`c4m.c4r` cannot run: `c4m_main` unconditionally calls
`c4_invoke_stub()`, which reads its own return address off the stack and
pattern-scans backwards for C4 opcode words (`get_calling_address`,
c4m.c:1114). Under binary translation the code in memory is OISC
triples, the scan fails, and c4m exits down its error path. Anything
that introspects raw C4 code words at runtime, or generates C4 code and
jumps to it (`C4IV`), is fundamentally outside a static translator's
contract — that is what the interpreting loaders (`c4l.c`, c4m) are
for. The JMP-gate idiom (§3) is deliberately the only exception.

## 6. Why the old attempt couldn't get there

Post-mortem of `src/tests/oisc-{min,c4,asm}.c` for the record:

1. **Absolute host pointers as OISC addresses.** `VM_CycleC4` dereferenced
   real pointers, so emitted code depended on where malloc happened to
   place buffers. Un-dumpable, un-relocatable, un-SIMDable.
2. **Frontend fork.** Every c4.c parsing quirk had to be maintained in the
   fork, and OISC emission was interleaved with parsing, so backpatching
   had to be done in *both* instruction streams (`*d = e+3; *o = oisc4_e+3;
   // TODO: ensure offsets correct`).
3. **Known-suspect expansions.** `OISC4_SI` carried a double dereference
   marked `TODO: needed?`; `JSRS`'s placeholder chain overwrote the same
   `d` twice.
4. **No ground truth.** With no reference output for the same binary, every
   miscompile looked like a VM bug and vice versa. Translating `.c4r`
   images gives a bit-for-bit oracle (`c4m load-c4r.c`) for every test.

## 7. SIMD / GPU feasibility (investigation only — no code yet)

The motivating question: can many OISC tasks run their add step (and the
flag compares) in parallel — SIMD lanes or GPU threads?

**What the current design already provides.** Each VM is one contiguous
arena plus nothing else; all addresses are arena-relative; the cycle is
branch-poor and uniform (2 loads + optional byte-select, add, store,
3 flag stores). That is exactly the shape SIMD wants. `vm_run` is ~20
lines with two data-dependent branches (port test on src, port test on
dst) plus the byte-mode selects, which can become masks.

**The lane-parallel model** (N independent programs, one lane each — the
c4ke-ish "several tasks run their add step in parallel" goal):

* AVX-512: 8 lanes of 64-bit. Fetch = `vpgatherqq` on `base[lane] +
  pc[lane] + {0,8,16}` (3 gathers); operand read = 1 gather; store =
  `vpscatterqq`; flags = 3 compares + blends, branchless. The catch:
  gather/scatter on Zen4/Ice Lake runs ~1 element/cycle-ish — expect
  little or no win over 8 well-pipelined scalar interpreters on 8 cores,
  *unless* lanes are cache-resident. Worth measuring, not assuming.
* GPU: one thread per VM, arenas in device memory. Uncoalesced by nature
  (each VM chases its own PC), but GPUs tolerate that with enough
  occupancy; 20–30k VMs at ~1 instr/thread/cycle-equivalent would dwarf
  host throughput. The real cost is **divergence at the ports**: any lane
  touching SYSCALL must fall back to host (or a device-side ring buffer
  serviced asynchronously). PUTC and MATH can stay device-side (MATH is
  pure ALU; PUTC appends to a per-VM output buffer).
* Self-modifying code is a *non-issue* in this model: each VM only writes
  its own arena, and there is no instruction cache to invalidate — code
  is data.

**The instruction-parallel model** (one program, parallel adds) is much
weaker: consecutive OISC instructions are dependency-chained through the
self-patching idiom (instruction k writes a field of instruction k+1), so
intra-task ILP is near zero *by construction*. Any SIMD story should be
lane-parallel across tasks, not across instructions within a task.

**What would need to change for a SIMD backend** (all cheap, none done):

1. Give lanes a uniform "waiting on host" state: a lane that writes
   SYSCALL parks (PC frozen) until the host services it; the batch loop
   keeps stepping the other lanes. (Design: one extra per-lane status
   word; the scalar VM needs no change.)
2. Replace the port-range branches with arithmetic masks
   (`in_port = (u >= 256) & (u < 512)` as a lane mask).
3. Optional: pack flags into one word (bit 0/1/2) instead of three words
   to cut stores from 3 to 1; the branch expansion changes accordingly.
4. Byte access: masked load + sign-extend under the `WR` lane mask —
   already branchless-friendly.

**Recommendation.** Prototype the lane model on CPU first: a
`vm_run_batch(int n)` that steps N scalar VMs round-robin from one loop.
That alone exposes the two design constraints that matter (park-on-port
and mask-friendly branches) and gives a baseline; only then decide if
AVX-512 gathers or a CUDA/WebGPU port beat N× scalar. The current
architecture requires no changes to *state* for any of these — only the
stepper is replaced — which was the point of banning host pointers.

## 8. Files and targets

```
src/oisc4/oisc4.c        VM + translator + loader, single file, c4 dialect
src/oisc4/test-oisc4.sh  the comparison ladder
make oisc4               native build
make test-oisc4          output-compare ladder vs c4m
make oisc4-lc.c4r        c4lc -O build of oisc4 itself
make test-oisc4-nested   the three nesting chains
```

Flags: `-d` trace (with `-c N`, only the last 100 cycles before the
limit), `-s` disassemble and exit, `-v` stats, `-m megs` arena size,
`-c N` cycle limit.

The source keeps to the c4cc/c4lc-compilable dialect (single `int` word
type — but `#ifndef C4CC`, since those toolchains' headers remap `long`
to `int` and a `#define int long long` would expand to `int int` — no
structs, locals-at-top, `while`/`if` only, no host library calls beyond
the c4 builtin set). That is what makes `oisc4-lc.c4r` possible: a One
Instruction Set Computer compiled by the Lisp compiler, interpreted by
the stack machine it is emulating — or by itself.
