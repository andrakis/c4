#!/usr/bin/env bash
# test-c4dos.sh - C4DOS's gates (see docs/c4dos-design.md).
#
# Four legs, each a scripted session:
#   1. NATIVE:   ./c4m load-c4r.c -- c4dos.c4r    (fast, the dev loop)
#   2. CLOCK:    the -DC4DOS_CLOCK build answers TIME with an uptime
#   3. THE TOWER: ./c4 c4l.c c4dos.c4r - the PURITY pin: original,
#      unmodified c4 runs DOS, DOS runs hello.c4r. Proves the image
#      contains nothing above EXIT (c4l refuses otherwise, by name).
#   4. THE MACHINE: c4bb boots c4dos32, DIR/RUN/TIME work, and the
#      layers stack: c4.c4r (the original interpreter, as a DOS
#      transient) compiles and runs hello.c INSIDE the simulated
#      computer, then hands the prompt back.
#
# Run from the repo root: bash src/c4dos/tests/test-c4dos.sh

set -e
cd "$(dirname "$0")/../../.."

[ -x ./cpp ]  || { echo "test-c4dos: run 'make cpp' first"; exit 2; }
[ -x ./c4cc ] || { echo "test-c4dos: run 'make c4cc' first"; exit 2; }
[ -x ./c4m ]  || { echo "test-c4dos: run 'make c4m' first"; exit 2; }
[ -x ./c4 ]   || { echo "test-c4dos: run 'make c4' first"; exit 2; }

T=.c4dos_test_tmp
rm -rf $T
mkdir -p $T/fs
trap 'rm -rf $T' EXIT

# ---- builds (both clock variants, through our own cpp - raw c4cc
# would skip the #if lines and compile both branches in) --------------
./cpp src/c4dos/c4dos.c > $T/pp.c
./c4cc -o $T/c4dos.c4r $T/pp.c > /dev/null
./cpp -DC4DOS_CLOCK=1 src/c4dos/c4dos.c > $T/ppck.c
./c4cc -o $T/c4dos-clock.c4r $T/ppck.c > /dev/null

# raycast's C4DOS build: c4lc with -D RC_DOS=1, which swaps puts for
# printf and stubs out the cycle counter and usleep, leaving an image
# that uses nothing above EXIT except TIME -- exactly what a DOS with
# DEVICE=CLOCK.SYS provides. Skipped if c4sp is not built; it is not a
# prerequisite of the rest of this script.
HAVE_RAYCAST=0
if [ -x ./c4sp ]; then
    ./c4sp -c 16000000 src/c4sp/lisp/c4lc.lisp -O -conforming -D RC_DOS=1 \
        src/tests/raycast.c $T/raycast.c4r > /dev/null && HAVE_RAYCAST=1
fi

# ---- the test floppy ------------------------------------------------
cp src/c4dos/fs/CONFIG.SYS $T/fs/config.sys
cp src/c4dos/fs/AUTOEXEC.BAT $T/fs/autoexec.bat
# Real spellings, not shouted: dir_resolve opens the name it finds in
# this listing, so an entry of HELLO.C4R would name a file that is not
# there and defeat the very lookup it exists to serve.
printf 'config.sys\nautoexec.bat\nc4dos.dir\nhello.c4r\nraycast.c4r\ndosenum.c4r\n' > $T/fs/c4dos.dir
cp src/tests/hello.c4r $T/fs/hello.c4r
# dosenum is the v2 API's pin: the only thing here that ENUMERATES the
# RAM disk rather than naming a file. The kernel extension that seeds
# C4KE reads exactly these slots, so if this leg is green that path
# has a working table under it.
./c4cc -o $T/fs/dosenum.c4r include/c4dos.h src/tests/dosenum.c > /dev/null
[ "$HAVE_RAYCAST" = 1 ] && cp $T/raycast.c4r $T/fs/raycast.c4r

R="$PWD"
run_native () { # image, script on stdin
  ( cd $T/fs && printf "$2" | "$R/c4m" "$R/load-c4r.c" -- "$R/$T/$1" 2>&1 )
}

# ---- leg 1: native session ------------------------------------------
OUT=$(run_native c4dos.c4r 'DIR\nTYPE c4dos.dir\nRUN hello.c4r\nECHO back at the prompt\nBADCMD\nEXIT\n')
echo "$OUT" | grep -q "C4DOS version"            || { echo "FAIL native: no banner"; exit 1; }
echo "$OUT" | grep -q "Welcome to C4DOS"         || { echo "FAIL native: autoexec did not run"; exit 1; }
echo "$OUT" | grep -q "hello.c4r"                || { echo "FAIL native: DIR"; exit 1; }
echo "$OUT" | grep -q "yello"                    || { echo "FAIL native: RUN hello"; exit 1; }
echo "$OUT" | grep -q "back at the prompt"       || { echo "FAIL native: no return to prompt"; exit 1; }
echo "$OUT" | grep -q "bad command or file name" || { echo "FAIL native: bad-command"; exit 1; }
echo "$OUT" | grep -q "no clock hardware"        || { echo "FAIL native: clockless build has a clock?"; exit 1; }
echo "test-c4dos: native session OK"

# ---- leg 2: the clock build -----------------------------------------
OUT=$(run_native c4dos-clock.c4r 'TIME\nEXIT\n')
echo "$OUT" | grep -q "clock device installed"   || { echo "FAIL clock: CONFIG.SYS DEVICE line"; exit 1; }
echo "$OUT" | grep -q "uptime"                   || { echo "FAIL clock: TIME"; exit 1; }
echo "test-c4dos: clock build OK"

# ---- leg 2b: a transient that draws ---------------------------------
# raycast is the one transient here that does real work: it renders two
# frames of 256-colour ANSI, reads the clock for its frame counter, and
# returns to the prompt. It also pins the RC_DOS build honestly -- if
# anyone reaches for puts, __c4_cycles or __c4_usleep in that source,
# the image gains an opcode DOS has no business executing.
if [ "$HAVE_RAYCAST" = 1 ]; then
    OUT=$(run_native c4dos-clock.c4r 'RUN raycast.c4r 15x15 -s 3 -d -n 2 -g 40x12\nECHO prompt is back\nEXIT\n')
    echo "$OUT" | grep -q "48;5;"           || { echo "FAIL raycast: no colour output"; exit 1; }
    echo "$OUT" | grep -q "prompt is back"  || { echo "FAIL raycast: no return to prompt"; exit 1; }
    [ "$(echo "$OUT" | grep -c '48;5;')" = 24 ] || { echo "FAIL raycast: expected 24 rendered rows"; exit 1; }
    echo "test-c4dos: raycast transient OK"
else
    echo "test-c4dos: raycast transient SKIPPED (no ./c4sp)"
fi

# ---- leg 2c: names ---------------------------------------------------
# DOS was case-insensitive and let you type the program rather than the
# file. Both are resolved through c4dos.dir, which is why that listing
# has to carry the TRUE on-disk spellings: dir_resolve opens whatever
# it finds there. A disk with no listing keeps the old exact-match
# behaviour, so this also checks the fallback does not become required.
OUT=$(run_native c4dos-clock.c4r 'HELLO\nRUN hello\nRUN HELLO.C4R\nRUN Hello.C4r\nTYPE CONFIG.SYS\nEXIT\n')
[ "$(echo "$OUT" | grep -c yello)" = 4 ] || { echo "FAIL names: expected 4 runs, got $(echo "$OUT" | grep -c yello)"; exit 1; }
echo "$OUT" | grep -q "DEVICE=CLOCK.SYS"  || { echo "FAIL names: TYPE CONFIG.SYS"; exit 1; }
OUT=$(run_native c4dos-clock.c4r 'nosuchthing\nRUN nosuchthing\nEXIT\n')
echo "$OUT" | grep -q "bad command or file name" || { echo "FAIL names: unknown word"; exit 1; }
echo "$OUT" | grep -q "file not found"           || { echo "FAIL names: RUN of a missing program"; exit 1; }
echo "test-c4dos: case-insensitive names and .c4r completion OK"

# ---- leg 2d: the RAM disk -------------------------------------------
# c4 and c4m have no write primitive and c4bb's disk is read-only, so
# "writing a file" is a DOS service: DEVICE=RAMDISK.SYS installs a
# name->buffer table, opens check it BEFORE the disk, and COPY is the
# one builtin that puts something in it. RUN out of RAM is the point --
# it is what lets one stage's output be the next stage's input.
OUT=$(run_native c4dos-clock.c4r 'COPY hello.c4r work.c4r\nDIR\nRUN work.c4r\nEXIT\n')
echo "$OUT" | grep -q "ramdisk device installed" || { echo "FAIL ramdisk: CONFIG.SYS DEVICE line"; exit 1; }
echo "$OUT" | grep -q "work.c4r  <ram>"           || { echo "FAIL ramdisk: DIR does not list it"; exit 1; }
echo "$OUT" | grep -q "yello"                     || { echo "FAIL ramdisk: RUN from RAM"; exit 1; }
# RAM shadows the read-only disk: same name, RAM wins.
OUT=$(run_native c4dos-clock.c4r 'COPY config.sys hello.c\nTYPE hello.c\nEXIT\n')
echo "$OUT" | grep -q "DEVICE=RAMDISK.SYS"        || { echo "FAIL ramdisk: RAM does not shadow disk"; exit 1; }
# and it is still reachable through the name rules
OUT=$(run_native c4dos-clock.c4r 'COPY hello.c4r work.c4r\nWORK\nRUN Work.C4R\nEXIT\n')
[ "$(echo "$OUT" | grep -c yello)" = 2 ] || { echo "FAIL ramdisk: name resolution into RAM"; exit 1; }
echo "test-c4dos: ramdisk OK"

# ---- leg 2e: the v2 API ----------------------------------------------
# Enumeration, and handing memory back. A tool asks for a file by name;
# a LOADER wants everything without being told what is there. The head=
# field is the part that matters -- it proves ENTDATA points at content
# and not at a plausible-looking address, which is exactly the bug the
# v1 table had in slots 9-15 (allocated 16 words, filled 9).
OUT=$(run_native c4dos-clock.c4r 'COPY hello.c4r work.c4r\nCOPY config.sys note.txt\nRUN dosenum.c4r\nEXIT\n')
echo "$OUT" | grep -q "dosenum: api version 2"      || { echo "FAIL v2: version word"; exit 1; }
echo "$OUT" | grep -q "dosenum: 2 entries"          || { echo "FAIL v2: entry count"; exit 1; }
echo "$OUT" | grep -q "work.c4r 434 bytes head=C4R" || { echo "FAIL v2: ENTDATA does not point at the image"; exit 1; }
echo "$OUT" | grep -q "note.txt 81 bytes head=REM"  || { echo "FAIL v2: ENTDATA for a text file"; exit 1; }

# TRIM and RELEASE hand DOS's own memory back -- the thing dosload
# exists to do. DOS must still WORK afterwards: the 4MB scratch is
# re-allocated on demand, so COPY/RUN/TYPE all have to survive it.
OUT=$(run_native c4dos-clock.c4r 'COPY hello.c4r work.c4r\nRUN dosenum.c4r -t -r\nDIR\nCOPY hello.c4r again.c4r\nRUN again.c4r\nTYPE config.sys\nEXIT\n')
echo "$OUT" | grep -q "trim released 4194304 bytes"   || { echo "FAIL v2: trim"; exit 1; }
echo "$OUT" | grep -q "0 entries left"                || { echo "FAIL v2: release"; exit 1; }
echo "$OUT" | grep -q "ramdisk: 0 of .* 0 file(s)"    || { echo "FAIL v2: DIR after release"; exit 1; }
echo "$OUT" | grep -q "yello"                         || { echo "FAIL v2: RUN after trim (scratch not re-allocated?)"; exit 1; }
echo "$OUT" | grep -q "DEVICE=RAMDISK.SYS"            || { echo "FAIL v2: TYPE after trim"; exit 1; }
echo "test-c4dos: API v2 enumeration and memory hand-back OK"

# ---- leg 2f: the ladder ---------------------------------------------
# The claim this whole system exists to support: C4DOS builds C4KE.
# dostar unpacks the sources onto the RAM disk, cpp preprocesses the
# kernel, c4cc compiles it, and the image that comes out boots. The
# kernel is left waiting at its shell, so the run is bounded by timeout
# and judged on what it printed rather than on how it ended.
#
# AND THE HANDOVER. There is deliberately NO init.c4r on this floppy:
# the only one in the machine is the one BUILD.BAT just compiled, and
# it exists only in DOS's RAM disk. If the kernel comes up at a shell,
# c4ke_dos.c seeded that image into the kernel's RAM filesystem and
# task_loadc4r found it there -- which is the whole point of the rung.
# Put init.c4r back on the disk and this leg stops proving anything.
if [ -f c4ke-src.tar ] && [ -f dostar.c4r ] && [ -f cpp.c4r ]; then
    mkdir -p $T/build
    sed 's/SIZE=[0-9]*/SIZE=16777216/' src/c4dos/fs/CONFIG.SYS > $T/build/config.sys
    cp src/c4dos/fs/AUTOEXEC.BAT $T/build/autoexec.bat
    cp src/c4dos/fs/BUILD.BAT $T/build/build.bat
    cp dostar.c4r cpp.c4r c4cc.c4r c4ke-src.tar c4sh.c4r $T/build/
    (cd $T/build && ls -p | grep -v '/$' > c4dos.dir)
    # The kernel comes up and waits at its shell, so timeout is the
    # expected way for this to end -- || true keeps set -e from calling
    # that a failure, and stdbuf keeps the output: a block-buffered
    # stdout killed by SIGTERM never flushes, which looks exactly like
    # a run that printed nothing at all.
    OUT=$( cd $T/build && printf 'BUILD\nRUN c4ke.c4r\n' \
        | timeout 120 stdbuf -o0 "$R/c4m" "$R/load-c4r.c" -- "$R/$T/c4dos-clock.c4r" 2>&1 || true )
    echo "$OUT" | grep -q "dostar: extracted 39 files" || {
        echo "FAIL ladder: unpack"; echo "$OUT" | head -20; exit 1; }
    echo "$OUT" | grep -q "cpp: wrote .* to ram:c4ke.i" || { echo "FAIL ladder: preprocess"; exit 1; }
    echo "$OUT" | grep -q "c4cc: wrote .* to ram:c4ke.c4r" || { echo "FAIL ladder: compile kernel"; exit 1; }
    echo "$OUT" | grep -q "c4cc: wrote .* to ram:init.c4r" || { echo "FAIL ladder: compile init"; exit 1; }
    echo "$OUT" | grep -q "c4ke v0.66 starting" || { echo "FAIL ladder: the built kernel did not start"; exit 1; }
    # N/N, not "some": a shortfall here means RAMFS_MAX or memory ran
    # out, and the symptom would otherwise be a much later mystery.
    SEED=$(echo "$OUT" | sed -n 's/.*seeded \([0-9]*\)\/\([0-9]*\) file(s).*/\1 \2/p' | head -1)
    [ -n "$SEED" ] || { echo "FAIL ladder: the kernel did not seed from the RAM disk"; exit 1; }
    read SEED_N SEED_TOT <<< "$SEED"
    [ "$SEED_N" -gt 0 ] && [ "$SEED_N" = "$SEED_TOT" ] || {
        echo "FAIL ladder: partial seed ($SEED_N/$SEED_TOT)"; exit 1; }
    echo "$OUT" | grep -q "Kernel ready" || { echo "FAIL ladder: kernel did not come up"; exit 1; }
    echo "$OUT" | grep -q "C4SH" || { echo "FAIL ladder: no shell"; exit 1; }
    echo "test-c4dos: the ladder holds (C4DOS built C4KE, seeded it, and booted its own init)"
else
    echo "test-c4dos: ladder SKIPPED (run 'make c4ke-src.tar dostar.c4r cpp.c4r')"
fi

# ---- leg 3: THE TOWER (plain, unmodified c4) ------------------------
OUT=$( cd $T/fs && printf 'RUN hello.c4r\nEXIT\n' | "$R/c4" "$R/c4l.c" "$R/$T/c4dos.c4r" 2>&1 )
echo "$OUT" | grep -q "yello"                    || { echo "FAIL tower: plain c4 could not run DOS+hello"; exit 1; }
echo "$OUT" | grep -q "system halted"            || { echo "FAIL tower: no clean halt"; exit 1; }
echo "test-c4dos: the tower holds (plain c4 -> c4l -> C4DOS -> hello)"

# ---- leg 4: THE MACHINE (c4bb) --------------------------------------
if [ -x ./c4cc32 ] && command -v node > /dev/null; then
  ./cpp -DC4DOS_CLOCK=1 src/c4dos/c4dos.c | ./c4cc32 -o $T/c4dos32.c4r - > /dev/null
  cp src/c4bb/images/hello32.c4r $T/fs/hello32.c4r
  cp src/c4bb/images/disk/c4.c4r $T/fs/c4.c4r
  cp src/c4bb/images/disk/hello.c $T/fs/hello.c
  OUT=$(printf 'DIR\nRUN hello32.c4r\nTIME\nRUN c4.c4r hello.c\nECHO layers hold\nEXIT\n' \
        | node src/c4bb/sim/cli.js -c 500000000 -d $T/fs $T/c4dos32.c4r 2>&1)
  echo "$OUT" | grep -q "clock device installed" || { echo "FAIL c4bb: CONFIG.SYS"; exit 1; }
  echo "$OUT" | grep -q "yello"                  || { echo "FAIL c4bb: RUN hello32"; exit 1; }
  echo "$OUT" | grep -q "uptime 0:00:0"          || { echo "FAIL c4bb: TIME should read boot uptime"; exit 1; }
  echo "$OUT" | grep -q "layers hold"            || { echo "FAIL c4bb: c4-under-DOS did not return"; exit 1; }
  # the nesting really ran: c4.c4r's own exit chatter appears mid-session
  echo "$OUT" | grep -q "exit(0) cycle"          || { echo "FAIL c4bb: no evidence c4.c4r ran hello.c"; exit 1; }
  echo "test-c4dos: the machine boots DOS, and c4-inside-DOS-inside-c4bb runs hello.c"
else
  echo "test-c4dos: SKIP c4bb leg (need c4cc32 + node; run 'make c4bb-32bit')"
fi

echo "test-c4dos: OK"
