#!/bin/bash
# test-c4ix-smp.sh N [--parallel] -- C4IX on N processors (docs/c4ix-smp.md
# S3): the simulated engine, or with --parallel a thread per CPU.
#
# test-libjs's C4IX checks, the ones that say whether the kernel holds
# together with several CPUs in it: boot and ps, the root archive's
# unpack, cc inside C4IX (mandel.c; echo built byte for byte as the
# disk's), two compiles at once, libc, two tasks sharing libc's code
# (its data slot is in the CPU-local window), and unixtest's vfork,
# execve and waitpid. Every task id is N - 1 higher than on one CPU:
# each other CPU's idle task comes before init.
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
c4ix () { timeout ${2:-300} $LIBJS -m 64 -c ${1} -d $IMAGES/disk $IMAGES/c4ix32.c4r 2>/dev/null; }

# (shutdown waits for the background unpack: the budget is for both)
out=$(printf 'c4ix-ps.c4r\nexit\n' | c4ix 2000000000)
if echo "$out" | grep -qF "protected mode on for user tasks, preemption on" && echo "$out" | grep -qF "tasks," &&
   echo "$out" | grep -qF "shutdown complete, 0 tasks remain" && ! echo "$out" | grep -qE "live [1-9].*|panic|illegal"; then
    ok "boots, runs ps, and shuts down with every task freed"
else bad "boots, runs ps, and shuts down with every task freed"; echo "$out" | grep -E 'processors|tasks|panic|illegal|live|shutdown' | head -8; fi
if [ "$N" -ge 2 ]; then
    if echo "$out" | grep -qxF "c4ix: $N processors" && [ "$(echo "$out" | grep -cE ' idle[0-9]+$')" = $((N - 1)) ]; then
        ok "starts $N processors, an idle task each"
    else bad "starts $N processors, an idle task each"; echo "$out" | grep -E 'processors|idle' | head -6; fi
fi

out=$(printf "wait $U\nls /usr/lib\ncat /usr/src/c4ix/user/tar.c\nexit\n" | c4ix 2000000000)
if echo "$out" | grep -qF "unpacking c4ix-root.tgz in the background as task $U" &&
   echo "$out" | grep -qF "[$U] done (0)" && echo "$out" | grep -qF "libarc.c4l" &&
   echo "$out" | grep -qF "list or extract a tar archive, gzipped or not"; then
    ok "the root archive unpacks"
else bad "the root archive unpacks"; echo "$out" | grep -E 'unpack|done|tar:|cannot' | head -5; fi

out=$(printf "wait $U\ncc -o /tmp/hello2.c4r /usr/src/c4ix/examples/hello.c\n/tmp/hello2.c4r\ncc -O -D C4IX_SHARED -l /lib/libc4ix.c4s -o /tmp/echo.c4r /usr/src/c4ix/user/echo.c\n/tmp/echo.c4r cc built it\ncrc32 /tmp/echo.c4r /bin/echo\ncd /usr/src/tests\ncc -O mandel.c\n./mandel.c4r\nexit\n" | c4ix 8000000000)
a=$(echo "$out" | grep -oE '^[0-9a-f]{8} [0-9]+ /tmp/echo.c4r' | cut -d' ' -f1,2)
b=$(echo "$out" | grep -oE '^[0-9a-f]{8} [0-9]+ /bin/echo' | cut -d' ' -f1,2)
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

out=$(printf 'unixtest\nexit\n' | c4ix 1000000000)
if echo "$out" | sed -n '/^lseek: set/,/unixtest: done/p' | cmp -s - src/c4ix/libc/tests/unixtest.txt; then
    ok "unixtest: vfork, execve, waitpid, lseek, fstat, links and truncate"
else bad "unixtest: vfork, execve, waitpid, lseek, fstat, links and truncate"
     echo "$out" | sed -n '/^lseek: set/,/unixtest: done/p' | diff - src/c4ix/libc/tests/unixtest.txt | head -10; fi

if [ $fail = 0 ]; then echo "$TAG: OK"; else echo "$TAG: FAILURES"; exit 1; fi
