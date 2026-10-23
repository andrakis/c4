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
| S2 (done) | libjs parallel engine: workers, `SharedArrayBuffer` arena, `Atomics`, the forwarding block; Node and the page | the same guests with `--parallel`; a two-CPU spin test that runs both halves at full speed; the page when isolated |
| S3 (done) | C4IX SMP kernel: AP boot, BKL, per-CPU state, scheduler, library slots in the window | test-libjs's C4IX checks on the simulated engine at 1, 2 and 4 CPUs and on threads (`make test-c4ix-smp`); compiles at once on the parallel engine, four in half the time of four on one CPU |
| S4 (done) | Show and use it: per-CPU load in `top` and System Monitor, `b4ke -j` | test-c4ix-smp's checks; a parallel rebuild measured against a serial one |
| later | A shared-memory heap (done) and finer kernel locks (memory, done; more where profiles say); c4mp pass 2 (native threads) and the window there | test-sheap; compiles measured; test-c4mp |

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

### S2, done (2026-10-22)

`libjs/par.js` (the control block, the opcodes on `Atomics`, CPU 0's
side) and `libjs/ap.js` (every other CPU, in a worker of its own). It is
`cli.js --cpus N --parallel` in Node (`worker_threads`), and
`?cpus=N&parallel` in a page that is cross-origin isolated.

- **The arena** is a `SharedArrayBuffer`, and a second one holds the
  control block: the machine's halt flag, a doorbell, the machine's time
  in milliseconds, and 24 words per CPU (state, entry and stack, IPI,
  the word it waits on, a request slot, cycles).
- **The opcodes** map one to one: `CAS`, `XCHG` and `FADD` to
  `Atomics.compareExchange`, `exchange` and `add`; `CWAI` and `CWAK` to
  `Atomics.wait` and `notify` on the guest's own word. CPU 0 cannot
  block, since it runs the devices, so its `CWAI` returns to its loop,
  which waits with `Atomics.waitAsync` on its word and the doorbell.
- **Forwarding**, as in 3.2: another CPU's host syscalls and device
  registers go to CPU 0 through its request slot, and it rings the
  doorbell. CPU 0 answers between slices of 16,384 cycles, and whenever
  it is parked or idle. The other CPUs run slices of 65,536 cycles and
  check for an IPI or the machine's end between them. Checking on every
  instruction halved the speed.
- **A deadlock** is reported as on the simulated engine, with the same
  text, after three seconds in which every CPU is parked, none is asking
  anything, and nothing has woken them. The machine exits 255.
- **Strings leave the machine as copies.** A browser's `TextDecoder`
  refuses a view of a `SharedArrayBuffer`, and a view posted to the page
  would share the arena rather than copy it.
- **Without isolation** (no `SharedArrayBuffer`), `?parallel` falls back
  to the simulated engine. A page is isolated only when it is served
  with COOP and COEP (`serve.mjs --isolate`) **and** from a secure
  origin: `https://`, or `http://localhost`. `http://192.168.1.202:PORT`
  from another computer is not isolated, whatever the headers.

`spinmp` (libjs/tests/src/spinmp.c) gives each CPU the same 20 million
iterations. Times are on helios (6 cores, load average about 2.5), in
seconds:

| | 1 CPU | 2 CPUs | 4 CPUs |
|---|---|---|---|
| simulated (`--cpus N`) | 2.3 | 4.0 | 7.7 |
| parallel (`--cpus N --parallel`) | | 2.1 | 2.5 |
| the page, unpaced, 4 CPUs (Chrome, `https://`) | | | 11.5 simulated, 5.0 parallel |

test-libjs runs `smp0`, `smp1`, the deadlock, the window and `spinmp`
with `--parallel`, and checks their answers. Their cycle counts are not
pinned, since the host interleaves the threads. C4IX boots to its shell
on `?cpus=2&parallel` as it does on one CPU. It starts no other CPU
until S3.

### S3, done (2026-10-22)

C4IX runs on every CPU the host gives it, when the host is libjs with
`--cpus N` (or `?cpus=N`): `C4I_SMP` and `C4I_C4MJS` both, since only
libjs has the window. Anywhere else, `--cpus 1` included, nothing
changes. On 2 CPUs `ps` shows two user tasks `run` at once.

- **Per-CPU state.** What was the scheduler's own state is `struct cpu`
  (c4ix.h): the running task, the trap flag, the mask depth, the
  accounting readings, the slice, and execve's and task_exit's
  hand-offs. `cpu_self` points at it, at an ordinary struct on one CPU
  and at the window, 0x800, under SMP. `sched_cur` and `sched_intrap` are
  macros over it, so no module changed for it but sched.c.
- **The kernel lock** (`bkl_take`, `bkl_drop`): a `CAS` spinlock that
  parks in `CWAI` after 64 spins, nested per CPU.
  - The trap handler holds it from entry to exit.
  - `sched_lock` takes it outside the handler, after masking.
  - A kernel task holds it the whole time it runs, since kernel tasks
    call the kernel directly. So init's loader and the C4KE layer's
    kernel callers are one CPU's at a time, as they were.
  - User tasks run without it, at once.
- **Where tasks run.** Kernel tasks run on CPU 0 only. Each other CPU has
  an idle task of its own, `idle1`, `idle2` and so on. They come before
  init in the list, so every task id is N - 1 higher than on one CPU.
- **On a CPU.** `oncpu` marks the CPU that may still be on a task's
  stack. That is the CPU running it, or the CPU that switched it out,
  which stays on the task's stack until its trap returns. Until that
  CPU's next trap, or its idle loop's next look (`smp_settle`), no other
  CPU resumes the task and nothing frees it (`task_busy`, in
  task_release, task_reap and the orphan sweep). A task killed while it
  runs elsewhere, by a Ctrl-C on CPU 0 say, is switched away from at its
  CPU's next trap, unserved.
- **Idle and wake.** CPU 0's idle task naps as before, under the lock
  while it looks at the kernel. The others park in `CWAI`. A trap that
  leaves ready work while some CPU idles sends that CPU an `IPI`
  (`smp_kick`), and the IPI arrives as a cycle interrupt. An IPI while
  a CPU holds the mask changes nothing, so an idle loop looks again under
  the lock before it parks.
- **Libraries.** A `-fpic` library's data slot is a word of the window
  (`smp_slot_alloc`, words 24 to 63), so each CPU points it at the data
  of the task it runs.
- **Boot and shutdown.** `sched_init` starts the others on a 64 KB stack
  each (`smp_ap`). At shutdown the kernel task lets the lock go, masked,
  and waits for each to halt.

The engines changed with it:
- `USLP` sleeps the CPU that executes it on the simulated engine too, not
  the machine, while another CPU can run. Otherwise CPU 0's idle naps
  stopped every other CPU.
- On the parallel engine, CPU 0's nap is real time while others run. An
  IPI ends it early. CPU 0 waits blocked in `Atomics.wait`, a few ms at
  a time, so the other CPUs' requests are answered at once: an awaited
  wake cost a third of a millisecond each.
- A CPU other than 0 read unaligned words as 0 (ap.js's bus), which broke
  every C4R header such a CPU loaded. Its bus is now arena.js's, as CPU 0's
  is.

`make test-c4ix-smp` runs test-libjs's C4IX checks that say whether the
kernel holds together with several CPUs in it (libjs/tests/test-c4ix-smp.sh),
at 1, 2 and 4 CPUs simulated and 2 on threads:
- boot, `ps` and a clean shutdown;
- the root archive's unpack;
- `cc` inside C4IX, `echo` byte for byte as the disk's;
- two compiles at once;
- libctest;
- three tasks sharing libc's code;
- unixtest's vfork, execve and waitpid.

`spin` cannot show the speed-up: it counts its CPU's cycles, which pass
whoever runs. A compile can. `cc -O mandel.c` inside C4IX, times on helios
past boot and the root archive's unpack, in seconds:

| | 1 CPU | 2 CPUs, threads | 4 CPUs, threads |
|---|---|---|---|
| one compile | 6.5 | 8.2 | |
| two at once | 14.5 | 9.9 | |
| four at once | 27.3 | | 14.0 |

One compile is slower on a CPU other than 0. Its 7,100 mallocs and
5,900 frees each go to CPU 0's heap (3.2), and its syscalls take the lock.
A heap in shared memory, and finer locks, are what "later" in the table
above is for.

### S4, done (2026-10-23)

**The accounting is per CPU** (sched.c, cpustat.h). The snapshot has the
CPUs: how many, each one's idle task, its busy and idle cycles, and the
ms it parked. It also has, for each task, the CPU running it.

- **Busy and idle** are counted at every trap, with the task's own
  cycles: a task's cycles are busy, its CPU's idle task's are idle.
- **A park or nap** is closed in the trap of the IPI that ends it, or by
  the idle loop's next line, whichever comes first. A park still going
  counts as far as it has got. (Closed by the loop alone, a park was
  charged the time its CPU then spent on the work it was woken for.)
- **On several CPUs the machine is all of them**, as in NT. A task that
  keeps one CPU of two busy has 50%. The usage is the CPUs' average, and
  each CPU's idle task has what that CPU left (`cpu_shares`). Each CPU's
  load (`cpu_load`) is its busy cycles of all it ran, over the part of the
  interval it was awake.
- On one CPU every number is what it was.

**What shows it:**
- `ps` and `top` gain a CPU column, and `top` a line of each CPU's load
  ("2 CPUs: cpu0 100% cpu1 100%"). On one CPU their columns are the ones
  there have always been, so the pinned outputs hold.
- System Monitor's idle row is every idle task together. Load draws a
  history for each CPU, up to eight, beside the machine's gauge. The
  taskbar's meter is the machine's average.

**`b4ke -j N`** runs up to N steps at once. A `RUN` is queued. It starts
once no step before it that is still to finish makes a file it names,
among its `DEPS` sources or its words, or names the file it makes. Its
up-to-date test is made then, against the files as those steps left them.
A `RUN` with no target waits for everything before it and holds up
everything after it. The other verbs wait for the queue. Sub-builds get
the same `-j`.

**A scheduling fix with it.** A task preempted while another CPU idles
keeps its CPU, and the ready task goes to the idle one. Before, a CPU
took turns between two tasks: the waiting one was always the task it had
just switched out, whose `oncpu` mark barred the other CPU from it. The
other CPU parked while both tasks waited for one.

`b4ke -B -C /usr/src/c4ix/lib` inside C4IX, on 2 CPUs on threads, rebuilds
13 steps: 72.6 s, and 42.0 s with `-j 2`. Its libraries are the disk's,
byte for byte. N compilers at once need memory for N, about 3 MB each.
Four of them beside the unpacked root archive overran the page's 64 MB.

`make test-c4ix-smp` checks `ps` and `top`'s columns and `b4ke -j`, at
each CPU count.

### After S4 (2026-10-23): a shared heap, and a lock the memory skips

From "later" in the table: the heap and the first of the finer locks.

**The heap is every CPU's** (libjs/sheap.js), on the parallel engine.
Its rules are heap.js's, and its books are a SharedArrayBuffer apart from
the arena:
- **a byte for every 16 bytes** says what block starts there, so freeing
  a stray pointer still changes nothing;
- **a word for every 4096 bytes** holds the span of a large block;
- **the free ranges** are kept in a sorted array.

Together they come to about 7% of the arena. Every thread allocates under
one spinlock, instead of asking CPU 0 through the event loop for every
`malloc`.

The one thing in the arena is a free small block's link to the next on
its class's list. A guest that writes to memory it has freed can break
that link, but every link is checked against the books before it is
followed, so a broken list is dropped and never handed out. The
simulated engine, and one CPU, keep heap.js. test-sheap.mjs runs four
threads at once against it.

**Memory skips the kernel lock.** A protected program's `malloc` and
`free` (its PM traps) touch only the heap and the program's own list of
blocks. That list now has a lock of its own (task.c, `umem_lock`). A trap
that is only that, and leaves its task running with its slice left and
nothing pending, takes `sched_trap`'s light path (`sched_light`) without
the kernel lock. In one compile on 4 CPUs, 11,564 of 11,793 such traps
did.

For it, a task's cycle accumulators are its own CPU's: snapshots and
`ps` read them and fold nothing. Locks are now one kind: `klock_take`
and `klock_drop`, nested per CPU, which spin and then park. The kernel
lock is one of them.

`cc -O mandel.c` inside C4IX, in seconds past boot, against S3's
figures:

| | S3 | now |
|---|---|---|
| one compile, 2 CPUs on threads (1 CPU: 7.3) | 8.2 | 7.0 |
| four at once, 4 CPUs on threads (1 CPU: 27.3) | 14.0 | 11.3 |

A compile on a CPU other than 0 now runs as fast as on one CPU. Four at
once are held back by the host, not the kernel: on helios (6 cores,
shared), four separate one-CPU machines compiling at once take 13.7 s
for what one alone does in 7.3.

**The rest:**
- **The boot says what the machine is:** its memory and layout ("64 MB:
  the kernel below 0x31000, the heap to 0x3eff000"), and its processors,
  each with only its window of its own, running "a thread each, all at
  once" or "simulated, taking turns on one thread". libjs reports the
  parallel engine as `C4I_PAR` (0x20000).
- **System Monitor's Tasks tab** has a CPU column on several CPUs.
- **For a rebuild from clean:** the shell expands `*` and `?` in a word's
  last part, `rm -r` takes a tree, `ls` takes several names, and Files
  selects and deletes several items, folders whole (docs/c4ix-desktop.md).

### After S4 (2026-10-23): pinning a program to CPUs

A task may be limited to some of the CPUs: `cpumask` in its `struct
task`, bit k for CPU k, and 0, the default, for any. `SYS_AFFINITY`
(242, `uaffinity` in libc4ix) sets and reads it. A child starts with its
parent's mask, through spawn and vfork both, and keeps it through
execve. A mask that names no CPU the machine has is no mask at all.

The scheduler honours it in four places in sched.c:
- `sched_mine`: a CPU picks only tasks it may run.
- `sched_pick` and the trap: the running task is not kept where it may no
  longer run. Re-pinning the caller takes effect at once; another task
  moves at its next trap, within one slice.
- `smp_kick`: an idle CPU is woken for a ready task only if the task may
  run there.
- The rule that keeps a preempted task while another CPU idles applies
  only when the task replacing it could go to that idle CPU. Otherwise a
  task pinned here would wait behind it.

Kernel tasks stay on CPU 0 and idle tasks on their own CPU; neither can
be pinned.

`pin` is the command:

```
pin 2 doom -window          # Doom in this Terminal's window, on CPU 2
pin 1,3 cc -O big.c         # a compile on CPU 1 or 3
pin -p 15 0-1               # task 15 to CPU 0 or 1, as it runs
pin -p 15                   # where task 15 may run
```

It sets its own mask, starts the command (found as the shell finds it)
and waits for it, so `pin CPUS cmd &` works as a background job.

To show it off, start the desktop on `?cpus=4`, open a Terminal per
game, and run something like this in each:
- `pin 1 wolf3d --window`
- `pin 2 doom -window`
- `pin 3 tyrian --window`

`top` and System Monitor then show each game on its own CPU, and the
desktop on CPU 0. The games ask for a nice of -5 themselves
(c4ix-design.md 4.4), so on a CPU they share with something else they are
still served first. test-c4ix-smp.sh checks that a pinned program runs
where it was put.

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
