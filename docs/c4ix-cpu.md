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

### 5. A syscall fast path, a software time slice, and an idle task

- **An idle task.** The idle loop is a kernel task of its own, `idle`,
  made first so that it is task 0, as NT's System Idle Process is. The
  context the machine booted in is now `kernel`, task 1. It waits on the
  idle task, which returns when nothing else is alive. `sched_pick` never
  gives the idle task a turn in the round-robin; it runs only when no
  other task can. It used to be READY all the time, so every syscall of
  a busy task went through it on the way back.
- **Task ids move up by one.** The root archive's unpack is task 4, not
  3, so the tests and the README say `wait 4`.
- **The fast path.** A syscall or emulated opcode that leaves its task
  able to run, and asks for no switch (`sched_yield` inside a trap sets
  `sched_resched`), returns straight to the task while its slice lasts.
  `CK_SCHEDULE` now yields, so a C4KE program that asks to be
  rescheduled still is.
- **The slice is kept in software.** c4m fires its cycle interrupt only
  when the counter is a multiple of the interval, and a multiple that
  falls inside the kernel is lost. A syscall-heavy task saw 68 of its
  10,000 ticks a second. `sched_trap` starts a slice at every pick and
  ends it on the syscall path once the interval has run.

**The slice, measured on C4IX** (your decision 6). Syscalls a second,
from `prof-c4ix` with the kernel built at each interval:

| interval | conspin | spin under the desktop | frames in 5 s |
|---|---|---|---|
| 10,000 (C4KE's) | 185.0k | 140.2k | 111 |
| 20,000 | 191.3k (+3.4%) | 147.6k (+5.2%) | 116 |
| 50,000 | 195.9k (+5.9%) | 153.3k (+9.3%) | 118 |
| 100,000 | 197.4k (+6.7%) | 154.9k (+10.4%) | 120 |

It is now 50,000: most of the gain, and a task waiting behind another
waits at most 0.5 ms at the browser's 100 MHz. Doubling it again buys
1%.

**Native c4m** (`make bench-c4ix`; both kernels built the same way):

| | baseline | step 5 |
|---|---|---|
| syscall | 2,009 | 632 |
| yield | 2,012 | 1,261 |
| spawn+wait | 27,242 | 22,704 |
| pi100 | 179,137 | 152,207 |
| pi300 | 648,546 | 539,035 |
| cycles to userland | 698,709 | 726,414 |

Making the idle task costs 4% more cycles to userland.

**libjs**, per 5 machine-seconds (`make prof-c4ix`):

| situation | baseline | step 5 |
|---|---|---|
| shell | 0.29M cycles | 0.39M |
| conspin | 51,031 syscalls/s, spin 2.6% | 195,848/s, spin 9.5% |
| desk1 | 6.7M (1.3%), kernel 85.9% | 2.1M (0.4%), kernel 71.4% |
| idle | 11.0M (2.2%), kernel 60.6% | 5.4M (1.1%), kernel 28.9% |
| spin | user 24.3% | user 28.0% |

top with spin at the console now gives the idle task 0 to 1% (it had
65%), System 80% and spin 19%. Spin is bound by its own `SYS_CYCLES`
syscalls, which step 9 removes. test-c4ix-cpu checks that a busy
machine's usage is at least 95%.

Found on the way:
- **A leak at shutdown.** The idle task is released while it runs, so
  it waits on the reap list for a safe point, and none came before
  shutdown. The kernel now reaps after its wait.
- **Ctrl-C at the console.** It finds the foreground job by descending
  from a kernel task's wait. That walk now skips the kernel task, whose
  wait is on the idle task.
- **The desktop terminal's Ctrl-C.** It drains the program's pending
  output before echoing `^C`, as a tty's output queue would. top, which
  writes a whole screen without being switched out now, otherwise
  cleared the `^C` with its last frame.
- **The desktop's once-a-second tick** (Task Manager, the CPU meter) is
  on machine time, which its numbers count and the desktop sleeps on.
  It was on the host's clock: headless, an idle machine that naps covers
  many seconds of its own in one of the host's, and Task Manager stopped
  refreshing.

### 6. Event-driven waits, and counters for the living and the dead

- **One death, `task_zombify(t, code)`.** It is used by a task's exit,
  by Ctrl-C at the console and in a terminal, and by a signal's default
  action. It wakes the task waiting on `t` there and then, delivering
  the code into `wait_result` and `sv_a`, and that task reaps it. Before,
  `sched_pick` asked every waiting task, on every pick, whether its
  target had died yet (`sched_waitdone`, one `task_get` walk each). A
  wait that starts after the death finds the zombie and reaps it at
  once, so a zombie never has a waiter. The orphan reaper no longer
  looks for one.
- **Counters.** `sched_nlive` counts the tasks still to finish: all but
  the kernel and idle tasks and the zombies, background jobs included.
  The idle task returns, and the machine shuts down, when it reaches 0;
  that replaces the idle loop's scan of the whole table on every pass.
  `sched_nzombies` gates the orphan reaper. A task is counted in as it
  joins the table, and counted out by `task_zombify` and
  `sched_retire`.
- **`sched_pick`** asks parked tasks only about what can wake them by
  itself: the clock and pipe data. A task that has just died may be off
  the list already, because its waiter reaped it, so the walk starts
  from the head for a zombie and stops when it comes back round.

Found on the way:
- **`task_exit` outside the trap handler.** This is a task whose entry
  returned, in `task_shim`, behind protected mode for a user task. It
  now leaves its code in `sched_dying` and dies in the trap it raises
  next. Releasing a task from its own context takes the scheduler lock,
  and from protected mode that is a trap out of which the zombie is
  never picked again. Its release stopped halfway, and the task stayed
  on the list to be released a second time.
- **The stack-overrun kill** set `trap = 0` afterwards, which is
  `C4IX_TRAP_ILLOP`, so the dead task's trap was then serviced as a
  syscall. It now passes `C4IX_TRAP_SOFT_IRQ`, which only switches.
- **The idle backoff** no longer drops back to the floor. It did so when
  a pass found a task ready, which since step 5 never happens. Dropping
  back whenever anything else had run, which I tried first, doubled the
  idle desktop's cycles in short naps.
- **c4fc** does not yet accept a header's `extern` for a global used
  before the unit defines it further down; c4lc does. `sched_nlive` and
  `sched_nzombies` are defined above their first use, and the compiler
  fix is a task of its own.

| | step 5 | step 6 |
|---|---|---|
| yield (native c4m) | 1,261 | 977 |
| spawn+wait (native) | 22,704 | 22,500 |
| desk1, per 5 s | 2.13M cycles (0.4%) | 1.69M (0.3%) |
| idle, per 5 s | 5.45M (1.1%) | 5.03M (1.0%) |
| conspin syscalls/s | 195,848 | 195,575 |

### 7. The trap path trimmed; `switch` dispatch

- **Dispatch by `switch`.** `sys_dispatch` and `sys_pmviolation` are
  switches; c4lc compiles each to a jump table over dense numbers, at a
  fixed cost. They had been ladders of ifs at six instructions a rung,
  and `SYS_SLEEP`, the desktop's most frequent call, was the 24th. c4fc
  gives the same objects.
- **`ck_dispatch` stays a ladder, for now.** As a switch, its jump
  table in the kernel data segment was overwritten in some layouts.
  test-c4ix-c4ke's benchtop failed with the kernel image at a path of 7
  or 8 characters ("OPCD does not support opcodes requiring arguments"),
  and passed at 5, 6, 10 and 17; a kprintf hid it. Something writes
  where it should not, and a ladder, which reads no data, does not show
  it. Finding the write is a task of its own.
- **Nothing to do costs nothing.** The reap list and pending signals
  are tested in `sched_trap` before a call is made. Both are almost
  always empty.
- **Macros.** `sched_current()` and `sched_in_trap()` are macros over
  the globals: every module reads them on every syscall, and `-minline`
  cannot inline across modules.
- **A fresh slice after a spawn.** Loading an image is a long stay in
  the kernel, and it counted against the parent's slice. A shell that
  spawns and then waits reached its wait, or did not, by where the
  slice happened to end, and the `ps` its child ran saw it either way,
  so the 64-bit golden changed with unrelated code. `SYS_SPAWN` now
  starts the parent's slice again (`sched_new_slice`), and the shell is
  always waiting.

| | step 6 | step 7 |
|---|---|---|
| syscall (native c4m) | 633 | 411 |
| yield (native) | 977 | 818 |
| spawn+wait (native) | 22,500 | 21,798 |
| conspin syscalls/s (libjs) | 195,575 | 249,889 |

Under conspin, the work in `sched_trap` itself is now about 300 cycles
per syscall and `sys_dispatch` about 40.

### 8. The idle loop naps straight to its next deadline

A task reading the console waits for a keystroke, which comes with no
deadline. While one does, the idle task keeps its backoff, doubling the
nap from 100 µs to 20 ms. With nobody reading the console, nothing a
task waits for can come from outside, and the nap goes straight to the
next sleeper's deadline: one pass where the backoff took about seven to
cover a desktop's 15 ms sleep. That nap is capped at 100 ms, so a Ctrl-C
is still seen promptly on a host whose sleep a signal does not cut
short.

| per 5 s | step 7 | step 8 |
|---|---|---|
| desk1 | 1.69M cycles | 1.61M |
| idle | 5.03M cycles | 4.94M |

The gain is small because step 5 had already taken the idle task out of
the busy path. What is left of an idle desktop is mostly the desktop
itself, which step 11 is for.

### 9. `ucycles()` without a trap

libc4ix's `ucycles()` was a syscall, `SYS_CYCLES`, the one a busy loop
makes most, for what one instruction answers: the cycle opcode is
allowed behind protected mode. It is now `__c4_cycles()`. `SYS_CYCLES`
stays for programs already linked against it.

With spin at the console, top gives spin 98% of the machine and System
2%; at step 5 they were 19% and 80%.

| per 5 s (`prof-c4ix`) | baseline | step 9 |
|---|---|---|
| conspin: kernel / user | 97.4% / 2.6% | 1.4% / 98.6% |
| spin under the desktop: kernel / user | 75.7% / 24.3% | 1.8% / 98.2% |

### 10. Plain c4 support removed

C4IX had a second way to run, for a host with no traps: a cooperative
switch through a trampoline LEV (`coop_switch`), and a second door into
the kernel, a call through a table the loader wrote into every image
(`__c4ix_systable`, `loader_systable`). No host reached it. Plain c4 has
no INFO opcode, and its images need opcodes plain c4 lacks, so plain c4
runs C4IX only through c4m.c, and then C4IX sees c4m.

Gone: `sched_on_c4m` and every test of it, the trampoline,
`coop_switch` and `sched_yield`'s cooperative branch, `host_type()` and
`HOST_C4`/`HOST_C4M`, `loader_systable`, and libc4ix's `__c4ix_systable`
and `uva_use_trap`; the gate always traps. init's preemption demo no
longer has a "no preemption on this host" branch. A host without
`C4IX_I_C4M` is refused at boot with a panic, instead of being run
degraded.

The `-c4ix-c4` targets stay as they were (your decision 5): they already
run `./c4 c4m.c load-c4r.c -- c4ix.c4r`, where C4IX sees c4m, and
`test-c4ix-c4` still matches the same golden. `src/tests/test_coop_switch.c`
stays too, as a compiler test and as the record of the trick (C4DOS
still uses it).

Also: init's `--help` text described `--desktop` in the old order.

### 11. The desktop

At step 10 an idle desktop with four windows ran 4.94M cycles in 5
seconds. 79% of that was the desktop, which redrew the whole scene once
a second for Task Manager's tick, at about 770k cycles a redraw:
`term_draw` 34%, `gui_frame` 21%, `gui_text2` 11%.

**(a) Task Manager minimised** still takes its sample each second, so
its history has no gap when the window comes back, but no longer redraws
the scene for a window that shows nothing.

**(b) A cheaper `gui_frame`** (libjs/guest/gui.h). Every command of every
frame reserves its room in the ring there, a thousand for a redraw.
`gui_at` now keeps where the head is in the ring as it moves, so the
common case, room before the end, is one test without a `%`. That is
about 126 cycles a command, from 158.

**(c) A row cache in `term_draw`.** Each row's background rectangles and
runs of text are worked out once and kept, as [column, columns, kind and
colour], and worked out again only for a row whose cells changed: the
single-cell put, `clear()` and the scroll, which moves the cache with
the cells. A row with more than 24 runs is drawn from its cells, as
before. What it draws is unchanged, command for command. Each redraw had
scanned the 2,000 cells of every terminal twice.

| idle desktop, 4 windows, per 5 s | step 10 | (a)-(c) |
|---|---|---|
| cycles | 4.94M (1.0%) | 3.59M (0.7%) |
| `term_draw` | 1.31M | below the top 8 |
| `gui_frame` + `gui_commit` | 0.97M | 0.90M |
