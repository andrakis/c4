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
# At the shipped interval this script now passes repeatedly. Under
# 5x preemption a LIVENESS stall remains -- no corruption, no crash,
# the system simply stops making progress -- and that one is not yet
# root-caused, which is why this file stays out of the pinned suite.

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
