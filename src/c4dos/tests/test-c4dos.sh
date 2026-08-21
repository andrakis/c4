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

# ---- the test floppy ------------------------------------------------
cp src/c4dos/fs/CONFIG.SYS $T/fs/config.sys
cp src/c4dos/fs/AUTOEXEC.BAT $T/fs/autoexec.bat
printf 'CONFIG.SYS\nAUTOEXEC.BAT\nC4DOS.DIR\nHELLO.C4R\n' > $T/fs/c4dos.dir
cp src/tests/hello.c4r $T/fs/hello.c4r

R="$PWD"
run_native () { # image, script on stdin
  ( cd $T/fs && printf "$2" | "$R/c4m" "$R/load-c4r.c" -- "$R/$T/$1" 2>&1 )
}

# ---- leg 1: native session ------------------------------------------
OUT=$(run_native c4dos.c4r 'DIR\nTYPE c4dos.dir\nRUN hello.c4r\nECHO back at the prompt\nBADCMD\nEXIT\n')
echo "$OUT" | grep -q "C4DOS version"            || { echo "FAIL native: no banner"; exit 1; }
echo "$OUT" | grep -q "Welcome to C4DOS"         || { echo "FAIL native: autoexec did not run"; exit 1; }
echo "$OUT" | grep -q "HELLO.C4R"                || { echo "FAIL native: DIR"; exit 1; }
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
