#!/bin/bash
# test-c4pc.sh -- C4IX on c4pc against libjs (make test-c4pc).
#
# Then several CPUs (smp.c, par.c), and test-c4pc-mmu.sh, test-libjs's MMU cases, and test-c4pc-progs.sh,
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
# the disk libjs is given, as one archive (not the ISO's .c4pc/disk.tar, which
# this tree's newer programs are laid over): made again when the images change
[ $W/test-disk.tar -nt $IMAGES/disk/c4ix.vfs.txt ] && [ "$(cat $W/test-disk.from 2>/dev/null)" = "$IMAGES" ] ||
  { tar --format=ustar -C $IMAGES/disk -cf $W/test-disk.tar . && echo "$IMAGES" > $W/test-disk.from; }
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
    -append "--no-cdrom -s $f $ca -m 64 -c $CYC" -initrd "$W/c4ix32.c4r,$W/test-disk.tar drive,$W/in.txt stdin"
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
9100 packet 1 0 1
9150 packet 0 1 0
9250 packet 1 0 1
9300 packet 0 -1 0
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
# The sound card (snd.c, opl.c): Rise of the Triad's timedemo on the bare
# machine with a card fitted (--wav), its OPL music and its samples, the
# accelerators off (this tree's libjs may have none): the
# same WAV from libjs and from c4pc-host, sample for sample. Where the
# images have the game and its data (a full build's).
if [ -f $IMAGES/games/rott.c4r ] && [ -f $IMAGES/games/HUNTBGIN.WAD ]; then
  echo "  ---   the sound card, on Rise of the Triad's timedemo 4"
  top=$PWD
  ( cd $IMAGES/games && timeout 600 node $top/libjs/cli.js --no-accel -m 64 -d . --wav $top/$W/snd-js.wav rott.c4r --timedemo 4 > /dev/null 2>&1 )
  ( cd $IMAGES/games && timeout 600 $top/c4pc-host --no-accel -s -m 64 -d . --wav $top/$W/snd-pc.wav rott.c4r --timedemo 4 > /dev/null 2> $top/$W/snd-pc.err )
  if [ -s $W/snd-js.wav ] && cmp -s $W/snd-js.wav $W/snd-pc.wav; then ok "the same WAV on both ($(stat -c %s $W/snd-pc.wav) bytes)"
  else bad "the WAVs differ"; cmp $W/snd-js.wav $W/snd-pc.wav | head -2; fi
  grep -h 'sound:' $W/snd-pc.err | sed 's/^/        /'
else
  echo "  skip  the sound card (no rott.c4r and its data in $IMAGES/games)"
fi
# A CD to copy from (cdrom.c; C4IX's mount, cp, umount): c4pc-host with a
# disc made here (xorriso: Rock Ridge and Joliet names) as drive 2. The
# disc's files list, read and copy with their own CRCs; umount ejects it.
# Where the images have the programs (a build since they were added) and
# xorriso is on PATH (~/opt/pctools/env.sh on helios).
XORRISO=$(command -v xorriso || true)
if [ -f $IMAGES/disk/c4ix-mount.c4r ] && [ -n "$XORRISO" ]; then
  echo "  ---   a CD to copy from"
  rm -rf $W/cd && mkdir -p $W/cd/src/sub/deeper
  echo "hello from the second CD" > $W/cd/src/README.TXT
  head -c 300000 /dev/urandom > $W/cd/src/sub/deeper/deep.bin
  echo "spaces" > "$W/cd/src/Long File Name.txt"
  "$XORRISO" -as mkisofs -R -J -V TESTDISC -o $W/cd/test.iso $W/cd/src > /dev/null 2>&1
  want=$(python3 -I -c "import zlib,sys; d=open(sys.argv[1],'rb').read(); print('%08x %d' % (zlib.crc32(d), len(d)))" $W/cd/src/sub/deeper/deep.bin)
  printf 'mount cdrom\nls /mnt/cdrom\ncat /mnt/cdrom/README.TXT\nmkdir /home/t\ncp -r /mnt/cdrom/sub /home/t/sub\ncrc32 /mnt/cdrom/sub/deeper/deep.bin /home/t/sub/deeper/deep.bin\ndf\numount\nmount\nexit\n' > $W/cd/in.txt
  timeout 300 ./c4pc-host -m 64 -d $IMAGES/disk --cdrom $W/cd/test.iso $W/c4ix32.c4r < $W/cd/in.txt > $W/cd/out.txt 2>&1
  out=$(cat $W/cd/out.txt)
  echo "$out" | grep -qF "mount: drive 2 (cdrom 'TESTDISC'" && ok "mount cdrom mounts the disc at /mnt/cdrom" || bad "mount cdrom"
  echo "$out" | grep -qxF "Long File Name.txt" && echo "$out" | grep -qxF "hello from the second CD" && ok "its names (Rock Ridge's) and its files" || bad "the disc's names and files"
  [ "$(echo "$out" | grep -cE "^$want /(mnt/cdrom|home/t)/sub/deeper/deep.bin")" = 2 ] && ok "a file read off it and copied off it, byte for byte ($want)" || bad "the copy off the disc"
  echo "$out" | grep -qE "^2:cdrom TESTDISC" && ok "df counts the disc" || bad "df and the disc"
  echo "$out" | grep -qF "umount: /mnt/cdrom let go; drive 2's disc ejected" && echo "$out" | grep -qF "2: cdrom, no disc" && ok "umount lets it go and ejects it" || bad "umount"
else
  echo "  skip  a CD to copy from (no c4ix-mount.c4r in $IMAGES/disk, or no xorriso)"
fi
# Several CPUs (smp.c, par.c). Taking turns (--cpus N): c4mp's own
# transcripts, and the session with the MMU on 4 CPUs byte for byte libjs's
# --cpus 4, in its cycles. On threads (--parallel), and on a PC's own
# processors (c4pc.elf in QEMU -smp 4): what test-libjs asks of par.js --
# the counters, the atomics, parking and IPIs, a deadlock named, the
# CPU-local window -- and C4IX on four of them
echo "  ---   several CPUs"
for g in smp0 smp1; do
  if ./c4pc-host --cpus 4 $IMAGES/c4mp-$g.c4r 2>&1 | cmp -s - src/c4mp/tests/$g-4cpu.txt; then ok "--cpus 4 runs c4mp's $g as c4mp does"
  else bad "--cpus 4 runs c4mp's $g as c4mp does"; fi
done
timeout 900 node libjs/cli.js -s --mmu --cpus 4 -m 64 -c $CYC -d $IMAGES/disk $W/c4ix32.c4r < $W/in.txt > $W/libjs-smp.out 2> $W/libjs-smp.err
timeout 600 ./c4pc-host -s --mmu --cpus 4 -m 64 -c $CYC -d $IMAGES/disk $W/c4ix32.c4r < $W/in.txt > $W/host-smp.out 2> $W/host-smp.err
cl=$(grep -o '^libjs: [0-9]* cycles' $W/libjs-smp.err | grep -o '[0-9]*')
ch=$(sed -n 's/^c4pc: \([0-9]*\) cycles.*/\1/p' $W/host-smp.err)
if grep -qxF "c4ix: 4 processors" $W/host-smp.out && cmp -s $W/libjs-smp.out $W/host-smp.out && [ -n "$cl" ] && [ "$cl" = "$ch" ]; then
  ok "the session on 4 CPUs taking turns, with the MMU: libjs's, byte for byte, in its cycles ($cl)"
else bad "the session on 4 CPUs: c4pc-host's differs from libjs's (cycles $cl, $ch)"; diff $W/libjs-smp.out $W/host-smp.out | head -10; fi
# smp_checks TAG RUN...: the c4mp checks, each program run by RUN N IMAGE
smp_checks () {
  local t=$1 out st; shift
  out=$("$@" 4 $IMAGES/c4mp-smp0.c4r 2>&1)
  echo "$out" | grep -q "smp0: total 48000 across 4 cpu(s), expected 48000" && ok "$t: smp0's counters add up" || bad "$t: smp0's counters"
  out=$("$@" 4 $IMAGES/c4mp-smp1.c4r 2>&1)
  echo "$out" | grep -q "smp1: all OK" && ok "$t: smp1's atomics, parking and IPIs" || { bad "$t: smp1"; echo "$out" | tail -4; }
  out=$("$@" 4 $IMAGES/c4mp-deadlock.c4r 2>&1)
  if echo "$out" | grep -qF "deadlock: every processor is parked and none can be woken" &&
     [ "$(echo "$out" | grep -c "waiting on 0x")" = 4 ]; then ok "$t: a deadlock is diagnosed, each CPU named"
  else bad "$t: the deadlock"; echo "$out" | tail -6; fi
  out=$("$@" 2 $IMAGES/libjs-smpwin.c4r 2>&1)
  echo "$out" | grep -q "smpwin: cpu 1 saw 111, cpu 0 sees 1 and 9" && ok "$t: the CPU-local window" || bad "$t: the CPU-local window"
  out=$("$@" 4 $IMAGES/libjs-spinmp.c4r 2>&1)
  echo "$out" | grep -q "spinmp: 4 cpus each summed 70000000" && ok "$t: four CPUs work at once" || bad "$t: four CPUs at once"
}
host_par () { local n=$1; shift; timeout 120 ./c4pc-host --cpus $n --parallel "$@"; }
smp_checks "--parallel" host_par
if [ -n "${NO_QEMU:-}" ] || [ ! -x "$Q" ]; then
  echo "  skip  c4pc.elf on 4 processors (no qemu-system-i386)"
else
  # KVM where there is one (helios's), else QEMU's own threads for each processor
  accel=""; [ -r /dev/kvm ] && [ -w /dev/kvm ] && accel="-accel kvm -cpu host"
  qemu_smp () {
    local n=$1; shift
    rm -f $W/serial-smp.txt
    timeout 600 "$Q" $accel -smp 4 -m 512 -display none -no-reboot -serial file:$W/serial-smp.txt \
      -device isa-debug-exit,iobase=0xf4,iosize=4 -kernel c4pc.elf -append "--cpus $n" -initrd "$1" > /dev/null 2>&1
    tr -d '\r' < $W/serial-smp.txt
  }
  smp_checks "c4pc.elf on 4 processors" qemu_smp
  rm -f $W/serial-smp.txt
  timeout 900 "$Q" $accel -smp 4 -m 512 -display none -no-reboot -serial file:$W/serial-smp.txt \
    -device isa-debug-exit,iobase=0xf4,iosize=4 -kernel c4pc.elf -append "--mmu --cpus 4 -m 64" \
    -initrd "$W/c4ix32.c4r,$W/test-disk.tar drive,$W/in.txt stdin" > /dev/null 2>&1
  out=$(tr -d '\r' < $W/serial-smp.txt)
  if echo "$out" | grep -qxF "c4ix: 4 processors" && echo "$out" | grep -qxF "  a thread each, all at once" &&
     echo "$out" | grep -qF "shutdown complete, 0 tasks remain" && ! echo "$out" | grep -qE "cache .task. .*live [1-9]|panic|illegal"; then
    ok "c4pc.elf on 4 processors: C4IX boots on all four, runs ps, and shuts down with every task freed"
  else bad "c4pc.elf on 4 processors: the C4IX session"; echo "$out" | grep -E "processors|thread|panic|illegal|shutdown" | head -6; fi
fi
# test-libjs's programs with a native oracle, on both
progs=$(IMAGES=$IMAGES bash src/c4pc/test-c4pc-progs.sh); r=$?
echo "$progs" | grep -v '^test-c4pc-progs: '
[ $r = 0 ] || fail=1
[ $fail = 0 ] && echo "test-c4pc: OK" || { echo "test-c4pc: FAILED"; exit 1; }
