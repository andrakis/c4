# C4IX stress script -- NOT pinned by make test-c4ix, because it is
# the one deliberately run under abnormal scheduling pressure.
#
# History, since it is the useful part. This script used to fail
# intermittently at the shipped preemption interval. Making the
# failure reproducible (by raising the preemption rate five-fold,
# PREEMPT_INTERVAL 2000 instead of 10000) turned a heisenbug into
# two ordinary bugs, both now fixed:
#
#   1. The trap handler claimed its re-entry guard AFTER masking the
#      cycle interrupt. Neither the syscall gateway nor __c4_trap
#      masks on the way in, so an interrupt landing in those few
#      instructions found the guard clear and ran the whole
#      non-reentrant handler recursively, on top of the switch
#      already in progress. Symptom: tasks resuming with a garbage
#      program counter.
#   2. Kernel tasks are preemptible while inside kernel code, but the
#      fd and vnode reference counts were updated without masking, so
#      a preemption between "--refs" and the test that followed could
#      free a description another task still held.
#
#   3. The preemption mask was GLOBAL rather than per-task. c4m
#      zeroes the cycle interval when the interrupt fires and relies
#      on the handler to re-arm it; the handler re-armed only at mask
#      depth zero, but the depth belonged to the machine. So a task
#      holding the mask across a context switch handed the next task
#      an interrupt-masked machine, and a compute-bound task landing
#      there never trapped again -- it just ran, burning cycles,
#      while nothing else could be scheduled. That was the stall.
#
#   4. The handler re-armed the cycle interrupt by writing the
#      machine, from inside itself. But a trap handler does not own
#      the registers -- TLEV installs them, several instructions
#      later. So between "sched_cur = n" and that TLEV the scheduler
#      said one task was running while the machine still ran another,
#      and an interrupt landing there saved the outgoing registers
#      into the incoming task and restored the incoming task's stale
#      ones. The two contexts swapped; one then built a trap frame
#      over the frame the other was parked on, and the machine spun
#      forever on a TLEV whose frame returned to itself.
#
#      The fix is c4m's CONF_TRAP_RESTORES_INTERVAL: the interrupt
#      mask became part of the trapped context, saved at trap entry
#      and restored by TLEV, so sched_trap sets it for the incoming
#      task through a frame slot and the machine stays masked until
#      the switch is complete. c4m also refuses to interrupt a TLEV
#      instruction at all, which covers kernels that have not opted
#      in. Either one alone closes this script's failure; both are in.
#
# With the mask made per-task, this script went from 0/4 to 4/4 at
# FIVE times the shipped preemption rate, and (4) took it to 4/4 at
# TWENTY times as well -- so the "different regime below the trap
# handler's own cost" reading of that failure was wrong; it was this
# same bug. This file stays out of the pinned suite because it exists
# to be run under abnormal pressure, not because it is expected to
# fail there.
#
# The reproducer that led to (4) was much smaller than this script.
# At the SHIPPED preemption rate:
#
#     make run-c4ix
#     c4ix:/$ spin 1
#
# spin would do its work, print its tick, then hang instead of
# exiting, while the same program run standalone always completed.
# `ps -s` inside test.sh became a reliable version of the same hang
# once ps emitted its heading as five writes instead of one. Both
# pass now.

echo alpha beta > /ram/t1
cat /ram/t1
wc < /ram/t1
cat /ram/t1 | wc
cat /ram/t1 | cat | cat | wc
cat /ram/t1 | wc > /ram/t2
cat /ram/t2
echo x > /ram/t2
wc < /ram/t2
ps
echo deferred > /ram/t3 &
jobs
wait
cat /ram/t3
exit 0
