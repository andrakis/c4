# Multiprocessing: libjs with several CPUs, and C4IX on them

Plan, 2026-10-22. Asked for: "investigate what would be needed to make
`libjs` and `c4ix` support multiprocessing ... I'd be interested in getting
it implemented."

## 1. What there is

- **c4mp** (`src/c4mp`, README there) defined the instruction set, and c4lc
  compiles its builtins:

  | # | Opcode | Builtin | Does |
  |---|---|---|---|
  | 66 | `CPUI` | `__c4_cpu_id()` | this CPU's index |
  | 67 | `CPUN` | `__c4_cpu_count()` | how many CPUs |
  | 68 | `CPUS` | `__c4_cpu_start(id, entry, stacktop)` | start a CPU on a function and a stack |
  | 69 | `CPUH` | `__c4_cpu_halt()` | halt this CPU, not the machine |
  | 70 | `CAS` | `__c4_cas(addr, expect, new)` | compare and swap |
  | 71 | `XCHG` | `__c4_xchg(addr, val)` | swap |
  | 72 | `FADD` | `__c4_fadd(addr, delta)` | fetch and add |
  | 73 | `CWAI` | `__c4_wait(addr, val)` | park while `*addr == val` |
  | 74 | `CWAK` | `__c4_wake(addr, n)` | wake up to n CPUs parked on addr |
  | 75 | `IPI` | `__c4_ipi(cpu)` | raise `HIRQ_IPI` on another CPU |

  Each CPU has its own trap handler, cycle interval and protected-mode bit.
  `C4I_SMP` (0x100) announces it. The guest tests `smp0`, `smp1` and
  `deadlock` exercise all of it.
- **But c4mp only simulates SMP.** Every CPU runs round-robin in one host
  thread, so every instruction is atomic and a run is deterministic. Its
  "pass 2", a host thread per CPU and real atomics, was never written.
- **libjs** runs one CPU. `Machine` (`c4m.js`) already keeps each CPU's
  state on its own (registers, cycle count, trap handler, interval, mode,
  signals), apart from what is shared (`arena`, `dev`, `bus`, `heap`). It
  runs none of 66-78.
- **C4IX** is a one-CPU kernel: `sched_cur`, `sched_intrap`,
  `sched_lockdepth`, one idle task, and accounting are single globals, and
  the trap handler relies on interrupts being off for mutual exclusion.

## 2. Why libjs is the easiest way in

JavaScript has exactly the parts a real multiprocessor needs:

- **Web Workers** (and Node's `worker_threads`) are real threads, one per
  CPU.
- **`SharedArrayBuffer`** gives every worker the same arena. The interpreter
  already reads and writes memory through `Int32Array` and `Uint8Array`
  views; over a shared buffer they are the same views.
- **`Atomics`** gives each new opcode its real counterpart:
  - `CAS` is `Atomics.compareExchange`, `XCHG` is `Atomics.exchange`, and
    `FADD` is `Atomics.add`, on the guest's own word;
  - `CWAI` is `Atomics.wait` on that word, which blocks the worker thread;
  - `CWAK` is `Atomics.notify`.

  There is no lost wake-up, for the same reason a futex has none: the wait
  rechecks the value itself.

The catch is that shared memory needs a **cross-origin isolated** page:
COOP/COEP headers, which `serve.mjs --isolate` sends. The server on 8472
runs without it today. A page that is not isolated gets one CPU, as now.

## 3. The design

### 3.1 Two engines, one instruction set

- **Simulated** (`--cpus N`): N `Machine`s in one thread, sharing arena,
  devices and heap, run round-robin a quantum at a time, as c4mp does.
  It's deterministic, so it's what the tests pin: c4mp's `smp0`, `smp1`
  and `deadlock` transcripts, and C4IX booting on 2 and 4 CPUs. It works
  everywhere, isolated or not.
- **Parallel** (`--cpus N --parallel`, and the page when it is isolated):
  one worker per CPU, the arena in a `SharedArrayBuffer`, and the atomics
  above. This is the one that runs faster.

Both engines implement the same opcodes. A guest cannot tell them apart,
except by speed and interleaving.

### 3.2 What is shared, and who owns the devices

- **The arena is shared:** plain loads and stores, and `Atomics` for 70-74.
  Aligned 32-bit accesses do not tear, which is all the guest relies on.
- **CPU 0's worker owns everything else.** It is the worker the page talks
  to today: the devices (UART, disks, clocks, display, sound, mailbox), the
  heap (`heap.js`, whose bookkeeping is JavaScript `Map`s and cannot be
  shared), and the console.
- **The other CPUs forward the operations that reach those** to CPU 0:
  - the host syscall opcodes (`OPEN`, `READ`, `CLOS`, `PRTF`, `PUTC`,
    `PUTS`, `MALC`, `FREE`, `RALC`, `USLP`, `TIME`);
  - device registers below 0x1000.

  A forwarded request goes through a small shared request block: the CPU
  writes the request, `Atomics.notify`s CPU 0, and `Atomics.wait`s for the
  answer. CPU 0 checks for requests at its instruction-boundary checks, every
  1,024 cycles, and while it is idle, through `Atomics.waitAsync`. A request
  costs microseconds, which is fine for I/O and acceptable for `malloc` at
  first. A heap whose bookkeeping lives in shared memory is a later step if
  it shows in profiles.
- **Time:** `TIME` and the clock registers answer from CPU 0's clock, so
  every CPU sees one machine time. Each worker paces itself to it.

### 3.3 Interrupts

- **Per-CPU, as in c4mp:** each `Machine` already has its own trap handler
  and cycle interval.
- **`IPI`:** sets a bit in the target CPU's word of a shared IPI table and
  `Atomics.notify`s it, in case it is parked. The target sees the bit at its
  next boundary check and takes `TRAP_HARD_IRQ` with `HIRQ_IPI`.
- **Device interrupts** (the PIT, the mailbox, the display) go to CPU 0
  only.

### 3.4 CPU-local storage: the one new piece

C4IX's shared libraries find their data through one word per library,
`__c4_dbase`'s slot. `loader_switch` writes the running task's data address
into it at every switch. With two CPUs running two tasks that use libc at
once, one word cannot hold both addresses.

**The fix is a CPU-local window**, like x86's `%gs`: 256 bytes of low memory,
0x0800-0x08FF, through which each CPU sees words of its own. The bus routes
that range to the current CPU's own 64 words. Then:

- **Library slots** are placed in the window, one per library. `IMM slot; LI`
  is unchanged, so no compiler change is needed, and `loader_switch` writes
  the incoming task's data address into this CPU's copy. A library data
  access then takes the bus's slow path, a few percent on library-heavy
  code, and only when SMP is on.
- **The kernel's per-CPU variables** can live there too, though
  `__c4_cpu_id()`-indexed arrays are simpler for most.

The window is announced with the SMP bit. Native c4mp could add the same
window later, so one C4IX image runs on both.

### 3.5 The capability bit

c4mp's `C4I_SMP` is 0x100, which c4m.c also gives to `C4I_C4MPG`, the memory
protection guard. libjs never runs that guard, so it reports 0x100 as SMP,
as c4mp does. A host that is both does not exist; note the overlap in
`include/c4bb_info.h`.

## 4. C4IX on several CPUs

First correctness, with one **big kernel lock**, then finer locks only
where profiles say so. User programs are where C4IX spends its time, and
they run in parallel under a BKL.

1. **Boot.** CPU 0 boots as now, then starts each other CPU with `CPUS` on
   an AP entry. Each AP gets its own stack, its own idle task, its own trap
   handler installation, and its own preemption interval.
2. **The lock.** The trap handler takes it on entry and drops it at the
   switch out. It is a `CAS` spinlock that parks with `CWAI` after a short
   spin. `sched_lock`/`sched_unlock` keep their meaning for the CPU they
   run on.
3. **Per-CPU state.** `sched_cur`, the idle task, `sched_intrap`,
   `sched_lockdepth`, `sched_resched`, `sched_dying` and the accounting
   slice become arrays indexed by `__c4_cpu_id()`.
4. **Scheduling.**
   - One shared task list, as now.
   - A task carries the CPU it runs on, so two CPUs never pick the same
     one.
   - Making a task ready while some CPU idles sends that CPU an `IPI`.
   - An idle AP parks on a per-CPU word with `CWAI`, instead of napping
     the machine.
5. **Libraries.** Slots in the CPU-local window (3.4).
6. **Devices and the console** need nothing: libjs forwards them to CPU 0
   (3.2).
7. **What it shows.**
   - `top` and System Monitor gain a CPU count and a per-CPU load.
   - The CPU meter averages the CPUs.
   - `ps` says which CPU a task is on.
8. **Then use it.** `b4ke -j N` runs independent rules at once, and the
   desktop and a game no longer take turns.

## 5. Milestones

| | What | Proved by |
|---|---|---|
| S1 (done) | libjs simulated engine: opcodes 66-75, per-CPU IRQs and IPIs, the CPU-local window, `C4I_SMP`; `cli.js --cpus N` | c4mp's `smp0`, `smp1` and `deadlock` guests (32-bit builds), totals and deadlock report as c4mp's |
| S2 | libjs parallel engine: workers, `SharedArrayBuffer` arena, `Atomics`, the forwarding block; Node and the page | the same guests with `--parallel`; a two-CPU spin test that runs both halves at full speed; the page when isolated |
| S3 | C4IX SMP kernel: AP boot, BKL, per-CPU state, scheduler, library slots in the window | the whole C4IX suite on the simulated engine at 1, 2 and 4 CPUs; two `spin`s at once on the parallel engine, each at full speed |
| S4 | Show and use it: per-CPU load in `top` and System Monitor, `b4ke -j` | test-desktop; a parallel rebuild measured against a serial one |
| later | Finer kernel locks; a shared-memory heap; c4mp pass 2 (native threads) and the window there | profiles; test-c4mp |

At one CPU nothing changes: the same images, the same timings, and the
same tests.

### S1, done (2026-10-22)

`libjs/smp.js`, opcodes 66-75 in `c4m.js`, the window, and `--cpus`/`-q`
in `cli.js` (`?cpus=N` in the page). c4mp's `smp0` and `smp1` give
exactly c4mp's output:
- at 1, 2, 4 and 8 CPUs;
- at quanta 1, 7 and 1,000;
- with the same cycle counts.

The deadlock report names every CPU and exits 255, as c4mp does.
test-libjs pins all of it against c4mp's own transcripts, and checks
the CPU-local window.

## 6. Risks

- **Isolation.** The parallel engine needs COOP/COEP. `serve.mjs --isolate`
  sends them, and earlier checks found the code-server proxy passes them
  through. The page falls back to one CPU when it is not isolated.
- **Device forwarding is a new path for every I/O on CPUs 1 and up.** It is
  kept to one request block and one service routine, tested in both
  engines.
- **The kernel was written for one CPU.** The BKL keeps that assumption
  true inside the kernel. The risk is code that touches kernel data
  outside the trap handler, such as `task_shim` and the C4KE compatibility
  layer. Each is found and brought under the lock in S3, with the simulated
  engine's determinism to reproduce what goes wrong.
- **Timing-dependent tests.** The simulated engine keeps every existing pin
  deterministic. Parallel runs are checked for results, not transcripts.
