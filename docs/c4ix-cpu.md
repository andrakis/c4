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
