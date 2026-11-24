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
  timeout 1800 "$Q" -machine vmport=off -m 512 -display none -no-reboot -serial file:$W/serial$t.txt \
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
# The ISO's boot disk is packed (pc/packdisk.py: each file gzipped alone,
# inflated by c4pc's drive when it is first read: drive.c, inflate.c): as
# --tar it gives the plain archive's session, byte for byte and in its
# cycles.
echo "  ---   a packed boot disk"
python3 src/c4pc/pc/packdisk.py $W/test-disk.tar $W/test-diskz.tar > /dev/null
for t in test-disk test-diskz; do
  timeout 600 ./c4pc-host -s --mmu -m 64 -c $CYC --tar $W/$t.tar $W/c4ix32.c4r < $W/in.txt > $W/$t.out 2> $W/$t.err
done
grep -qF "shutdown complete" $W/test-diskz.out && cmp -s $W/test-disk.out $W/test-diskz.out &&
  ok "the packed disk's session is the plain one's, byte for byte" || { bad "the packed disk's session"; diff $W/test-disk.out $W/test-diskz.out | head -6; }
c1=$(sed -n 's/^c4pc: \([0-9]*\) cycles.*/\1/p' $W/test-disk.err); c2=$(sed -n 's/^c4pc: \([0-9]*\) cycles.*/\1/p' $W/test-diskz.err)
[ -n "$c1" ] && [ "$c1" = "$c2" ] && ok "the same cycles from both ($c1)" || bad "cycles: plain $c1, packed $c2"
echo "        $(stat -c %s $W/test-disk.tar) bytes packed to $(stat -c %s $W/test-diskz.tar)"

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
# C4IX's own filesystem (docs/c4ix-ext2.md): a disk mke2fs makes, as a
# block device (--disk), mounted, read, written and unmounted by the
# kernel's ext2.c; then e2fsck finds nothing wrong, and debugfs reads the
# copy back byte for byte. Where the images have it (mkfs on the disk) and
# e2fsprogs is installed.
if [ -f $IMAGES/disk/c4ix-mkfs.c4r ] && command -v mke2fs > /dev/null && command -v debugfs > /dev/null; then
  echo "  ---   a disk of C4IX's own: ext2"
  rm -rf $W/e2 && mkdir -p $W/e2/src/docs
  head -c 1500000 /dev/urandom > $W/e2/src/docs/big.bin; echo hi > $W/e2/src/hi.txt
  mke2fs -q -t ext2 -b 4096 -d $W/e2/src $W/e2/disk.img 16M
  truncate -s 24M $W/e2/new.img
  want=$(python3 -I -c "import zlib,sys; d=open(sys.argv[1],'rb').read(); print('%08x' % zlib.crc32(d))" $W/e2/src/docs/big.bin)
  printf 'mount hd0 /mnt/a\ncrc32 /mnt/a/docs/big.bin\nmkdir /mnt/a/made\ncp /mnt/a/docs/big.bin /mnt/a/made/copy.bin\nrm /mnt/a/hi.txt\nchmod 600 /mnt/a/made/copy.bin\numount /mnt/a\nmkfs hd1\nmount hd1 /mnt/b\nmkdir /mnt/b/home\necho made > /mnt/b/home/n.txt\nexit\n' > $W/e2/in.txt
  timeout 600 ./c4pc-host --mmu -m 64 -d $IMAGES/disk --disk $W/e2/disk.img --disk $W/e2/new.img $W/c4ix32.c4r < $W/e2/in.txt > $W/e2/out.txt 2>&1
  grep -qE "^$want 1500000 /mnt/a/docs/big.bin" $W/e2/out.txt && ok "a Linux-made disk mounted and read" || bad "a Linux-made disk mounted and read"
  debugfs -R "dump /made/copy.bin $W/e2/copy.bin" $W/e2/disk.img 2> /dev/null
  cmp -s $W/e2/copy.bin $W/e2/src/docs/big.bin && ok "a file written to it, byte for byte (debugfs)" || bad "the file written to it"
  debugfs -R "stat /made/copy.bin" $W/e2/disk.img 2> /dev/null | grep -q "Mode:  0600" && ok "chmod kept in its inode" || bad "chmod"
  e2fsck -fn $W/e2/disk.img > $W/e2/fsck1.txt 2>&1 && ok "e2fsck finds nothing wrong with it" || { bad "e2fsck on the written disk"; tail -4 $W/e2/fsck1.txt; }
  e2fsck -fn $W/e2/new.img > $W/e2/fsck2.txt 2>&1 && debugfs -R "cat /home/n.txt" $W/e2/new.img 2> /dev/null | grep -qx made &&
    ok "a disk mkfs made, written and left mounted at shutdown: clean, its file there" || { bad "the disk mkfs made"; tail -4 $W/e2/fsck2.txt; }
else
  echo "  skip  a disk of C4IX's own (no c4ix-mkfs.c4r in $IMAGES/disk, or no e2fsprogs)"
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
    timeout 600 "$Q" $accel -machine vmport=off -smp 4 -m 512 -display none -no-reboot -serial file:$W/serial-smp.txt \
      -device isa-debug-exit,iobase=0xf4,iosize=4 -kernel c4pc.elf -append "--cpus $n -m 32" -initrd "$1" > /dev/null 2>&1
    tr -d '\r' < $W/serial-smp.txt
  }
  smp_checks "c4pc.elf on 4 processors" qemu_smp
  rm -f $W/serial-smp.txt
  timeout 900 "$Q" $accel -machine vmport=off -smp 4 -m 512 -display none -no-reboot -serial file:$W/serial-smp.txt \
    -device isa-debug-exit,iobase=0xf4,iosize=4 -kernel c4pc.elf -append "--mmu --cpus 4 -m 64" \
    -initrd "$W/c4ix32.c4r,$W/test-disk.tar drive,$W/in.txt stdin" > /dev/null 2>&1
  out=$(tr -d '\r' < $W/serial-smp.txt)
  if echo "$out" | grep -qxF "c4ix: 4 processors" && echo "$out" | grep -qxF "  a thread each, all at once" &&
     echo "$out" | grep -qF "shutdown complete, 0 tasks remain" && ! echo "$out" | grep -qE "cache .task. .*live [1-9]|panic|illegal"; then
    ok "c4pc.elf on 4 processors: C4IX boots on all four, runs ps, and shuts down with every task freed"
  else bad "c4pc.elf on 4 processors: the C4IX session"; echo "$out" | grep -E "processors|thread|panic|illegal|shutdown" | head -6; fi
fi
# Windowed games in the desktop as the ISO has it (its boot disk and this
# tree's kernel and desktop): an Ace card game and a Puzzles game typed
# into the desktop's Terminal (the Go menu's games run in such a window,
# term_run), and duke3d --window in the background (a window of its own,
# the O op). Each must draw its window: the screen's middle, where the
# window is over the Terminal's black, mostly not black. The user saw
# them black in VMware (2026-11-23); this is what would say so here. Duke's
# menu is dark: 37% of its middle lit, a black window 18%; 30% passes.
for game in "solitaire" "blackbox" "duke3d --window &"; do
  nm=${game%% *}
  # Duke loads for over a minute of the machine's time before its menu
  printf '40000 type %s|\n150000 shot %s\n150100 quit\n' "$game" "$W/win-$nm.png" > $W/win-events.txt
  timeout 600 ./c4pc-host -s --mmu -m 128 --gui 1024x768 --events $W/win-events.txt --tar $W/disk.tar \
    $W/kernel/c4ix32.c4r --desktop --no-top > $W/win-$nm.out 2> $W/win-$nm.err
  lit=$(python3 - $W/win-$nm.png << 'PNG'
import sys, zlib, struct
d = open(sys.argv[1], "rb").read()
w, h = struct.unpack(">II", d[16:24])
i, z = 8, b""
while i < len(d):
    n, t = struct.unpack(">I4s", d[i:i + 8])
    if t == b"IDAT": z += d[i + 8:i + 8 + n]
    i += 12 + n
raw = zlib.decompress(z)
lit = tot = 0
for y in range(250, 520, 4):
    row = raw[y * (1 + w * 3) + 1:(y + 1) * (1 + w * 3)]
    for x in range(300, 700, 4):
        tot += 1
        if sum(row[x * 3:x * 3 + 3]) > 90: lit += 1
print(lit * 100 // tot)
PNG
)
  [ "${lit:-0}" -ge 30 ] && ok "the desktop's windowed $nm draws its window ($lit% of its middle lit)" ||
    bad "the desktop's windowed $nm: its window is black (${lit:-?}% lit; $W/win-$nm.png)"
done
# A PNG's size, how much of it is lit (pixels brighter than near-black,
# every 8th each way) and how much of its right edge: "W H PERCENT EDGE"
png_lit () {
  python3 - "$1" << 'PNG'
import sys, zlib, struct
d = open(sys.argv[1], "rb").read()
w, h = struct.unpack(">II", d[16:24])
i, z = 8, b""
while i < len(d):
    n, t = struct.unpack(">I4s", d[i:i + 8])
    if t == b"IDAT": z += d[i + 8:i + 8 + n]
    i += 12 + n
raw = zlib.decompress(z)
lit = tot = edge = etot = 0
for y in range(0, h, 8):
    row = raw[y * (1 + w * 3) + 1:(y + 1) * (1 + w * 3)]
    for x in range(0, w, 8):
        tot += 1
        if sum(row[x * 3:x * 3 + 3]) > 90: lit += 1
    etot += 1
    if sum(row[(w - 6) * 3:(w - 5) * 3]) > 90: edge += 1
print(w, h, lit * 100 // tot, edge * 100 // etot)
PNG
}
# Alt+Tab (gui.c's screens and SWITCH, C4IX's sys.c "The display", the
# desktop's shell.c): doom typed into the desktop's Terminal takes the whole
# display, on a screen of its own (1); Alt+Tab parks it and gives the
# desktop its screen (0) back, a button on the taskbar naming the game;
# Alt+Tab there brings Doom back, drawing on from where it was
echo "  ---   Alt+Tab: fullscreen Doom, the desktop, Doom again"
cat > $W/alttab-events.txt <<EV
40000 type doom|
90000 stats
90200 keydown 18 0 4
90250 keydown 9 0 4
90300 keyup 9 0 4
90350 keyup 18 0 0
95000 stats
100200 keydown 18 0 4
100250 keydown 9 0 4
100300 keyup 9 0 4
100350 keyup 18 0 0
105000 stats
115000 stats
115100 shot $W/alttab.png
115200 quit
EV
timeout 600 ./c4pc-host -s --mmu -m 128 --gui 1024x768 --events $W/alttab-events.txt --texts $W/alttab-texts.txt \
  --tar $W/disk.tar $W/kernel/c4ix32.c4r --desktop --no-top > $W/alttab.out 2> $W/alttab.err
# each stats line as "SCREEN PRESENTS"
at=( $(grep "^c4pc: at " $W/alttab.err | awk '{ sub(",", "", $6); print $6 ":" $7 }') )
scr () { echo "${at[$1]:-x:0}" | cut -d: -f1; }
pre () { echo "${at[$1]:-x:0}" | cut -d: -f2; }
[ "$(scr 0)" = 1 ] && [ "$(pre 0)" -gt 0 ] && ok "doom has the whole display, on a screen of its own ($(pre 0) frames)" ||
  bad "doom on the whole display: screen $(scr 0), $(pre 0) frames ($W/alttab.err)"
[ "$(scr 1)" = 0 ] && ok "Alt+Tab: the desktop's screen is back" || bad "Alt+Tab: screen $(scr 1), not the desktop's"
grep -qE "^[0-9]+ [0-9]+ doom$" $W/alttab-texts.txt && ok "the desktop's taskbar has a button for the parked doom" ||
  bad "no taskbar button for the parked doom ($W/alttab-texts.txt)"
n=$(( $(pre 3) - $(pre 2) ))
[ "$(scr 2)" = 1 ] && [ "$(scr 3)" = 1 ] && [ $n -gt 20 ] && ok "Alt+Tab in the desktop: doom's screen again, and $n frames in the next 10 s" ||
  bad "Alt+Tab back to doom: screens $(scr 2) $(scr 3), $n frames in 10 s"
grep -a -E "panic|illegal" $W/alttab.out | head -3
# The screen's modes (gui.c MODE; pc.c and screen.c on a PC, here --modes'
# list taken as asked): the desktop starts in the screen's mode, Settings
# lists them in a dropdown, and a size chosen and applied is the display's,
# the picture, windows and taskbar drawn again at it -- up to 1920x1080,
# wider than the window kit once went (WK_MAX_W) and with a tile of the
# picture over half the desktop's first ring, its taskbar at the foot, then
# then 1366x768, a width no multiple of 8 (VMware's users saw the desktop
# stop at 1280 there, with a dark strip down the right), then down to
# 800x600 (1920x1080's picture takes 8-17 s of the machine's time to scale
# and send, here). Each without a restart. The clicks are where Go, Settings, its dropdown, the
# list (opening upward) and Apply are at 1024x768 (the texts say if they moved)
echo "  ---   the screen's modes: Settings' screen size, up and down"
{ echo "30000 click 30 755"; echo "30500 click 80 549"
  echo "32000 click 180 432"; echo "32500 click 180 390"; echo "33000 click 288 432"; echo "52000 shot $W/modes-up.png"
  echo "52100 click 180 432"; echo "52600 click 180 320"; echo "53100 click 288 432"; echo "64000 shot $W/modes-odd.png"
  echo "64100 click 180 432"; echo "64600 click 180 212"; echo "65100 click 288 432"; echo "72000 shot $W/modes-down.png"
  echo "72100 quit"; } > $W/modes-events.txt
timeout 600 ./c4pc-host -s --mmu -m 128 --gui 1024x768 --modes --events $W/modes-events.txt --texts $W/modes-texts.txt   --tar $W/disk.tar $W/kernel/c4ix32.c4r --desktop --no-top > $W/modes.out 2> $W/modes.err
grep -qE "^151 426 1024x768$" $W/modes-texts.txt && grep -qE "^152 386 1920x1080$" $W/modes-texts.txt &&
  ok "Settings: the screen's modes in a dropdown, from the one it is in (1024x768)" ||
  bad "Settings' screen size is not where the test clicks ($W/modes-texts.txt)"
for s in "up 1920 1080" "odd 1366 768" "down 800 600"; do
  read -r d mw mh <<< "$s"
  r=( $(png_lit $W/modes-$d.png 2> /dev/null) )
  [ "${r[0]:-0}" = $mw ] && [ "${r[1]:-0}" = $mh ] && [ "${r[2]:-0}" -ge 50 ] && [ "${r[3]:-0}" -ge 60 ] &&
    ok "the desktop at ${mw}x$mh: the display that size, ${r[2]}% of it drawn, ${r[3]}% of its right edge" ||
    bad "the desktop at ${mw}x$mh: ${r[0]:-?}x${r[1]:-?}, ${r[2]:-?}% drawn, ${r[3]:-?}% of its right edge ($W/modes-$d.png)"
done
grep -qE "^1213 74[0-9] CPU" $W/modes-texts.txt && ok "at 1366x768 the taskbar reaches the right edge (its CPU meter at x 1213)" ||
  bad "at 1366x768 the taskbar does not reach the right edge ($W/modes-texts.txt)"
grep -qE "^[0-9]+ 10[5-7][0-9] Go$" $W/modes-texts.txt && ok "at 1920x1080 the taskbar is along the screen's foot" ||
  bad "at 1920x1080 the taskbar is not at the foot ($W/modes-texts.txt)"
grep -a -E "panic|illegal|killed" $W/modes.out | head -3
# The picture at 1920x1080, the mode changed back and forth without a
# restart: 1024x768, 1920x1080, 1366x768, 1920x1080, 1280x720, 1920x1080.
# A tile the display's ring refused once left the last size's tiles in its
# place, a grid with dark gaps, which no later redraw mended (a VMware user,
# 2026-11-24: wall.c now sends it again, smaller if need be). At the end,
# right of the windows, no column or row of the picture dark, and the
# taskbar across the whole width (its CPU meter at x 1767)
echo "  ---   the picture at 1920x1080, the mode changed back and forth"
wall_gaps () {
python3 - "$1" << 'PNG'
import sys, zlib, struct
d = open(sys.argv[1], "rb").read()
w, h = struct.unpack(">II", d[16:24])
i, z = 8, b""
while i < len(d):
    n, t = struct.unpack(">I4s", d[i:i + 8])
    if t == b"IDAT": z += d[i + 8:i + 8 + n]
    i += 12 + n
raw = zlib.decompress(z)
rows = [raw[y * (1 + w * 3) + 1:(y + 1) * (1 + w * 3)] for y in range(h)]
lit = lambda x, y: sum(rows[y][x * 3:x * 3 + 3]) > 90
x0 = w * 2 // 5                     # right of the desktop's windows
cols = sum(1 for x in range(x0, w) if sum(lit(x, y) for y in range(0, h - 40, 8)) * 4 < len(range(0, h - 40, 8)))
rws = sum(1 for y in range(0, h * 4 // 5) if sum(lit(x, y) for x in range(x0, w, 8)) * 4 < len(range(x0, w, 8)))
print(w, h, cols, rws)
PNG
}
{ echo "30000 click 30 755"; echo "30500 click 80 549"; t=32000
  for y in 390 320 390 266 390; do
    echo "$t click 180 432"; echo "$(( t + 500 )) click 180 $y"; echo "$(( t + 1000 )) click 288 432"; t=$(( t + 20000 ))
  done
  echo "$t shot $W/wall-1920.png"; echo "$(( t + 100 )) quit"; } > $W/wall-events.txt
timeout 900 ./c4pc-host -s --mmu -m 128 --gui 1024x768 --modes --events $W/wall-events.txt --texts $W/wall-texts.txt \
  --tar $W/disk.tar $W/kernel/c4ix32.c4r --desktop --no-top > $W/wall.out 2> $W/wall.err
r=( $(wall_gaps $W/wall-1920.png 2> /dev/null) )
[ "${r[0]:-0}" = 1920 ] && [ "${r[1]:-0}" = 1080 ] && [ "${r[2]:-1}" = 0 ] && [ "${r[3]:-1}" = 0 ] &&
  ok "after five changes, 1920x1080: the picture whole (no dark column or row right of the windows)" ||
  bad "after five changes: ${r[0]:-?}x${r[1]:-?}, ${r[2]:-?} dark columns, ${r[3]:-?} dark rows ($W/wall-1920.png)"
grep -qE "^1767 1059 CPU" $W/wall-texts.txt && ok "at 1920x1080 the taskbar spans the width (its CPU meter at x 1767)" ||
  bad "at 1920x1080 the taskbar does not span the width ($W/wall-texts.txt)"
# VMware's absolute pointer (pc/vmware.c; QEMU's vmport has the same) on a
# display that does not fill the screen: `gui` (640x480, until QMP's quit) on a 1366x768 mode
# of the SVGA II (QEMU's -vga vmware), scaled by 1.6 and centred, 171
# pixels in. The host's pointer put at screen pixels (QMP), where it lands on
# the display (--debug-mouse logs each): the screen point through where the
# display is, as it is drawn. Mapped straight onto the display's size, as
# it once was, (300, 600) landed at 140, not 80 -- VMware's users "kept
# missing buttons" at 1366x768
if [ -n "${NO_QEMU:-}" ] || [ ! -x "$Q" ]; then
  echo "  skip  VMware's pointer on a scaled display (no qemu-system-i386)"
else
  echo "  ---   VMware's absolute pointer, the display scaled and centred"
  accel=""; [ -r /dev/kvm ] && [ -w /dev/kvm ] && accel="-accel kvm -cpu host"
  printf 'gui 1000000\nshutdown\n' > $W/inptr.txt
  rm -f $W/serialptr.txt $W/qmp.sock
  timeout 300 "$Q" $accel -machine vmport=on -vga vmware -m 512 -display none -no-reboot -serial file:$W/serialptr.txt \
    -qmp unix:$W/qmp.sock,server=on,wait=off -device isa-debug-exit,iobase=0xf4,iosize=4 -kernel c4pc.elf \
    -append "--no-cdrom --mmu --debug-mouse --gui 1366x768" \
    -initrd "$W/c4ix32.c4r,$W/test-disk.tar drive,$W/inptr.txt stdin" > /dev/null 2>&1 &
  qp=$!
  python3 - $W/serialptr.txt $W/qmp.sock << 'QMP'
import json, socket, sys, time
log, sock = sys.argv[1], sys.argv[2]
t0 = time.time()
while not __import__("os").path.exists(sock) and time.time() - t0 < 30: time.sleep(0.2)
s = socket.socket(socket.AF_UNIX); s.connect(sock); f = s.makefile("rw")
while time.time() - t0 < 200:
    try:
        if b"gui: fitted" in open(log, "rb").read(): break
    except OSError: pass
    time.sleep(0.5)
time.sleep(2)
def cmd(c):
    f.write(json.dumps(c) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r: return r
f.readline(); cmd({"execute": "qmp_capabilities"})
for x, y in ((300, 600), (1100, 420)):
    ev = [{"type": "abs", "data": {"axis": "x", "value": x * 32768 // 1366}}, {"type": "abs", "data": {"axis": "y", "value": y * 32768 // 768}}]
    cmd({"execute": "input-send-event", "arguments": {"events": ev}})
    time.sleep(1.5)
cmd({"execute": "quit"})
QMP
  wait $qp
  at=( $(tr -d '\r' < $W/serialptr.txt | grep -a "^vmmouse at " | tail -2 | awk '{print $3 "," $4}') )
  grep -qa "the screen is VMware SVGA II" $W/serialptr.txt && grep -qa "1366 x 768 x 32" $W/serialptr.txt &&
    ok "c4pc.elf in QEMU: the SVGA II at 1366x768" || bad "c4pc.elf in QEMU: no SVGA II at 1366x768 ($W/serialptr.txt)"
  near () { local x=${1%,*} y=${1#*,}; [ $(( x - $2 )) -ge -3 ] && [ $(( x - $2 )) -le 3 ] && [ $(( y - $3 )) -ge -3 ] && [ $(( y - $3 )) -le 3 ]; }
  near "${at[0]:-0,0}" 80 375 && near "${at[1]:-0,0}" 580 262 &&
    ok "the pointer at (300, 600) and (1100, 420) lands at ${at[0]} and ${at[1]} on the display (80,375 and 580,262)" ||
    bad "the pointer at (300, 600) and (1100, 420) lands at ${at[0]:-?} and ${at[1]:-?}, not 80,375 and 580,262 ($W/serialptr.txt)"
fi
# C4IX with more than 256 MB (a VM of 512 MB and more): the MMU's user
# window above memory (src/c4ix/mmu.c mmu_wbase), so swap is on, where it
# once kept the identity map and System Monitor said the MMU was off; and
# with no -m, all of the PC's memory (pc.c), the window above that. This
# tree's kernel (the images' may not have mmu_wbase yet)
if [ -n "${NO_QEMU:-}" ] || [ ! -x "$Q" ]; then
  echo "  skip  c4pc.elf with 512 MB (no qemu-system-i386)"
else
  accel=""; [ -r /dev/kvm ] && [ -w /dev/kvm ] && accel="-accel kvm -cpu host"
  for mem in "-m 512:0x20000000:1024" ":0x40000000:2048"; do
    arg=${mem%%:*}; rest=${mem#*:}; want=${rest%%:*}; pcmb=${rest#*:}
    printf 'cat /proc/meminfo\nshutdown\n' > $W/in512.txt
    rm -f $W/serial512.txt
    timeout 300 "$Q" $accel -machine vmport=off -m $pcmb -display none -no-reboot -serial file:$W/serial512.txt \
      -device isa-debug-exit,iobase=0xf4,iosize=4 -kernel c4pc.elf -append "--no-cdrom --mmu $arg" \
      -initrd "$W/kernel/c4ix32.c4r,$W/test-disk.tar drive,$W/in512.txt stdin" > /dev/null 2>&1
    out=$(tr -d '\r' < $W/serial512.txt)
    what="c4pc.elf ${arg:-with all of a $pcmb MB PC}"
    if echo "$out" | grep -qF "mmu: user addresses are 256 MB from $want" && echo "$out" | grep -qE "^swap [1-9]" &&
       echo "$out" | grep -qF "shutdown complete"; then
      ok "$what: the user window at $want, swap on"
    else bad "$what: the window or swap"; echo "$out" | grep -E "arena|mmu: user|^swap|swap:|panic" | head -6; fi
  done
fi
# test-libjs's programs with a native oracle, on both
progs=$(IMAGES=$IMAGES bash src/c4pc/test-c4pc-progs.sh); r=$?
echo "$progs" | grep -v '^test-c4pc-progs: '
[ $r = 0 ] || fail=1
# the 64-bit machine (docs/c4pc-64.md): against native c4m and c4mp, on a
# PC, and the devices
if [ -z "${NO_W64:-}" ]; then
  echo "  ---   the 64-bit machine (test-c4pc64.sh)"
  out=$(bash src/c4pc/test-c4pc64.sh); r=$?
  echo "$out" | grep -v '^test-c4pc64: '
  [ $r = 0 ] || fail=1
fi
[ $fail = 0 ] && echo "test-c4pc: OK" || { echo "test-c4pc: FAILED"; exit 1; }
