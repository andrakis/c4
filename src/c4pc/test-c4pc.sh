#!/bin/bash
# test-c4pc.sh -- C4IX on c4pc against libjs (make test-c4pc).
#
# Then test-c4pc-progs.sh: test-libjs's native-oracle programs on both.
# The session is test-libjs's boot check (ps, then exit), run with no
# accelerators, since c4pc has none: libjs, c4pc-host and, where QEMU is
# installed, c4pc.elf on a bare (emulated) PC. Each must print what the
# others print, byte for byte, and take the same number of cycles.
#   QEMU=path   the emulator (default: qemu-system-i386 on PATH, or
#               ~/opt/pctools/bin/qemu-system-i386)
#   NO_QEMU=1   only libjs and c4pc-host
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

timeout 900 node libjs/cli.js -s --no-accel --no-osaccel -m 64 -c $CYC -d $IMAGES/disk $W/c4ix32.c4r \
  < $W/in.txt > $W/libjs.out 2> $W/libjs.err
st=$?
[ $st = 0 ] && ok "libjs: the session ends with status 0" || bad "libjs: status $st"
timeout 600 ./c4pc-host -s -m 64 -c $CYC -d $IMAGES/disk $W/c4ix32.c4r < $W/in.txt > $W/host.out 2> $W/host.err
st=$?
[ $st = 0 ] && ok "c4pc-host: the session ends with status 0" || bad "c4pc-host: status $st"
for want in "C4IX booting" "c4ix-sh" "c4ix-ps.c4r" "tasks," "shutdown complete"; do
  grep -qF "$want" $W/host.out && ok "c4pc-host prints '$want'" || bad "c4pc-host prints '$want'"
done
if cmp -s $W/libjs.out $W/host.out; then ok "c4pc-host's output is libjs's, byte for byte"
else bad "c4pc-host's output differs from libjs's:"; diff $W/libjs.out $W/host.out | head -20; fi
cl=$(grep -o '^libjs: [0-9]* cycles' $W/libjs.err | grep -o '[0-9]*')
ch=$(sed -n 's/^c4pc: \([0-9]*\) cycles.*/\1/p' $W/host.err)
[ -n "$cl" ] && [ "$cl" = "$ch" ] && ok "the same cycles on both ($cl)" || bad "cycles: libjs $cl, c4pc-host $ch"
grep -h 'inst/s' $W/libjs.err $W/host.err | sed 's/^/        /'
# test-libjs's programs with a native oracle, on both
progs=$(IMAGES=$IMAGES bash src/c4pc/test-c4pc-progs.sh); r=$?
echo "$progs" | grep -v '^test-c4pc-progs: '
[ $r = 0 ] || fail=1

Q=${QEMU:-$(command -v qemu-system-i386 || echo ~/opt/pctools/bin/qemu-system-i386)}
if [ -n "${NO_QEMU:-}" ] || [ ! -x "$Q" ]; then
  echo "  skip  c4pc.elf in QEMU (no qemu-system-i386)"
else
  rm -f $W/serial.txt
  timeout 1800 "$Q" -m 512 -display none -no-reboot -serial file:$W/serial.txt \
    -device isa-debug-exit,iobase=0xf4,iosize=4 -kernel c4pc.elf \
    -append "-s -m 64 -c $CYC" -initrd "$W/c4ix32.c4r,$W/disk.tar drive,$W/in.txt stdin"
  st=$?
  # isa-debug-exit: QEMU exits (status << 1) | 1
  [ $st = 1 ] && ok "c4pc.elf in QEMU: the session ends with status 0" || bad "c4pc.elf in QEMU: QEMU's status $st"
  tr -d '\r' < $W/serial.txt | grep -v '^c4pc: ' > $W/qemu.out
  if cmp -s $W/libjs.out $W/qemu.out; then ok "c4pc.elf's output in QEMU is libjs's, byte for byte"
  else bad "c4pc.elf's output in QEMU differs from libjs's:"; diff $W/libjs.out $W/qemu.out | head -20; fi
  cq=$(tr -d '\r' < $W/serial.txt | sed -n 's/^c4pc: \([0-9]*\) cycles.*/\1/p')
  [ "$cl" = "$cq" ] && ok "the same cycles in QEMU ($cq)" || bad "cycles: libjs $cl, QEMU $cq"
  tr -d '\r' < $W/serial.txt | grep 'inst/s' | sed 's/^/        /'
fi
[ $fail = 0 ] && echo "test-c4pc: OK" || { echo "test-c4pc: FAILED"; exit 1; }
