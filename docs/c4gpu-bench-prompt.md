# C4GPU benchmark brief — hand this to Claude Code on the GPU machine

Self-contained. It needs no files from this repo; everything it depends on is
inline below. Paste it whole, or open this file in VS Code and say "do this."
Background and the predictions being tested: `docs/c4gpu-design.md`.

---

## Brief

You are measuring whether it is worth porting a small stack-based VM (the C4
VM) to run thousands of independent instances on a GPU, one instance per
thread. **Do not build the port.** Build a five-part measurement kit, run it,
and report numbers. The decision comes after the numbers, and the predictions
being tested are stated below so they can be shown wrong.

Write CUDA, not WebGPU. This phase is about hardware physics and CUDA
measures it with the least API in the way.

Put everything in `bench/c4gpu/`, one `.cu` per benchmark, plus a `Makefile`
and a `run.sh` that runs all five and writes CSV to `results/<gpu-name>/`.
Print a human-readable summary table at the end of each run.

Run the whole kit on **every** NVIDIA GPU in the machine, separately, and
label results by device name. If two cards are present, do not average them —
the difference between them is part of the experiment.

## The machine being simulated

A VM with 32-bit words, a flat private byte-addressed arena per instance, and
these registers: `pc`, `sp`, `bp`, `a` (accumulator). Code and data live in
the same arena; every address is a byte offset into it. There are no host
pointers. Instructions are one 32-bit word, and some carry a second word of
operand. Semantics of the ones you need:

```
LEA n    a = bp + n*4               ADD      a = pop() + a
IMM n    a = n                      SUB      a = pop() - a
JMP addr pc = addr                  MUL      a = pop() * a
JSR addr push(pc+4); pc = addr      LT       a = pop() < a
BZ  addr pc = a ? pc+4 : addr       GT       a = pop() > a
BNZ addr pc = a ? addr : pc+4       EQ       a = pop() == a
ENT n    push(bp); bp = sp; sp -= n*4
ADJ n    sp += n*4
LEV      sp = bp; bp = pop(); pc = pop()
LI       a = mem32[a]               SI       mem32[pop()] = a
LC       a = sext8(mem8[a])         SC       mem8[pop()] = a & 0xff
PSH      push(a)                    EXIT     halt, status = peek()
```

The stack grows **down** from the top of the arena. `push(x)` is
`sp -= 4; mem32[sp] = x`. `pop()` is `x = mem32[sp]; sp += 4; x`.

Operand convention: the opcode word is fetched first and `pc` then points at
the operand word, so "`pc+4`" above means "skip the operand and continue."
`JSR` therefore pushes `pc+4`, and `BZ` falls through to `pc+4`.

## Baseline to beat (already measured, do not re-measure)

The same VM, natively on a Ryzen 5 3600, `gcc -O2`:

* **370M instructions/sec** on one core.
* **~2.2G instructions/sec** across all six.

That six-core figure is the number your results are judged against.

## The oracle program

Every VM instance runs this. It is 24 VM instructions per iteration, 480M
instructions total, and the answer is a 32-bit wrapped sum:

```c
int i, s;
i = 0; s = 0;
while (i < 20000000) { s = s + i; i = i + 1; }
/* s == 542894464 */
```

Hand-assemble it into the arena; it needs only
`ENT LEA PSH IMM SI LI LT BZ ADD JMP EXIT`. There is no `printf` in B3, so
finish with `s` on the stack and read it from the `EXIT` status, or leave it
in a known arena slot the host reads back. **Any run in which any instance
produces a value other than `542894464` is invalid and must be reported as a
failure, not tuned away.**

## Measured opcode distribution (use this, do not invent one)

From 2,106,946 real instructions of a Mandelbrot renderer on this VM:

```
PSH 22.18   LI 19.75   LEA 18.10   IMM 13.48   SI 7.45   ADD 3.85
BZ   3.37   MUL 3.05   SHR  2.82   SUB  1.23   JMP 1.17   LT  1.12
GT   0.97   SHL 0.89   DIV  0.23   ADJ  0.12   LC  0.11   (tail <0.2)
```

Derived facts you will need: ~2.07 memory accesses per VM instruction (1.36
code words + 0.71 data words), and 36.2% of instructions carry a second
operand word.

## B0 — device facts

`cudaGetDeviceProperties`: name, SM count, clock, **`l2CacheSize`**, memory
bus width and clock, `maxThreadsPerMultiProcessor`, total global memory,
compute capability. Then a grid-stride copy kernel over ≥1 GB for *achieved*
bandwidth, not the sticker number. Report both. Also report `nvcc --version`
and the driver version.

## B1 — scattered dependent-load rate

`N` threads, each pointer-chasing a randomized cycle inside its own private
arena of `S` bytes. The chain must be data-dependent so nothing prefetches:
`idx = arena[idx]`, with the cycle built on the host to visit every slot once.

Sweep `S ∈ {16KB, 64KB, 256KB, 1MB}` × `N ∈ {2^10, 2^12, 2^14, 2^16, 2^18,
2^20}` (skip combinations that exceed device memory, and say which you
skipped). Report dependent loads/sec and effective GB/s for each cell.

**This is the physics floor.** Divide loads/sec by 2.07 for the hard upper
bound on VM instructions/sec before any dispatch cost. If the best cell is
under ~4.5G loads/s, say so prominently — the project is capped below the CPU
and the rest of the kit is academic.

## B2 — the divergence tax

A kernel whose body is a 66-way `switch` over short opcode-like bodies (make
the bodies resemble the real ones: a couple of ALU ops and zero to two memory
accesses). Two modes:

1. each lane draws its opcode from the distribution above — the realistic case;
2. every lane takes the same opcode — the uniform floor.

Report the ratio. **Prediction: ~9.15 distinct opcodes per 32-lane warp, and a
tax well under 9x because the fetch/decode prologue is shared.** If the tax is
much worse than the distinct-path count implies, the switch is compiling
badly — check the SASS and say so, because it means the real port needs a
different dispatch shape.

## B3 — a minimal VM (the money number)

~200 lines. One thread per VM instance, private arena, only the opcodes the
oracle program uses. No traps, no devices, no protected mode, no I/O. Run in
bounded quanta of `Q` instructions with state written back to a per-VM control
block at the boundary and the kernel re-dispatched, because that is what the
real port must do.

Sweep `N ∈ {1K, 4K, 16K, 64K}` × `ARENA ∈ {64KB, 256KB, 1MB}` × `Q ∈ {1K, 10K,
100K}`. Report aggregate instructions/sec, per-VM instructions/sec, device
memory used, and the correctness check for every cell.

**Then plot aggregate rate against `N * ARENA` and look for a cliff where it
crosses `l2CacheSize` from B0.** That cliff is the central prediction of the
whole study. If it is there, the sweet spot is "as many VMs as fit in L2." If
there is no cliff, something else is the bottleneck — find out what
(`nsight-compute` on the best and worst cells) and report it, because that is
a more interesting result than the cliff.

## B4 — the park-on-I/O tax

Extend B3 so that one lane in `K` must stop every `I` instructions and wait
for a host round-trip before resuming (write a request to its control block,
set a status word, return from the quantum; host services it between
dispatches). Sweep the I/O rate across several orders of magnitude and report
the throughput curve. This prices the design's one genuinely new mechanism.

## Go / no-go, stated in advance

> **B3 aggregate must exceed 6.6G instructions/sec — 3x the 2.2G six-core CPU
> baseline — on at least one card.**

Report the verdict plainly. A result between 2.2G and 6.6G is the awkward
outcome and must be reported as awkward, not rounded up. Do not tune the
benchmark to reach the threshold; if you optimize something, report both the
naive and the optimized number and say what changed.

## What to report back

* One CSV per benchmark per device, in `results/<gpu-name>/`.
* A summary table: B0 facts, best cell of B1, B2 ratio, B3 best and worst
  cells with correctness status, B4 curve.
* The L2 cliff plot (or a statement that there wasn't one).
* Each prediction above marked **confirmed** or **wrong, by this much**.
* Anything surprising, at whatever length it deserves.

## Out of scope

No WGSL, no WebGPU, no port, no changes to any existing VM source, no
multi-GPU work distribution, and no attempt to make one VM instance run
faster — the instruction stream is a serial dependency chain and that is
known to be a dead end. Throughput across independent instances is the entire
question.
