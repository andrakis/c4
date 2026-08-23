#!/usr/bin/env bash
# test-ladder.sh - the whole HOMEWARD ladder, end to end.
#
# DELIBERATELY NOT IN `make test-c4bb`. It compiles an operating system
# inside a virtual machine and takes minutes. Run it by hand, or when
# something in the DOS->C4KE handover changes:
#
#     bash src/c4bb/tests/test-ladder.sh
#
# The rungs it walks, and what each one PROVES:
#
#   1. C4DOS boots the recovery disk, which has no init.c4r on it.
#   2. BUILD: dostar unpacks the kernel sources onto the RAM disk, cpp
#      preprocesses, c4cc compiles c4ke.c4r AND init.c4r -- all of it
#      into RAM, because nothing in this machine has a write syscall.
#   3. dosload hands DOS's 4MB scratch back and starts the kernel with
#      a command line of its own.
#   4. The kernel's DOS extension copies the whole RAM disk into its own
#      RAM filesystem and reaches a shell -- running the init that was
#      compiled in step 2 and exists ONLY in memory. That is the rung:
#      before this, the kernel booted the prebuilt init off the floppy
#      and everything the machine had just built was invisible to it.
#   5. Under C4KE, a compiler and a LINKER both work in memory: two
#      objects compiled to the RAM filesystem, linked from it, and the
#      result executed out of it. That is what building C4IX needs --
#      it is twelve objects plus a library archive.
#   6. c4lc, running on c4sp, compiles a real C4IX kernel module into
#      the RAM filesystem, and c4rlink reads that object BACK out of
#      memory and writes a library from it. The toolchain C4IX is
#      actually built with runs on the machine that will run it.
#
# What this does NOT claim: C4IX is not booted here. Its userland links
# against libc4ix and runs under C4IX, not under C4KE, so "compile it,
# link it, run it" cannot be one sentence at this rung. Steps 5 and 6
# are the two halves of the claim that it CAN be built here.

set -e
cd "$(dirname "$0")/../../.."
R="$PWD"

for t in c4m c4 cpp c4cc c4sp; do
    [ -x ./$t ] || { echo "test-ladder: run 'make $t' first"; exit 2; }
done

T=.ladder_test_tmp
rm -rf $T
mkdir -p $T/disk
trap 'rm -rf $T' EXIT

echo "test-ladder: building the pieces..."
make c4dos-clock.c4r dostar.c4r cpp.c4r c4cc.c4r dosload.c4r c4ke-src.tar \
     c4sh.c4r c4sp.c4r c4rlink.c4r c4ke.c4r \
     src/tests/test_ramlink.c4r src/tests/test_ixbuild.c4r > /dev/null
cp src/tests/test_ramlink.c4r src/tests/test_ixbuild.c4r .

# ---- rung 1: the recovery floppy -------------------------------------
# No init.c4r. If a kernel reaches a shell later in this script, the
# init it ran can only have come out of memory. Put init.c4r back here
# and this test stops proving the thing it exists to prove.
sed 's/SIZE=[0-9]*/SIZE=16777216/' src/c4dos/fs/CONFIG.SYS > $T/disk/config.sys
cp src/c4dos/fs/AUTOEXEC.BAT $T/disk/autoexec.bat
cp src/c4dos/fs/BUILD.BAT    $T/disk/build.bat
cp dostar.c4r cpp.c4r c4cc.c4r dosload.c4r c4ke-src.tar c4sh.c4r $T/disk/
(cd $T/disk && ls -p | grep -v '/$' > c4dos.dir)
[ -f $T/disk/init.c4r ] && { echo "FAIL: init.c4r is on the floppy, the test proves nothing"; exit 1; }

# ---- rungs 2-4: BUILD, dosload, and the handover ---------------------
# The kernel comes up and waits at its shell, so timeout is the expected
# way for this to end; stdbuf keeps the output, because a block-buffered
# stdout killed by SIGTERM never flushes and looks like silence.
echo "test-ladder: C4DOS is building C4KE (slow)..."
# The timeout is the EXPECTED end, not a failure: the kernel reaches its
# shell and waits there forever. It cannot be told to quit -- DOS reads
# the whole pipe into its own line buffer before the kernel starts, so a
# trailing '\q' never reaches c4sh. 360s is the observed run (~2min to
# build, seconds to boot) with room to spare; the assertions below judge
# what it printed, not how it ended.
OUT=$( cd $T/disk && printf 'BUILD\nMEM\nRUN dosload.c4r c4ke.c4r\n' \
    | timeout 360 stdbuf -o0 "$R/c4m" "$R/load-c4r.c" -- "$R/c4dos-clock.c4r" 2>&1 || true )
echo "$OUT" > $T/ladder.log

grep -q "dostar: extracted" $T/ladder.log       || { echo "FAIL rung 2: unpack"; tail -20 $T/ladder.log; exit 1; }
grep -q "cpp: wrote .* to ram:c4ke.i" $T/ladder.log     || { echo "FAIL rung 2: preprocess"; exit 1; }
grep -q "c4cc: wrote .* to ram:c4ke.c4r" $T/ladder.log  || { echo "FAIL rung 2: compile kernel"; exit 1; }
grep -q "c4cc: wrote .* to ram:init.c4r" $T/ladder.log  || { echo "FAIL rung 2: compile init"; exit 1; }
grep -q "dosload: .* bytes released, loading c4ke.c4r" $T/ladder.log \
    || { echo "FAIL rung 3: dosload did not release DOS's scratch"; exit 1; }

SEED=$(sed -n 's/.*seeded \([0-9]*\)\/\([0-9]*\) file(s).*/\1 \2/p' $T/ladder.log | head -1)
[ -n "$SEED" ] || { echo "FAIL rung 4: the kernel never seeded from the RAM disk"; exit 1; }
read SEED_N SEED_TOT <<< "$SEED"
[ "$SEED_N" -gt 0 ] && [ "$SEED_N" = "$SEED_TOT" ] \
    || { echo "FAIL rung 4: partial seed ($SEED_N/$SEED_TOT) - RAMFS_MAX or memory?"; exit 1; }
grep -q "Kernel ready" $T/ladder.log || { echo "FAIL rung 4: kernel did not come up"; exit 1; }
grep -q "C4SH" $T/ladder.log         || { echo "FAIL rung 4: no shell, so no init ran"; exit 1; }
echo "test-ladder: rungs 1-4 OK (C4DOS built C4KE, seeded $SEED_N files, booted its own init)"

# ---- rung 5: a linker that works in memory ---------------------------
./c4m load-c4r.c -- c4ke.c4r -v 0 test_ramlink > $T/ramlink.log 2>&1 || true
grep -q "c4rlink: wrote .* to ramfs:ramlinked.c4r" $T/ramlink.log \
    || { echo "FAIL rung 5: c4rlink did not write to the RAM filesystem"; tail -10 $T/ramlink.log; exit 1; }
grep -q "b_add(3, 4) = 7" $T/ramlink.log \
    || { echo "FAIL rung 5: the linked image did not run correctly"; tail -10 $T/ramlink.log; exit 1; }
echo "test-ladder: rung 5 OK (compiled, linked and ran, all in the RAM filesystem)"

# ---- rung 6: the C4IX toolchain, in the machine ----------------------
# About a minute and a half: c4lc on c4sp is the slow part, and one
# module is enough to answer whether the path works at all.
echo "test-ladder: c4lc is compiling a C4IX module inside C4KE (slow)..."
./c4m load-c4r.c -- c4ke.c4r -v 0 test_ixbuild > $T/ixbuild.log 2>&1 || true
grep -q "object is .* bytes of RAM filesystem" $T/ixbuild.log \
    || { echo "FAIL rung 6: c4lc produced no object in the RAM filesystem"; tail -20 $T/ixbuild.log; exit 1; }
grep -q "c4rlink: wrote .* to ramfs:ixboot.c4l" $T/ixbuild.log \
    || { echo "FAIL rung 6: c4rlink could not read the object back"; tail -20 $T/ixbuild.log; exit 1; }
grep -q "ixbuild: library is .* ok" $T/ixbuild.log \
    || { echo "FAIL rung 6: no library produced"; tail -20 $T/ixbuild.log; exit 1; }
echo "test-ladder: rung 6 OK (c4lc compiled a C4IX module, c4rlink read it back)"

echo "test-ladder: OK"
