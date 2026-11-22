#!/bin/bash
# test-c4pc.sh -- C4IX on c4pc against libjs (make test-c4pc).
#
# Then test-c4pc-mmu.sh, test-libjs's MMU cases, and test-c4pc-progs.sh,
# its native-oracle programs, on both.
# The session is test-libjs's boot check (ps, then exit), on libjs,
# c4pc-host and, where QEMU is installed, c4pc.elf on a bare (emulated) PC.
# Each must print what the others print, byte for byte, and take the same
# number of cycles. It runs three times: with no accelerators, without an
# MMU and with one (--mmu: C4IX's user tasks each in a space of its own,
# mmu.c); and with the MMU and C4IX's own accelerators (accel.c: its
# inflater and CRC-32), where libjs's build lists them.
#   QEMU=path   the emulator (default: qemu-system-i386 on PATH, or
#               ~/opt/pctools/bin/qemu-system-i386)
#   NO_QEMU=1   only libjs and c4pc-host
#   NO_MMU=1    only the session without an MMU
set -u
IMAGES=${IMAGES:-src/c4bb/images}
W=.c4pc
mkdir -p $W
fail=0
ok () { echo "  ok    $*"; }
bad () { echo "  FAIL  $*"; fail=1; }
printf 'c4ix-ps.c4r\nexit\n' > $W/in.txt
CYC=3000000000
cp $IMAGES/c4ix32.c4r $W/c4ix32.c4r
[ -f $W/disk.tar ] || tar --format=ustar -C $IMAGES/disk -cf $W/disk.tar .
Q=${QEMU:-$(command -v qemu-system-i386 || echo ~/opt/pctools/bin/qemu-system-i386)}

# session TAG FLAG [accel]: the session on libjs, c4pc-host and QEMU, FLAG
# --mmu or --no-mmu; with accel, C4IX's own accelerators on both (accel.c)
session () {
  local t=$1 f=$2 st la="--no-accel --no-osaccel" ca="--no-accel --no-osaccel"
  [ "${3:-}" = accel ] && { la=""; ca=""; }
  echo "  ---   the session, $f ${3:-}"
  timeout 900 node libjs/cli.js -s $f $la -m 64 -c $CYC -d $IMAGES/disk $W/c4ix32.c4r \
    < $W/in.txt > $W/libjs$t.out 2> $W/libjs$t.err
  st=$?
  [ $st = 0 ] && ok "libjs: the session ends with status 0" || bad "libjs: status $st"
  timeout 600 ./c4pc-host -s $f $ca -m 64 -c $CYC -d $IMAGES/disk $W/c4ix32.c4r < $W/in.txt > $W/host$t.out 2> $W/host$t.err
  st=$?
  [ $st = 0 ] && ok "c4pc-host: the session ends with status 0" || bad "c4pc-host: status $st"
  for want in "C4IX booting" "c4ix-sh" "c4ix-ps.c4r" "tasks," "shutdown complete"; do
    grep -qF "$want" $W/host$t.out && ok "c4pc-host prints '$want'" || bad "c4pc-host prints '$want'"
  done
  if [ $f = --mmu ]; then
    grep -qF ", MMU" $W/host$t.out && ok "C4IX finds the MMU" || bad "C4IX finds the MMU"
  fi
  if cmp -s $W/libjs$t.out $W/host$t.out; then ok "c4pc-host's output is libjs's, byte for byte"
  else bad "c4pc-host's output differs from libjs's:"; diff $W/libjs$t.out $W/host$t.out | head -20; fi
  cl=$(grep -o '^libjs: [0-9]* cycles' $W/libjs$t.err | grep -o '[0-9]*')
  ch=$(sed -n 's/^c4pc: \([0-9]*\) cycles.*/\1/p' $W/host$t.err)
  [ -n "$cl" ] && [ "$cl" = "$ch" ] && ok "the same cycles on both ($cl)" || bad "cycles: libjs $cl, c4pc-host $ch"
  grep -h 'inst/s' $W/libjs$t.err $W/host$t.err | sed 's/^/        /'

  if [ -n "${NO_QEMU:-}" ] || [ ! -x "$Q" ]; then
    echo "  skip  c4pc.elf in QEMU (no qemu-system-i386)"
    return
  fi
  rm -f $W/serial$t.txt
  timeout 1800 "$Q" -m 512 -display none -no-reboot -serial file:$W/serial$t.txt \
    -device isa-debug-exit,iobase=0xf4,iosize=4 -kernel c4pc.elf \
    -append "-s $f $ca -m 64 -c $CYC" -initrd "$W/c4ix32.c4r,$W/disk.tar drive,$W/in.txt stdin"
  st=$?
  # isa-debug-exit: QEMU exits (status << 1) | 1
  [ $st = 1 ] && ok "c4pc.elf in QEMU: the session ends with status 0" || bad "c4pc.elf in QEMU: QEMU's status $st"
  tr -d '\r' < $W/serial$t.txt | grep -v '^c4pc: ' > $W/qemu$t.out
  if cmp -s $W/libjs$t.out $W/qemu$t.out; then ok "c4pc.elf's output in QEMU is libjs's, byte for byte"
  else bad "c4pc.elf's output in QEMU differs from libjs's:"; diff $W/libjs$t.out $W/qemu$t.out | head -20; fi
  cq=$(tr -d '\r' < $W/serial$t.txt | sed -n 's/^c4pc: \([0-9]*\) cycles.*/\1/p')
  [ "$cl" = "$cq" ] && ok "the same cycles in QEMU ($cq)" || bad "cycles: libjs $cl, QEMU $cq"
  tr -d '\r' < $W/serial$t.txt | grep 'inst/s' | sed 's/^/        /'
}

session "" --no-mmu
if [ -z "${NO_MMU:-}" ]; then
  session -mmu --mmu
  # libjs loads C4IX's accelerators only where its tree's build listed them
  # (src/c4bb/images/accel.json, build-images.sh)
  if grep -q '"os/c4ix"' src/c4bb/images/accel.json 2>/dev/null; then session -accel --mmu accel
  else echo "  skip  the session with C4IX's accelerators (no os/c4ix in src/c4bb/images/accel.json for libjs)"; fi
  # test-libjs's MMU cases (test-c4ix-mmu.mjs), on c4pc-host and libjs
  cases=$(IMAGES=$IMAGES bash src/c4pc/test-c4pc-mmu.sh); r=$?
  echo "$cases" | grep -v '^test-c4pc-mmu: '
  [ $r = 0 ] || fail=1
fi
# The display (gui.c): the desktop booted on c4pc-host with a display
# fitted (C4IX --desktop, as the web page boots it), the Calculator's icon
# double-clicked, and the strings it then draws looked for
echo "  ---   the desktop, on a display"
cat > $W/gui-events.txt <<EV
9000 move 40 330
9100 click 40 330
9250 click 40 330
13000 shot $W/desktop.png
13100 quit
EV
timeout 600 ./c4pc-host -s --mmu -m 128 --gui 1024x768 --events $W/gui-events.txt --texts $W/gui-texts.txt \
  -d $IMAGES/disk $W/c4ix32.c4r --desktop > $W/gui.out 2> $W/gui.err
st=$?
[ $st = 0 ] && ok "c4pc-host with a display: quits at the script's end" || bad "c4pc-host with a display: status $st"
for want in Filesystem Terminal "Text Editor" Calculator Integer Double; do
  grep -q " $want\$\| $want " $W/gui-texts.txt && ok "the display draws '$want'" || bad "the display draws '$want'"
done
grep -h 'display:' $W/gui.err | sed 's/^/        /'
# test-libjs's programs with a native oracle, on both
progs=$(IMAGES=$IMAGES bash src/c4pc/test-c4pc-progs.sh); r=$?
echo "$progs" | grep -v '^test-c4pc-progs: '
[ $r = 0 ] || fail=1
[ $fail = 0 ] && echo "test-c4pc: OK" || { echo "test-c4pc: FAILED"; exit 1; }
