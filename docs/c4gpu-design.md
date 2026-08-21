# C4GPU — the C4 VM on graphics hardware

**Status: investigation only — no code yet.** This document is the feasibility
study, the arithmetic behind it, and a measurement kit to be run on real
hardware before a line of the port is written. Everything below marked
*measured* was measured; everything marked *estimate* is envelope arithmetic
and is exactly what §8 exists to replace.

The short version: the interesting form of "c4 on a GPU" is **N independent
VMs, one per GPU thread**, it is *architecturally* almost free because
`src/oisc4` and `src/c4bb` already did the hard part, and the expected speedup
over a six-core desktop is **somewhere between 1x and 3x** — not the 100x the
phrase "GPU-accelerated" suggests. Whether that is worth building is a
measurement, not an opinion, and §8 is how to take it.

## 1. Four things the phrase could mean

| Interpretation | Verdict |
|---|---|
| **A. N independent VMs, one per GPU thread** | Feasible. The only one that can win. This document is about A. |
| **B. One c4 program, running faster** | Not feasible. See below. |
| **C. c4 as host, GPU as a device** | Feasible, small, and a different project. See §10. |
| **D. C4KE/C4IX tasks as GPU threads** | Feasible in principle, pointless in practice. See §10. |

**B is worth dismissing explicitly**, because it is what people mean when they
ask. The C4 instruction stream is a serial dependency chain through `a`, `sp`
and `bp`: `PSH` writes the slot the next `ADD` reads, `LEA` feeds the `LI` that
feeds the `SI`. There is no intra-program parallelism to extract, and a single
GPU lane is roughly an order of magnitude *slower* than a Ryzen core at
chasing dependent loads. This is the same conclusion `docs/oisc4-design.md` §7
reached about instruction-parallelism in OISC, for the same reason, and it is
structural rather than an artifact of any particular VM: anything that helped
here would be a data-parallel *compiler*, not a c4 VM.

## 2. The baseline to beat

Measured on this box (AMD Ryzen 5 3600, 6 cores, `gcc -O2`), running a
20,000,000-iteration integer loop compiled to exactly 24 VM instructions per
iteration — 480M instructions total, instruction count taken from `c4m -s`:

| VM | Dispatch | Word | Time | Rate |
|---|---|---|---|---|
| `./c4m` | if-else chain | 64-bit | 1.272 s | **377M instr/s** |
| `./c4mp` | switch (`src/c4mp/vm.c:262`) | 64-bit | 1.290 s | 372M instr/s |
| `./c4m32` | if-else chain | 32-bit | 1.316 s | 365M instr/s |

Note the 32-bit build costs ~3%, which matters because the GPU port is
necessarily 32-bit (§4). Note also that this contradicts the "~4.4M
VM-instr/sec ceiling" quoted in `docs/c4or1k-design.md:603` by two orders of
magnitude — that figure is a *hosted* c4m, not a native one. The honest
number for a native c4m on this machine is **~370M instr/s per core, ~2.2G
across six**, and that is what a GPU has to beat to be worth the trouble.

The program used, and its answer, are the GPU's correctness oracle too:

```c
int main() {
  int i, s;
  i = 0; s = 0;
  while (i < 20000000) { s = s + i; i = i + 1; }
  printf("s=%d\n", s);
  return 0;
}
```

`s=542894464` — the 32-bit wrapped sum. Every VM on the GPU must produce that
exact word or the run is discarded.

## 3. What the repo already provides

This is the surprise, and the reason this document exists at all. The port is
mostly a *port of things that have already been written twice*.

* **Arena addressing.** `src/c4bb/sim/arena.js` is a flat `Int32Array` in which
  every guest address is a byte offset — no host pointers in VM state
  anywhere. `src/oisc4` established the same convention first, and
  `docs/oisc4-design.md:246` names host pointers as precisely what made the
  2023 OISC attempt "un-dumpable, un-relocatable, un-SIMDable." That is the
  single hardest requirement for GPU execution and it is already satisfied,
  twice, in two languages.
* **32-bit is a shipped configuration.** `c4cc32`, `c4m32`, `c4sp32`,
  `c4rlink32`, `make c4bb-32bit`, and fifteen 32-bit images in
  `src/c4bb/images/` diffed byte-for-byte against native `c4m32`. GPUs have no
  64-bit integer worth using; without this the port would begin by forking the
  toolchain.
* **The syscalls are already guest-side.** `src/c4bb/fw/fw.c` implements
  `MALC`, `FREE`, `RALC`, `PRTF` and `STRC` as *firmware*, reached through
  vector latches at `0x20`–`0x30`. `printf` formatting happens inside the
  arena and only finished bytes reach the UART. On a GPU this is worth more
  than it sounds: the single most frequent "syscall" in real C4 code needs no
  host round-trip at all.
* **All host interaction is already one 256-byte window.** `DEV_BASE`
  `0x100`–`0x1FF` in `src/c4bb/sim/devices.js`: UART, clock, cycle counter,
  disk, power, heap bounds. Everything a lane cannot do for itself is a store
  to a known address range, which is exactly the shape a park-and-service
  protocol needs (§5.1).
* **Bounded execution already exists.** `C4MP_QUANTUM` in `src/c4mp/c4mp.h`
  and `Turbo.run(maxCycles)` in `src/c4bb/sim/turbo.js`. Browsers and drivers
  will kill a compute kernel that runs for seconds; the port must dispatch in
  bounded batches and resume, and both engines are already written that way.
* **Self-modifying code is a non-issue.** Each VM writes only its own arena
  and there is no instruction cache to invalidate — code is data, which is
  true of C4 everywhere and happens to be exactly what SIMT wants.

## 4. Why it cannot descend from c4m or c4mp

`c4m.c` and `src/c4mp` keep real host pointers in machine state — `int *pc`,
`int *sp`, `int *bp` in `struct c4_cpu`, and `c4.c:527`'s `PRTF` hands guest
words straight to the host `printf` as a format pointer. Neither can run on a
device that has no host address space, and neither can be *made* to without
becoming the arena machine that c4bb already is.

So the lineage is:

```
src/oisc4  (arena convention, no host pointers)
 └ src/c4bb  (32-bit, arena, MMIO devices, firmware syscalls, quantum)
    └ src/c4gpu  (the same machine, N times, in a compute kernel)
```

`c4m`/`c4mp` remain the *oracle*, not the ancestor: they are what the GPU's
output gets diffed against, the same role native `c4m32` plays for c4bb.

## 5. The lane-parallel machine

One thread per VM. Each VM owns a private, contiguous arena; the device
allocation is `N * ARENA_BYTES` plus a per-VM control block. State that lives
in `struct c4_cpu` today lives in thread registers for the duration of a
quantum and is written back to the control block at the boundary — which is
already how `c4_run` is structured (`src/c4mp/vm.c:19`), and is not a
coincidence: it was written that way so a second processor was expressible.

### 5.1 Park-on-I/O

The one genuinely new mechanism. A lane that touches `DEV_BASE`–`DEV_END`
cannot service itself, so it:

1. writes the request (address, value, direction) to its control block,
2. sets its status word to `PARKED`,
3. stops advancing its PC, and returns from the quantum.

The host drains parked lanes between dispatches, fills in results, and clears
the status. Every other lane keeps running. This is `docs/oisc4-design.md` §7
item 1, specified there and never built.

The cost is a full dispatch round-trip per I/O — hundreds of microseconds in
WebGPU, tens in CUDA. That is fine for `PUTC` at human rates and fatal for a
program that does I/O in a loop, which is the real reason interpretation D
(kernel tasks on GPU) fails: kernels trap constantly.

### 5.2 Arena sizing is the binding constraint

c4bb defaults to a 32 MB arena. At 20,000 VMs that is 640 GB, so the default
is not merely large, it is disqualifying. The sweep in §8 exists mostly to
find how small an arena can get:

| Arena | 4,096 VMs | 16,384 VMs | 65,536 VMs |
|---|---|---|---|
| 64 KB | 256 MB | 1 GB | 4 GB |
| 256 KB | 1 GB | 4 GB | 16 GB |
| 1 MB | 4 GB | 16 GB | 64 GB |

An 8 GB 3060 Ti therefore supports roughly *16K VMs at 256 KB* or *64K at
64 KB*, and a 16 GB 5080 twice that. Whether a useful image fits in 64 KB is
an open question — `hello32.c4r` is 230 bytes but `c4ix32.c4r` is 127 KB
before it allocates anything — and it interacts with the L2 question below in
a way that makes small arenas doubly valuable.

### 5.3 Byte access, and why lanes must not share

`LC`/`SC` are byte operations on what will be a `u32` storage buffer:
shift-and-mask to read, read-modify-write to write. That is correct and
lock-free *only* because each arena is lane-private. The moment two lanes
share an arena — an SMP guest, a shared heap — every byte store needs an
atomic, and the cost lands on `SC`, which real C4 code executes constantly
(string handling). Lane-private arenas are not an optimization here; they are
the reason the naive implementation is correct.

### 5.4 Ragged completion

VMs exit at different times, so a naive one-thread-per-VM grid decays to a few
stragglers holding whole warps. The fix is standard — persistent threads
pulling VM indices from a work queue — but it should be built in from the
start, because the benchmark in §8 will otherwise measure the stragglers.

## 6. The arithmetic

### 6.1 A measured opcode distribution

From `./c4mp -d mandel.c4r`, 2,106,946 instructions — a real workload, all
integer, no I/O in the inner loop:

| Op | Share | | Op | Share |
|---|---|---|---|---|
| `PSH` | 22.18% | | `MUL` | 3.05% |
| `LI`  | 19.75% | | `SHR` | 2.82% |
| `LEA` | 18.10% | | `SUB` | 1.23% |
| `IMM` | 13.48% | | `JMP` | 1.17% |
| `SI`  |  7.45% | | `LT`  | 1.12% |
| `ADD` |  3.85% | | `GT`  | 0.97% |
| `BZ`  |  3.37% | | `SHL` | 0.89% |

Five opcodes are 81% of dynamic instructions; nine are 91.5%. The tail
(`DIV`, `ADJ`, `PRTF`, `LC`, and the entire syscall and trap block) is under
0.6% combined.

### 6.2 Divergence is smaller than it looks

The obvious objection to a 66-way `switch` on SIMT hardware is that a warp
serializes over every distinct opcode its lanes chose. With 32 lanes drawn
independently from the distribution above:

```
E[distinct opcodes among  8 lanes] = 5.03
E[distinct opcodes among 16 lanes] = 7.05
E[distinct opcodes among 32 lanes] = 9.15
```

Not 32, and not the 15–20 a uniform distribution would give — the skew works
in our favour. Nine of sixty-six paths taken per warp step, and the taken
paths are two to five instructions each, against a *uniform* fetch/decode/PC
prologue that all lanes share. The dispatch tax is real but is a small
constant, not the catastrophe the opcode count suggests. **It is also the
single least trustworthy number in this document** — it assumes lane PCs are
independent, which they are when N VMs run N different programs and are not
when they run the same program on different data. Benchmark B2 measures it
rather than assuming it.

### 6.3 Memory traffic is the actual ceiling

Same distribution, counting guest memory operations per instruction (`PSH`=1
store, `LI`/`LC`=1 load, `SI`/`SC`=2, ALU=1 stack pop, `LEA`/`IMM`=0):

```
mean guest data accesses / instruction = 0.711
two-word (operand-carrying) instructions = 36.2%
=> ~1.36 code words + 0.71 data words = ~2.07 memory accesses per instruction
```

Every one of those is uncoalesced by construction: 32 lanes chase 32
unrelated PCs into 32 unrelated arenas. Assume a 32-byte sector per access and
no cache hits, and each guest instruction costs ~66 bytes of memory traffic:

| Device | Bandwidth | Ceiling at 66 B/instr | At 30% of peak |
|---|---|---|---|
| Ryzen 5 3600 (6 cores) | — | — | **2.2 G instr/s** *(measured)* |
| RTX 3060 Ti | ~448 GB/s | 6.8 G instr/s | ~2.0 G instr/s |
| RTX 5080 | ~960 GB/s | 14.5 G instr/s | ~4.4 G instr/s |

*Estimates.* 30% of peak is a deliberately pessimistic figure for fully
scattered access; the true value is what benchmark B1 reports.

So the honest expectation is: **a 3060 Ti roughly ties this desktop, and a
5080 beats it about twofold.** That is the headline finding, and it is not an
argument for building the port on performance grounds alone.

### 6.4 The one thing that could change the answer

The table above assumes DRAM. It stops being true the moment the working set
fits in L2, and modern L2s are large enough that this is not a fantasy:
Ampere GA104 carries ~4 MB, and the Ada/Blackwell parts carry tens of MB. At
64 KB per arena, *hundreds* of VMs are L2-resident on a 3060 Ti and possibly
*a thousand or more* on a 5080 — and L2 bandwidth is several times DRAM's.

That gives a concrete, falsifiable prediction: **aggregate instructions/sec,
swept over arena size and VM count, should show a cliff where
`N * ARENA_BYTES` crosses L2 capacity.** If the cliff is where theory says it
is, the model is sound and the sweet spot is "as many VMs as fit in L2, and
not one more." If there is no cliff, something else is the bottleneck and the
estimates in §6.3 are wrong in an interesting way. Either outcome is worth the
afternoon.

## 7. The OISC alternative, which is not obviously worse

`src/oisc4` measured ×5–7 code expansion and **5.8x slower than c4m**
natively (`docs/oisc4-design.md:224`). On a CPU that settles it. On SIMT it
does not, because the two engines pay opposite costs:

| | c4 switch VM | OISC4 |
|---|---|---|
| Instructions per unit of work | 1 | 5–7 |
| Distinct paths per warp step | ~9.15 | ~1 |
| Cycle shape | 66-way switch | 2 loads, add, store, 3 flag stores |
| Memory pattern | irregular | uniform, predictable |

OISC pays 6x in instruction count to buy near-total warp uniformity and a
regular access pattern. Whether that trade wins depends on numbers neither of
us has, and it is genuinely not predictable from the CPU result — which is
the whole reason to measure both. If the port happens, both engines get
benchmarked, and the CPU's answer is not evidence about the GPU's.

## 8. The measurement kit

This is the deliverable to run on real hardware **before** any porting. It
deliberately does not build the VM: it measures the three physical quantities
that determine whether the VM is worth building, then builds the smallest
possible VM to check the prediction.

**The brief to hand to whoever runs it is `docs/c4gpu-bench-prompt.md`** —
self-contained, needs no files from this repo, and carries the predictions
below inline so they can be marked confirmed or wrong.

Target: a machine with an NVIDIA GPU and CUDA. CUDA rather than WebGPU
because this phase is about *hardware physics*, and CUDA measures it with the
least API in the way; §9 covers what changes when the real port targets
WebGPU.

Devices to run it on: **RTX 3060 Ti** (GA104, 38 SMs, 8 GB GDDR6) and
**RTX 5080** (GB203, 16 GB GDDR7). Run every benchmark on both; the
generational difference in L2 capacity is precisely what §6.4 predicts will
matter, so two devices is not redundancy, it is the experiment.

### B0 — device facts

`cudaGetDeviceProperties`: SM count, clock, **`l2CacheSize`**, memory bus
width and clock, `maxThreadsPerMultiProcessor`. Then an achieved-bandwidth
kernel (grid-stride copy over ≥1 GB) for real GB/s rather than the sticker
number. Everything downstream is quoted as a fraction of these.

### B1 — scattered dependent-load rate

The physics floor. `N` threads, each pointer-chasing a randomized cycle
inside its own private arena of `S` bytes; the chain is data-dependent so
nothing prefetches. Sweep `S ∈ {16 KB, 64 KB, 256 KB, 1 MB}` and
`N ∈ {2^10 … 2^20}`. Report dependent loads/sec and effective GB/s.

**Divide by 2.07** (§6.3) and you have the hard upper bound on VM
instructions/sec, before any dispatch or divergence cost. If B1's answer is
below ~4.5 G loads/s the project is already capped under the six-core CPU and
the rest of the kit is academic.

### B2 — the divergence tax

A kernel whose body is a 66-way switch over short opcode-like bodies. Two
modes: every lane draws its opcode from the measured distribution in §6.1
(the realistic case), and every lane takes the same opcode (the uniform
floor). The ratio is the divergence tax. Prediction: ~9.15 distinct paths per
warp, and a tax well under 9x because the prologue is shared. If the measured
tax is much worse than the distinct-path count implies, the switch is
compiling badly and the port needs a different dispatch shape.

### B3 — a minimal VM, which is the money number

~200 lines of CUDA. One thread per VM, private arena, the ~20 opcodes the
oracle program in §2 actually executes (`LEA IMM JMP JSR BZ ENT ADJ LEV LI SI
PSH LT ADD EXIT` and friends), no traps, no devices, no protected mode. Every
VM runs the same 20M-iteration loop and must produce `s=542894464`; any VM
that does not invalidates the run.

Sweep `N ∈ {1K, 4K, 16K, 64K}` × `ARENA ∈ {64 KB, 256 KB, 1 MB}` × quantum
`Q ∈ {1K, 10K, 100K}`. Report aggregate instructions/sec, per-VM
instructions/sec, and device memory used. **Look specifically for the L2
cliff predicted in §6.4** — plot aggregate rate against `N * ARENA`.

### B4 — the park-on-I/O tax

Extend B3 so one lane in `K` parks every `I` instructions and needs a host
round-trip before it resumes. Sweep the I/O rate over several orders of
magnitude. This prices interpretation D and tells the real port how much
firmware-side buffering `PUTC` needs.

### The go/no-go criterion

Stated in advance, so the result cannot be rationalized afterwards:

> **B3 aggregate must exceed 6.6 G instr/s — 3x this desktop's measured
> 2.2 G — on at least one of the two cards.**

Below 3x, the port is a curiosity that is slower to develop, harder to debug,
and dependent on hardware the rest of the stack does not need. At or above,
it is a real result and §11's plan is worth executing. A number between 2.2 G
and 6.6 G is the genuinely awkward outcome and should be reported as such
rather than rounded up.

### What to report back

One table per device: B0 facts, B1 at each `(S, N)`, B2 ratio, B3 at each
`(N, ARENA, Q)`, B4 curve. Plus the correctness check on every B3 run, and
the `nvcc` version and driver. Raw CSV beats prose; the analysis happens
afterwards, against §6's predictions, and the predictions are on the record
above precisely so they can be shown wrong.

## 9. If it ships: verification and where it runs

**Verification is the same discipline as everywhere else in this repo.** The
GPU is one more engine to lockstep. `make test-c4bb` diffs ~15 images
byte-for-byte against native `c4m32`, including exact cycle-counter parity;
the GPU port earns the same treatment, with each VM writing to its own output
buffer so N runs can be diffed at once. `tools/lockstep.js` proves c4bb's step
and turbo engines stay register-identical at every instruction boundary — the
GPU engine belongs in that harness, not in a separate one.

**But the real target is WebGPU, not CUDA.** The verification path for GPU
work here is a browser driven over CDP, which makes WGSL the only dialect
that can actually be checked end-to-end, and it costs something: no 64-bit
integers (already fine), no pointers (already fine), storage-buffer binding
size limits, watchdog timeouts that make §3's bounded quantum mandatory
rather than merely tidy, and byte access via shift-and-mask (§5.3). CUDA in
§8 measures the hardware; a WGSL rerun of B3 measures the API tax, and the
gap between them is a number worth publishing on its own.

**Hardware, as it currently stands.** This development box is a Hyper-V guest
with `hyperv_drm` (`/sys/class/drm/card1`, vendor `0x1414`) — software
rendering only, no GPU of any kind, so none of §8 can run here. The 3060 Ti
is expected to move to this machine's *host*, which does not by itself make
it reachable from inside the guest: that needs DDA or GPU-P passthrough, and
if neither is available the workflow stays "develop in the guest, dispatch on
the host," which is awkward but survivable given that §5's quantum model
already assumes a host/device split.

## 10. The two adjacent projects, kept separate on purpose

**C — c4 as host, GPU as a device.** A `GPUD` block in the `0x100`–`0x1FF`
device window: the guest writes a buffer descriptor and a kernel id, the host
dispatches a compute shader, the guest reads results back. `mandel.c4r`
becomes a two-line change and renders at absurd speed. This is genuinely
cheap — a device in `src/c4bb/sim/devices.js` and a firmware vector — and it
is *not* this document's project. It should not be smuggled into the port as
a milestone; it is its own small thing, and a better first thing to build if
the answer to §8 is "no."

**D — C4KE/C4IX tasks as GPU threads.** Technically expressible: a task is a
context and the kernel already round-robins them. Ruinous in practice, for
the reason §5.1 gives — a kernel traps constantly, every trap is a host
round-trip, and the throughput case evaporates before the first scheduler
tick. Worth stating so that nobody rediscovers it enthusiastically in six
months.

## 11. Recommendation

Gate everything on cheap experiments, in this order.

**Step 1 — `vm_run_batch(n)`, on the CPU, no GPU needed (about a day).**
Step N arena-based VMs round-robin from one loop, with park-on-I/O. This
forces the only two design changes the GPU actually requires (§5.1 and the
control-block split), produces the honest N-VM CPU baseline that B3 must be
compared against, and is useful to `c4mp` on its own merits. `oisc4-design.md`
§7 recommended exactly this and it was never done.

**Step 2 — shrink an arena (a day).** Get a real image running in ≤256 KB,
ideally 64 KB. If this fails, §5.2's table says the throughput story fails
with it, and steps 3–4 are moot.

**Step 3 — the measurement kit (§8), on the 3060 Ti and the 5080.** Report
against the predictions in §6. Honour the go/no-go criterion as written.

**Step 4 — only then** port the batch stepper to WGSL and lockstep it against
`c4m32` over CDP.

The honest read: **steps 1 and 2 are worth doing regardless of the GPU**,
because they improve `c4mp` and settle a question `oisc4-design.md` left open
two milestones ago. Step 3 is an afternoon and produces a real number either
way. Step 4 should be planned as *a demonstration that the arena model was
right*, in the spirit of OISC4 and c4bb, rather than as a performance win —
because on the arithmetic available today, the performance win is a factor of
two, and factors of two are not why anyone builds a VM twice.

## 12. Files and targets (proposed; none exist yet)

```
docs/c4gpu-design.md          this document
docs/c4gpu-bench-prompt.md    §8, as a self-contained brief for the GPU box
src/c4gpu/bench/              §8 measurement kit, CUDA, throwaway
  b0_device.cu                  device facts + achieved bandwidth
  b1_chase.cu                   scattered dependent-load sweep
  b2_diverge.cu                 divergence tax, measured opcode mix
  b3_minivm.cu                  minimal VM, the money number
  b4_park.cu                    park-on-I/O tax
  results/                      raw CSV, one file per device
src/c4mp/batch.c              step 1: vm_run_batch on the CPU
src/c4gpu/                    step 4, only if step 3 says so
  c4gpu.wgsl                    the kernel
  host.js                       dispatch, park service, lockstep hooks
```

Proposed make targets: `bench-c4gpu` (build the CUDA kit),
`test-c4gpu` (lockstep N GPU VMs against `c4m32`, in the shape of
`test-c4bb.sh`).
