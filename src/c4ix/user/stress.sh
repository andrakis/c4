# C4IX stress script -- NOT pinned by make test-c4ix.
#
# This is test.sh plus a background job, and it currently exposes an
# intermittent hang: run after roughly a dozen prior commands, the
# shell can stall spawning a backgrounded, redirected command. Adding
# any output to the shell's per-command path makes it complete, which
# is the signature of a scheduling race rather than a logic error.
# It is NOT root-caused, so it is kept out of the pinned suite rather
# than pinned in whatever state happens to pass.
#
# What has been ruled out: descriptor exhaustion (saved fds stay at
# 4/5 for the whole run), fd leaks per command, the four-stage
# pipeline, cat on a missing file, truncate-and-rewrite, and the
# background job itself in isolation -- each of those runs clean.

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
