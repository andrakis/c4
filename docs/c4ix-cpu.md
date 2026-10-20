# C4IX's CPU: where the cycles go, and the work to cut them

A review on 2026-10-20 found that C4IX's kernel took most of the cycles
it ran, and that Task Manager's CPU figures were wrong. This document is
the work that followed, one step at a time, each measured against the
baseline below. The plan and the decisions it needed were reviewed
first.

Decisions taken:
- The idle loop becomes a task of its own, and `boot` is renamed `kernel`.
- Shutdown is decided by an `nlive` counter.
- Idle is measured in machine time. Time spent in system calls is charged
  to a "System" row, not to the program that made them.
- The desktop redraws only what changed, and waits for events with a new
  syscall instead of polling.
- `test-c4ix-c4`, `run-c4ix-c4` and `demo-c4ix-c4` stay; they run c4m.c
  under `./c4`, which is a c4m host to C4IX.
- The time slice is measured again here rather than taken from C4KE.
- Counters are 64-bit `long long`, which c4lc now has on a 32-bit target
  too (c4lc-design.md 14.7).

## The profiler

`make prof-c4ix` runs `libjs/tests/prof-c4ix.mjs` in five situations for
5 machine-seconds each. It boots the browser's C4IX (c4ix32 on libjs at
100 MHz, a 64 MB arena), and charges every few hundred cycles to the
kernel function the pc was in, or to user code. Run directly, it takes
`USERIMGS=` to break user code down by an image's functions, `TRACE=1`
to log Task Manager's rows, and `SCREEN=1` for the screen's text.

| situation | what runs |
|---|---|
| `shell` | the console shell at its prompt |
| `conspin` | `spin` at the console |
| `desk1` | the desktop and its Command Prompt, idle |
| `idle` | the desktop with Command Prompt, Task Manager, Notepad and Calculator, idle |
| `spin` | the same, with `spin` running in the Command Prompt |

## Baseline (2026-10-21, at f15564b)

Per 5 machine-seconds:

| situation | cycles run | of the machine | kernel | user | the kernel's top functions |
|---|---|---|---|---|---|
| shell | 0.29M | 0.1% | 100% | 0% | `sched_unblocked`, `sched_run`, `sched_unlock` |
| conspin | 500M | 100% | 97.4% | 2.6% | `sched_trap` 34.9%, `sched_run` 11.7%, `task_get` 7.5%, `sched_pick` 6.3%, `task_reap_orphans` 5.9% |
| desk1 | 6.7M | 1.3% | 85.9% | 14.1% | `sched_run` 17.3%, `sched_waitdone` 11.3%, `sched_trap` 10.6%, `task_get` 9.1% |
| idle | 11.0M | 2.2% | 60.6% | 39.4% | `sched_run` 11.4%, `sched_trap` 9.4%, `sched_waitdone` 7.2%, `task_get` 6.1% |
| spin | 500M | 100% | 75.7% | 24.3% | `sched_trap` 22.2%, `sched_run` 10.7%, `task_get` 8.4%, `sched_pick` 5.8% |

In `conspin`, `spin` makes about 51,000 system calls a second (ILLOP
213, `SYS_CYCLES`), and each costs about 1,960 cycles, nearly all of it
the scheduler: every syscall goes through `sched_pick`, and the boot
task's idle loop is always READY, so every syscall switches into it and
back.

Native c4m, `c4ix-bench`:

| | cycles |
|---|---|
| syscall | 1,797 each |
| yield | 1,717 each |
| pi100 | 173,346 |
| pi300 | 625,526 |

Task Manager, idle with four windows, says "CPU Usage 45%" while the
machine is 98% asleep.

## Steps

1. The profiler and this baseline.
2. Orphan zombies are reaped.
3. Task Manager's and top's percentages, without overflow.
4. An accounting model whose rows add up to 100%: System, System Idle,
   64-bit counters, one atomic snapshot.
5. A syscall fast path with a software time slice, and an idle task.
6. Event-driven waits, and `nlive` and zombie counters.
7. The trap path trimmed; `switch` dispatch.
8. The idle loop naps straight to its next deadline.
9. `ucycles()` without a trap.
10. Plain c4 support removed.
11. The desktop: no tick while minimised, a cheaper `gui_frame`, a row
    cache in the terminal, redrawing only what changed, and an event wait.

## What each step did

### 2. Orphan zombies

`task_reap_orphans` sweeps a zombie only when its parent is a userland
task: kernel code waits by pointer, so a kernel parent's children must be
left for it. A parent that no longer exists was treated like a kernel
one, though it holds nothing, and its zombie stayed forever. vfsload
starts `c4ix-tar` to unpack the root archive and exits without waiting,
so every boot left one. A missing parent now counts as userland.
`libjs/tests/test-c4ix-zombie.mjs` boots without `wait`, lets the unpack
finish, and checks that `ps` lists no zombie. Without the fix it fails.

### 3. Percentages without overflow

Task Manager and top worked out a task's share as `cycles * 100 /
total`. A task that runs more than 21.47M cycles in a sample, a fifth of
a second at 100 MHz, overflows 32 bits there: under load Task Manager
showed "desktop -17" and "CPU Usage: 104%", and top showed -5 and -11.
Both now do the arithmetic in thousands of cycles, which a word holds
for 24 days at 100 MHz, and clamp to 0..100. top also took its first
screen against nothing, so that screen showed totals since boot; it
takes a baseline first now. `libjs/tests/test-c4ix-cpu.mjs` runs top
with spin busy at 100 MHz; before the fix it reproduces the negative
shares.

### 4. Accounting whose rows add up to 100%

Cycles used to be charged at a context switch, to the task switched
away from. A syscall's kernel time therefore went to whichever task
happened to run before the next switch. The time the idle loop napped
counted for nothing, so every percentage was a share of a small total;
an idle desktop showed 45% busy.

- **Per task, two counts.** `sched_trap` reads the cycle counter on the
  way in and on the way out. Cycles since the last trap ended are the
  task's own (`ucycles`). Cycles inside the handler are the kernel's on
  its behalf (`kcycles`): its syscalls, its preemption ticks and its
  switches.
- **No 64-bit arithmetic per trap.** The totals are `long long`, but a
  trap only adds into two int accumulators. These are folded into the
  totals before they could overflow (2^30), and whenever the totals are
  read.
- **Naps count.** The idle loop adds the machine milliseconds it napped
  to `acct_napms`. The counter stands still meanwhile, so this is the
  one count in time rather than cycles.
- **Reaped tasks are not lost.** A task leaving the table adds its
  totals to `gone_user`/`gone_kernel`, so the sum never goes backwards.
- **One snapshot.** `SYS_CPUSTAT` (229, `ucpustat`) copies out every
  task's record and the header, all in one trap. Task Manager used to
  build its sample from one `SYS_TASKINFO` call per task, which was
  O(n^2) and not atomic.
- **The shares** (`src/c4ix/include/cpustat.h`, shared by top and Task
  Manager) are weighed by time. In an interval of `ms` milliseconds the
  machine was awake for `ms - nap`, and that part is divided among the
  rows by cycles; the naps are idle.
  - A task's row is its own cycles.
  - **System** is the kernel's cycles on behalf of every task but the
    idle one.
  - **System Idle Process** is the idle task's cycles plus the naps. It
    is what the other rows leave, so a screen adds up to exactly 100.
  - A task that ended during the interval is in the total but has no
    row, as in NT.
- `cpustat.h` is separate from `c4ix_user.h` because it is the only
  userland code that needs `long long`.

Found on the way: C4KE's task table (`ck_export_fill`) dropped the
billions of a task's cycles, so its count restarted past a billion. It
now takes both words from the 64-bit total.

`test-c4ix-cpu.mjs` now checks that the rows add up to exactly 100, and
that an idle machine is at least 95% idle (it is 100%). test-desktop
checks that Task Manager has the two new rows.

The accounting is honest, and so it shows the problem step 5 fixes. With
spin running at the console, top gives:

| row | share | cycles in the second |
|---|---|---|
| idle loop (`boot`) | 65% | 64.8M |
| System | 30% | 30.3M |
| spin | 5% | 5.0M |
| CPU usage | 35% | |

The idle loop is READY, so every one of spin's syscalls switches into
it and back.

**Cost.** Profiled against the baseline kernel on `conspin`, the two
counter reads and the adds cost about 44 cycles a trap. `sched_trap`
grew from 174.6M to 197.0M cycles in 5 seconds, over about 505k traps.
Spin's syscalls fell from 51,031 to 50,273 a second (−1.5%). Nothing
else in the profile moved.
