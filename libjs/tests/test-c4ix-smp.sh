#!/bin/bash
# test-c4ix-smp.sh N [--parallel] -- C4IX on N processors (docs/c4ix-smp.md
# S3): the simulated engine, or with --parallel a thread per CPU.
#
# test-libjs's C4IX checks, the ones that say whether the kernel holds
# together with several CPUs in it: boot and ps, the root archive's
# unpack, cc inside C4IX (mandel.c; echo built byte for byte as the
# disk's), two compiles at once, libc, two tasks sharing libc's code
# (its data slot is in the CPU-local window), and unixtest's vfork,
# execve and waitpid. And S4's: ps and top's CPU columns, and b4ke -j
# rebuilding the libraries byte for byte. And what came after: the boot's
# account of memory and processors, and the shell's wildcards and rm -r.
# Every task id is N - 1 higher than on one CPU: each other CPU's idle
# task comes before init.
#
# Run from the repo root, after make c4bb-images. `make test-c4ix-smp`
# runs it at 1, 2 and 4 CPUs and at 2 on threads.

N=${1:-2}
ENGINE="--cpus $N $2"
LIBJS="node libjs/cli.js $ENGINE"
IMAGES=src/c4bb/images
U=$((4 + N - 1))                  # the root archive's unpack, task 4 on one CPU
[ "$N" -lt 2 ] && U=4
TAG="test-c4ix-smp($N${2:+ parallel})"
fail=0
ok ()  { echo "$TAG: $1 OK"; }
bad () { echo "$TAG: $1 FAIL"; fail=1; }
c4ix () { timeout ${2:-300} $LIBJS -m ${3:-64} -c ${1} -d $IMAGES/disk $IMAGES/c4ix32.c4r 2>/dev/null; }

# (shutdown waits for the background unpack: the budget is for both)
out=$(printf 'c4ix-ps.c4r\nexit\n' | c4ix 2000000000)
if echo "$out" | grep -qF "protected mode on for user tasks, preemption on" && echo "$out" | grep -qF "tasks," &&
   echo "$out" | grep -qF "shutdown complete, 0 tasks remain" && ! echo "$out" | grep -qE "cache .task. .*live [1-9]|panic|illegal"; then
    ok "boots, runs ps, and shuts down with every task freed"
else bad "boots, runs ps, and shuts down with every task freed"; echo "$out" | grep -E 'processors|tasks|panic|illegal|live|shutdown' | head -8; fi
# the layout: the kernel image, its arena, the heap, the guarded boot
# stack (sched.c, sched_banner), each [mem FIRST-LAST]
mem () { echo "$out" | grep -qE "^  \[mem 0x[0-9a-f]{8}-0x[0-9a-f]{8}\] $1"; }
if echo "$out" | grep -qxF 'c4ix: 64 MB of memory:' && mem 'Kernel image \(' && mem 'Kernel arena - ' &&
   mem 'Heap - ' && mem 'Boot stack, guarded \(1024 KB\)'; then
    ok "the boot says the memory and its layout"
else bad "the boot says the memory and its layout"; echo "$out" | grep -E 'memory|\[mem' | head -6; fi
if [ "$N" -ge 2 ]; then
    if [ -n "$2" ]; then how="a thread each, all at once"; else how="simulated, taking turns on one thread"; fi
    if echo "$out" | grep -qxF "c4ix: $N processors" && echo "$out" | grep -qxF "  $how" &&
       echo "$out" | grep -qxF "  CPUs share all memory and the heap" &&
       [ "$(echo "$out" | grep -cE ' idle[0-9]+$')" = $((N - 1)) ]; then
        ok "starts $N processors, an idle task each, and says how they run"
    else bad "starts $N processors, an idle task each, and says how they run"; echo "$out" | grep -E 'processors|idle' | head -6; fi
fi

# The shell's wildcards, and rm -r: what a rebuild from clean needs
out=$(printf 'mkdir /tmp/g\nmkdir /tmp/g/sub\necho a > /tmp/g/a.txt\necho b > /tmp/g/b.txt\necho c > /tmp/g/c.c\necho d > /tmp/g/sub/d.txt\necho /tmp/g/*.txt /tmp/g/?.c /tmp/g/*.none\nrm /tmp/g/*.txt\nls /tmp/g\nrm /tmp/g\nrm -r /tmp/g\nls /tmp\nexit\n' | c4ix 400000000)
if echo "$out" | grep -qxF "/tmp/g/a.txt /tmp/g/b.txt /tmp/g/c.c /tmp/g/*.none" &&
   echo "$out" | grep -qxF "rm: /tmp/g is a directory with things in it: rm -r" &&
   echo "$out" | grep -qxF "(/tmp is empty)" && echo "$out" | grep -qx "c.c" && echo "$out" | grep -qx "sub/" &&
   ! echo "$out" | grep -qxE "a.txt|b.txt"; then
    ok "wildcards expand, sorted; rm takes them, and rm -r a tree"
else bad "wildcards expand, sorted; rm takes them, and rm -r a tree"; echo "$out" | grep -E 'tmp|rm|txt' | head -8; fi

out=$(printf "wait $U\nls /usr/lib\ncat /usr/src/c4ix/user/tar.c\nexit\n" | c4ix 2000000000)
if echo "$out" | grep -qF "unpacking c4ix-root.tgz in the background as task $U" &&
   echo "$out" | grep -qF "[$U] done (0)" && echo "$out" | grep -qF "libarc.c4l" &&
   echo "$out" | grep -qF "list or extract a tar archive, gzipped or not"; then
    ok "the root archive unpacks"
else bad "the root archive unpacks"; echo "$out" | grep -E 'unpack|done|tar:|cannot' | head -5; fi

out=$(printf "wait $U\ncc -o /tmp/hello2.c4r /usr/src/c4ix/examples/hello.c\n/tmp/hello2.c4r\ncc -O -D C4IX_SHARED -l /lib/libc4ix.c4s -o /tmp/echo.c4r /usr/src/c4ix/user/echo.c\n/tmp/echo.c4r cc built it\ncrc32 /tmp/echo.c4r\ncd /usr/src/tests\ncc -O mandel.c\n./mandel.c4r\nexit\n" | c4ix 8000000000)
a=$(echo "$out" | grep -oE '^[0-9a-f]{8} [0-9]+ /tmp/echo.c4r' | cut -d' ' -f1,2)
b=$(python3 -I libjs/tests/c4r-body.py $IMAGES/disk/c4ix-echo.c4r)   # its map aside
if echo "$out" | grep -qF "hello from a program compiled inside C4IX" && echo "$out" | grep -qxF "cc built it" &&
   [ -n "$a" ] && [ "$a" = "$b" ] && echo "$out" | grep -qE "^Mandelbrot rendered in [0-9]+ms"; then
    ok "cc compiles inside C4IX: hello, echo byte for byte, and mandel.c"
else bad "cc compiles inside C4IX: hello, echo byte for byte, and mandel.c"; echo "$out" | grep -E 'cc|crc|hello|mandel|illegal' | head -8; fi

# two compiles at once: on several CPUs, at the same time
C="cc -O /usr/src/tests/mandel.c -o"
out=$(printf "wait $U\n$C /tmp/m1.c4r &\n$C /tmp/m2.c4r\nwait\ncrc32 /tmp/m1.c4r /tmp/m2.c4r\nexit\n" | c4ix 8000000000)
a=$(echo "$out" | grep -oE '^[0-9a-f]{8} [0-9]+ /tmp/m1.c4r' | cut -d' ' -f1,2)
b=$(echo "$out" | grep -oE '^[0-9a-f]{8} [0-9]+ /tmp/m2.c4r' | cut -d' ' -f1,2)
if [ -n "$a" ] && [ "$a" = "$b" ] && ! echo "$out" | grep -qE "did not compile|illegal"; then
    ok "two compiles at once make the same program"
else bad "two compiles at once make the same program"; echo "$out" | grep -E 'cc|crc|illegal|cannot' | head -6; fi

out=$(printf 'libctest -v -n 3 one two\nexit\n' | c4ix 600000000)
if echo "$out" | sed -n '/^printf: \[42/,/libctest: done/p' | cmp -s - src/c4ix/libc/tests/libctest.txt; then
    ok "libc: libctest's output is the reference"
else bad "libc: libctest's output is the reference"
     echo "$out" | sed -n '/^printf: \[42/,/libctest: done/p' | diff - src/c4ix/libc/tests/libctest.txt | head -10; fi

# libc is -fpic: its data slot is a word of the CPU-local window, which
# each CPU points at the data of the task it runs
out=$(printf 'pictest A 15 > /tmp/a &\npictest B 15 > /tmp/b &\npictest C 15 > /tmp/c &\nwait\ncat /tmp/a\ncat /tmp/b\ncat /tmp/c\nexit\n' | c4ix 1200000000)
if echo "$out" | grep -qx "A: ok" && echo "$out" | grep -qx "B: ok" && echo "$out" | grep -qx "C: ok" && ! echo "$out" | grep -qF "crossed"; then
    ok "three tasks share libc's code and keep their own data"
else bad "three tasks share libc's code and keep their own data"; echo "$out" | grep -E 'ok|crossed|loader' | head -6; fi

# S4: ps and top say which CPU runs each task, and top each CPU's load;
# on one CPU, the columns there always were
out=$(printf 'ps\ntop 1\nexit\n' | c4ix 1000000000)
if [ "$N" -ge 2 ]; then
    if echo "$out" | grep -qE '^  ID  PPID STATE    CPU PRIV' && echo "$out" | grep -qE "^CPUs:  0:[0-9]+% 1:[0-9]+%"; then
        ok "ps and top show the CPUs"
    else bad "ps and top show the CPUs"; echo "$out" | grep -E 'ID|CPUs' | head -4; fi
else
    if echo "$out" | grep -qE '^  ID  PPID STATE   PRIV' && ! echo "$out" | grep -qE 'CPUs:'; then
        ok "ps and top: one CPU, no CPU column"
    else bad "ps and top: one CPU, no CPU column"; echo "$out" | grep -E 'ID|CPUs' | head -4; fi
fi

# nice: on one CPU a favoured program (nice -3: a turn of four slices)
# gets several times the cycles of a plain one beside it
if [ "$N" -eq 1 ]; then
    out=$(printf "wait $U\nspin 300 &\nnice -n -3 spin 300 &\nps\nps\nps\nps\nps\nps\nexit\n" | c4ix 2500000000)
    cyc() { echo "$out" | grep -E "^ +$1 +[0-9]+ .* spin$" | tail -1 |
            awk '{v=$(NF-1); m=1; if (v ~ /k$/) m=1e3; if (v ~ /M$/) m=1e6; if (v ~ /G$/) m=1e9; sub(/[kMG]$/,"",v); printf "%d", v*m}'; }
    a=$(cyc 6); b=$(cyc 9)
    if [ -n "$a" ] && [ -n "$b" ] && [ "$b" -gt $((a * 3)) ]; then
        ok "nice: a favoured program gets more of the CPU"
    else bad "nice: a favoured program gets more of the CPU"; echo "$out" | grep -E 'spin|nice' | tail -4; fi
fi

# pin: a program put on the last CPU runs there, and pin says what it can.
# ps is pinned to CPU 0, or on two CPUs it would be on spin's while it looks
if [ "$N" -ge 2 ]; then
    L=$((N - 1))
    out=$(printf "wait $U\npin $L spin 8 &\npin 0 ps\npin 0 ps\npin 0 ps\npin 0 ps\npin 7x echo no\nexit\n" | c4ix 1500000000)
    runs=$(echo "$out" | grep -E "^ +[0-9]+ +[0-9]+ run +[0-9]+ user .* spin$")
    if [ -n "$runs" ] && ! echo "$runs" | grep -qvE " run +$L user " &&
       echo "$out" | grep -qF "usage: pin CPUS COMMAND"; then
        ok "pin puts a program on a CPU"
    else bad "pin puts a program on a CPU"; echo "$out" | grep -E 'spin|pin' | head -6; fi
fi

# b4ke -j: the libraries rebuilt with steps at once are the disk's, byte for
# byte up to the source map cc does not write yet (c4r-body.py).
# 128 MB: N compilers at once, beside the unpacked root archive, take more than 64.
out=$(printf "wait $U\nb4ke -B -j $((N > 1 ? N : 2)) -C /usr/src/c4ix/lib\ncrc32 /build/lib/libc4ix.c4l /build/lib/libarc.c4l\nexit\n" | c4ix 40000000000 900 128)
a=$(echo "$out" | grep -oE '^[0-9a-f]{8} [0-9]+ /build/lib/libc4ix.c4l' | cut -d' ' -f1,2)
b=$(python3 -I libjs/tests/c4r-body.py $IMAGES/libc4ix32.c4l)
c=$(echo "$out" | grep -oE '^[0-9a-f]{8} [0-9]+ /build/lib/libarc.c4l' | cut -d' ' -f1,2)
d=$(python3 -I libjs/tests/c4r-body.py $IMAGES/libarc32.c4l)
if echo "$out" | grep -qF "c4ix/lib: 13 ran, 0 up to date, 0 failed" && [ -n "$a" ] && [ "$a" = "$b" ] && [ -n "$c" ] && [ "$c" = "$d" ]; then
    ok "b4ke -j rebuilds the libraries, byte for byte"
else bad "b4ke -j rebuilds the libraries, byte for byte"; echo "$out" | grep -E 'b4ke|crc|failed' | head -8; fi

out=$(printf 'unixtest\nexit\n' | c4ix 1000000000)
if echo "$out" | sed -n '/^lseek: set/,/unixtest: done/p' | cmp -s - src/c4ix/libc/tests/unixtest.txt; then
    ok "unixtest: vfork, execve, waitpid, lseek, fstat, links and truncate"
else bad "unixtest: vfork, execve, waitpid, lseek, fstat, links and truncate"
     echo "$out" | sed -n '/^lseek: set/,/unixtest: done/p' | diff - src/c4ix/libc/tests/unixtest.txt | head -10; fi

if [ $fail = 0 ]; then echo "$TAG: OK"; else echo "$TAG: FAILURES"; exit 1; fi
