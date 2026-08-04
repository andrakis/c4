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
# With the mask made per-task this script goes from 0/4 to 4/4 at
# FIVE times the shipped preemption rate. It still fails at twenty
# times and beyond, where the interrupt period falls below the trap
# handler's own cost (~1237 cycles) -- a different regime, outside
# the design envelope. That is why this file stays out of the pinned
# suite: it exists to be run under abnormal pressure.
#
# 2026-08-04: a FAR simpler reproducer for what remains, found while
# testing Ctrl-C. At the SHIPPED preemption rate:
#
#     make run-c4ix
#     c4ix:/$ spin 1
#
# spin does its work, prints its tick, and then hangs instead of
# exiting. The same program run standalone --
# `./c4m load-c4r.c -- c4ix.c4r -q c4ix-spin.c4r 5` -- completes
# every time. So the trigger is a compute-bound task spawned BY THE
# SHELL, i.e. a user task whose parent is itself a user task waiting
# on it, and the stall is at or just after the child's exit. Anyone
# picking this up should start there rather than with this script.

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
