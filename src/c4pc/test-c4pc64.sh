#!/bin/bash
# test-c4pc64.sh -- the 64-bit machine (docs/c4pc-64.md), run by test-c4pc.
#
# c4pc built with C4PC_W64 has native c4m's word, and runs the 64-bit C4IX.
# There is no 64-bit libjs to hold it against, so it is held against what
# there is:
#   - native c4m (c4m.c) and c4mp, the 64-bit reference engines, on what they
#     run: c4pc-host64w --c4m announces c4m's INFO (no devices), and C4IX's
#     demonstration session (test-c4ix's) and the test programs then print
#     what c4m prints, byte for byte; c4mp's SMP guests what c4mp's
#     transcripts say;
#   - itself on a PC: c4pcw64.elf in QEMU prints what c4pc-host64w prints,
#     in the same number of cycles, the session on the 64-bit disk
#     (.c4pc/disk64.tar, disk64.sh), with C4IX's accelerators and without;
#   - what the 32-bit machine's test asks of the devices: the desktop on a
#     display, several CPUs (taking turns, threads, a PC's processors), a
#     CD to copy from, and ext2 on a block device.
#   QEMU64=path  qemu-system-x86_64 (default: on PATH, or ~/opt/pctools/bin)
#   NO_QEMU=1    no QEMU
set -u
W=.c4pc
D=$W/disk64
fail=0
ok () { echo "  ok    $*"; }
bad () { echo "  FAIL  $*"; fail=1; }
CYC=3000000000
Q=${QEMU64:-$(command -v qemu-system-x86_64 || echo ~/opt/pctools/bin/qemu-system-x86_64)}
KVM=""; [ -r /dev/kvm ] && [ -w /dev/kvm ] && KVM="-accel kvm -cpu host"
[ -n "${NO_QEMU:-}" ] || [ -x "$Q" ] || NO_QEMU=1

# ---- native c4m: test-c4ix's demonstration session, and the programs ----
echo "  ---   against native c4m and c4mp"
X=$W/x5
rm -rf $X && mkdir -p $X/src/c4ix/user
# test-c4ix's programs (the Makefile passes its C4IX_PROGS)
PROGS=${C4IX_PROGS:-$(make -s -p -n test-c4ix 2>/dev/null | sed -n 's/^C4IX_PROGS := //p' | head -1)}

# ls loads libc4ix.c4s, which a native host finds beside the programs
cp c4ix.c4r $PROGS libc4ix.c4s $X/

cp src/c4ix/user/test.sh $X/src/c4ix/user/
ARGS="--demo c4ix-hello.c4r c4ix-uhello.c4r c4ix-echo.c4r c4ix-wc.c4r c4ix-sh.c4r src/c4ix/user/test.sh"
MASK () { sed -E -e 's/[0-9]+ cycles/N cycles/g' -e 's/info 0x[0-9a-f]+/info 0xHH/'; }
timeout 120 ./c4m load-c4r.c -- c4ix.c4r $ARGS < /dev/null 2>&1 | MASK > $X/native.txt
timeout 120 ./c4pc-host64w --c4m -m 64 -d $X $X/c4ix.c4r $ARGS < /dev/null 2>&1 | MASK > $X/w64.txt
if grep -qF "shutdown complete" $X/native.txt && cmp -s $X/native.txt $X/w64.txt; then
  ok "test-c4ix's session (C4IX's demonstrations): native c4m's, byte for byte ($(wc -l < $X/native.txt) lines, cycles masked)"
else bad "test-c4ix's session differs from native c4m's:"; diff $X/native.txt $X/w64.txt | head -10; fi
# the test programs c4m runs as they are (load-c4r.c's opcode probes, which
# c4m reports as missed traps, left out); mandel's time masked
for t in cycles factorial hello multifun mandel test_args test_basic test_continue test_exit test_float test_for \
         test_malloc test-order test_printloop tests test_vprintf test-ptrs test_static; do
  [ -f src/tests/$t.c4r ] || { echo "  skip  $t (no src/tests/$t.c4r)"; continue; }
  a=$(timeout 60 ./c4m load-c4r.c -- src/tests/$t.c4r a1 a2 < /dev/null 2>&1; echo "status $?")
  a=$(echo "$a" | grep -v '^Trap type TRAP_ILLOP start\|^c4m: missed a trap' | sed 's/rendered in [0-9]*ms/rendered in Nms/')
  b=$(timeout 60 ./c4pc-host64w --c4m src/tests/$t.c4r a1 a2 < /dev/null 2>&1; echo "status $?")
  b=$(echo "$b" | sed 's/rendered in [0-9]*ms/rendered in Nms/')
  [ "$a" = "$b" ] && ok "$t: native c4m's output and status" || { bad "$t differs from native c4m's"; diff <(echo "$a") <(echo "$b") | head -6; }
done
for g in smp0 smp1; do
  ./c4pc-host64w --cpus 4 c4mp-$g.c4r 2>&1 | cmp -s - src/c4mp/tests/$g-4cpu.txt && ok "--cpus 4 runs c4mp's $g as c4mp does" ||
    bad "--cpus 4: c4mp's $g"
done

# ---- the session on the 64-bit disk: c4pc-host64w and c4pcw64.elf ----
printf 'c4ix-ps.c4r\nexit\n' > $W/in64.txt
session64 () {
  local t=$1 acc=$2 st cl cq
  echo "  ---   the session on the 64-bit disk${acc:+, $acc}"
  timeout 600 ./c4pc-host64w -s $acc -m 64 -c $CYC -d $D $D/c4ix.c4r < $W/in64.txt > $W/h64$t.out 2> $W/h64$t.err
  st=$?
  [ $st = 0 ] && ok "c4pc-host64w: the session ends with status 0" || bad "c4pc-host64w: status $st"
  for want in "C4IX booting" "unpacking c4ix-root.tgz" "c4ix-sh" "c4ix-ps.c4r" "tasks," "shutdown complete"; do
    grep -qF "$want" $W/h64$t.out && ok "c4pc-host64w prints '$want'" || bad "c4pc-host64w prints '$want'"
  done
  cl=$(sed -n 's/^c4pc: \([0-9]*\) cycles.*/\1/p' $W/h64$t.err)
  grep -h 'inst/s' $W/h64$t.err | sed 's/^/        /'
  if [ -n "${NO_QEMU:-}" ]; then echo "  skip  c4pcw64.elf in QEMU (no qemu-system-x86_64)"; return; fi
  rm -f $W/q64serial$t.txt
  timeout 1800 "$Q" $KVM -machine vmport=off -m 512 -display none -no-reboot -serial file:$W/q64serial$t.txt \
    -device isa-debug-exit,iobase=0xf4,iosize=4 -kernel c4pcw64.elf \
    -append "--no-cdrom -s $acc -m 64 -c $CYC" -initrd "$D/c4ix.c4r,$W/disk64.tar drive,$W/in64.txt stdin" > /dev/null 2>&1
  st=$?
  [ $st = 1 ] && ok "c4pcw64.elf in QEMU${KVM:+ (KVM)}: the session ends with status 0" || bad "c4pcw64.elf in QEMU: QEMU's status $st"
  tr -d '\r' < $W/q64serial$t.txt | grep -v '^c4pc: ' > $W/q64$t.out
  if cmp -s $W/h64$t.out $W/q64$t.out; then ok "c4pcw64.elf's output in QEMU is c4pc-host64w's, byte for byte"
  else bad "c4pcw64.elf's output in QEMU differs from c4pc-host64w's:"; diff $W/h64$t.out $W/q64$t.out | head -20; fi
  cq=$(tr -d '\r' < $W/q64serial$t.txt | sed -n 's/^c4pc: \([0-9]*\) cycles.*/\1/p')
  [ -n "$cl" ] && [ "$cl" = "$cq" ] && ok "the same cycles in QEMU ($cq)" || bad "cycles: c4pc-host64w $cl, QEMU $cq"
  tr -d '\r' < $W/q64serial$t.txt | grep 'inst/s' | sed 's/^/        /'
}
session64 "" ""
session64 -noacc "--no-accel --no-osaccel"
# with the MMU (mmu.c's 512-entry tables of words; src/c4ix/mmu.c's)
session64 -mmu "--mmu"
if grep -qF "each user task has a space of its own" $W/h64-mmu.out; then ok "C4IX finds the MMU and gives each task a space of its own"
else bad "C4IX and the MMU, 64-bit"; fi

# ---- the desktop, on a display (test-c4pc's, on the 64-bit system) ----
echo "  ---   the 64-bit desktop, on a display"
# (the double click at 15 s: the 64-bit desktop is ready at about 9.6 s of
# the machine's time -- boot 7.3, then its own start -- and at 9 s the
# click came before it, 2026-11-24; "mouse" waits for its ring to ask for
# the mouse besides, 2026-11-26)
cat > $W/gui64-events.txt <<EV
8000 mouse
15000 move 40 330
15100 packet 1 0 1
15150 packet 0 1 0
15250 packet 1 0 1
15300 packet 0 -1 0
19000 shot $W/desktop64.png
19100 quit
EV
timeout 600 ./c4pc-host64w -s --mmu -m 128 --gui 1024x768 --events $W/gui64-events.txt --texts $W/gui64-texts.txt \
  -d $D $D/c4ix.c4r --desktop > $W/gui64.out 2> $W/gui64.err
st=$?
[ $st = 0 ] && ok "c4pc-host64w with a display: quits at the script's end" || bad "c4pc-host64w with a display: status $st"
for want in Filesystem Terminal "Text Editor" Calculator Integer Double; do
  grep -q " $want\$\| $want " $W/gui64-texts.txt && ok "the display draws '$want'" || bad "the display draws '$want'"
done
grep -h 'display:' $W/gui64.err | sed 's/^/        /'

# The 64-bit desktop straight into 1920x1080 (c4pc-host64w --modes: the
# screen's mode, which the desktop takes): its picture whole. With 8-byte
# words a picture's tile is twice the bytes, and one the ring refused left
# a grid of dark gaps (wall.c); right of the windows, no column or row of
# the picture is dark
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
printf '25000 shot %s\n25100 quit\n' $W/desktop64-1920.png > $W/gui64w-events.txt
timeout 600 ./c4pc-host64w -s --mmu -m 128 --gui 1920x1080 --modes --events $W/gui64w-events.txt --texts $W/gui64w-texts.txt \
  -d $D $D/c4ix.c4r --desktop > $W/gui64w.out 2> $W/gui64w.err
r=( $(wall_gaps $W/desktop64-1920.png 2> /dev/null) )
[ "${r[0]:-0}" = 1920 ] && [ "${r[1]:-0}" = 1080 ] && [ "${r[2]:-1}" = 0 ] && [ "${r[3]:-1}" = 0 ] &&
  grep -qE "^1767 1059 CPU" $W/gui64w-texts.txt &&
  ok "the 64-bit desktop at 1920x1080: its picture whole (no dark column or row), the taskbar across" ||
  bad "the 64-bit desktop at 1920x1080: ${r[0]:-?}x${r[1]:-?}, ${r[2]:-?} dark columns, ${r[3]:-?} dark rows ($W/desktop64-1920.png)"

# ---- several CPUs ----
echo "  ---   several CPUs, 64-bit"
# the CPU-local window's and the parallel engine's guests, as 64-bit images
./c4sc-host compile -O libjs/tests/src/smpwin.c $W/smpwin64.c4r > /dev/null
./c4sc-host compile -O libjs/tests/src/spinmp.c $W/spinmp64.c4r > /dev/null
smp_checks64 () {
  local t=$1 out; shift
  out=$("$@" 4 c4mp-smp0.c4r 2>&1)
  echo "$out" | grep -q "smp0: total 48000 across 4 cpu(s), expected 48000" && ok "$t: smp0's counters add up" || bad "$t: smp0's counters"
  out=$("$@" 4 c4mp-smp1.c4r 2>&1)
  echo "$out" | grep -q "smp1: all OK" && ok "$t: smp1's atomics, parking and IPIs" || { bad "$t: smp1"; echo "$out" | tail -4; }
  out=$("$@" 4 c4mp-deadlock.c4r 2>&1)
  if echo "$out" | grep -qF "deadlock: every processor is parked and none can be woken" &&
     [ "$(echo "$out" | grep -c "waiting on 0x")" = 4 ]; then ok "$t: a deadlock is diagnosed, each CPU named"
  else bad "$t: the deadlock"; echo "$out" | tail -6; fi
  out=$("$@" 2 $W/smpwin64.c4r 2>&1)
  echo "$out" | grep -q "smpwin: cpu 1 saw 111, cpu 0 sees 1 and 9" && ok "$t: the CPU-local window (64 words of 8 bytes)" || { bad "$t: the CPU-local window"; echo "$out" | tail -3; }
  out=$("$@" 4 $W/spinmp64.c4r 2>&1)
  echo "$out" | grep -q "spinmp: 4 cpus each summed 70000000" && ok "$t: four CPUs work at once" || bad "$t: four CPUs at once"
}
turns64 () { local n=$1; shift; timeout 120 ./c4pc-host64w --cpus $n "$@"; }
par64 () { local n=$1; shift; timeout 120 ./c4pc-host64w --cpus $n --parallel "$@"; }
smp_checks64 "--cpus (taking turns)" turns64
smp_checks64 "--parallel" par64
for how in "" "--parallel"; do
  timeout 600 ./c4pc-host64w -s --mmu --cpus 4 $how -m 64 -c $CYC -d $D $D/c4ix.c4r < $W/in64.txt > $W/h64smp.out 2> $W/h64smp.err
  st=$?
  if [ $st = 0 ] && grep -qxF "c4ix: 4 processors" $W/h64smp.out && grep -qF "shutdown complete, 0 tasks remain" $W/h64smp.out &&
     ! grep -qE "cache .task. .*live [1-9]|panic|illegal" $W/h64smp.out; then
    ok "C4IX on 4 CPUs ${how:-taking turns}, with the MMU: boots on all four, runs ps, shuts down with every task freed"
  else bad "C4IX on 4 CPUs ${how:-taking turns} (status $st)"; grep -E "processors|panic|illegal|shutdown" $W/h64smp.out | head -5; fi
done
if [ -z "${NO_QEMU:-}" ]; then
  rm -f $W/q64smp.txt
  timeout 900 "$Q" $KVM -machine vmport=off -smp 4 -m 512 -display none -no-reboot -serial file:$W/q64smp.txt \
    -device isa-debug-exit,iobase=0xf4,iosize=4 -kernel c4pcw64.elf -append "--no-cdrom --mmu --cpus 4 -m 64" \
    -initrd "$D/c4ix.c4r,$W/disk64.tar drive,$W/in64.txt stdin" > /dev/null 2>&1
  out=$(tr -d '\r' < $W/q64smp.txt)
  if echo "$out" | grep -qxF "c4ix: 4 processors" && echo "$out" | grep -qxF "  a thread each, all at once" &&
     echo "$out" | grep -qF "shutdown complete, 0 tasks remain" && ! echo "$out" | grep -qE "cache .task. .*live [1-9]|panic|illegal"; then
    ok "c4pcw64.elf on 4 processors, with the MMU: the 64-bit C4IX boots on all four, runs ps, and shuts down with every task freed"
  else bad "c4pcw64.elf on 4 processors: the C4IX session"; echo "$out" | grep -E "processors|thread|panic|illegal|shutdown" | head -6; fi
fi

# ---- a CD to copy from, and ext2 on a block device ----
XORRISO=$(command -v xorriso || true)
if [ -n "$XORRISO" ]; then
  echo "  ---   a CD to copy from, 64-bit"
  rm -rf $W/cd64 && mkdir -p $W/cd64/src/sub/deeper
  echo "hello from the second CD" > $W/cd64/src/README.TXT
  head -c 300000 /dev/urandom > $W/cd64/src/sub/deeper/deep.bin
  echo "spaces" > "$W/cd64/src/Long File Name.txt"
  "$XORRISO" -as mkisofs -R -J -V TESTDISC -o $W/cd64/test.iso $W/cd64/src > /dev/null 2>&1
  want=$(python3 -I -c "import zlib,sys; d=open(sys.argv[1],'rb').read(); print('%08x %d' % (zlib.crc32(d), len(d)))" $W/cd64/src/sub/deeper/deep.bin)
  printf 'mount cdrom\nls /mnt/cdrom\ncat /mnt/cdrom/README.TXT\nmkdir /home/t\ncp -r /mnt/cdrom/sub /home/t/sub\ncrc32 /mnt/cdrom/sub/deeper/deep.bin /home/t/sub/deeper/deep.bin\ndf\numount\nmount\nexit\n' > $W/cd64/in.txt
  timeout 300 ./c4pc-host64w -m 64 -d $D --cdrom $W/cd64/test.iso $D/c4ix.c4r < $W/cd64/in.txt > $W/cd64/out.txt 2>&1
  out=$(cat $W/cd64/out.txt)
  echo "$out" | grep -qF "mount: drive 2 (cdrom 'TESTDISC'" && ok "mount cdrom mounts the disc at /mnt/cdrom" || bad "mount cdrom"
  echo "$out" | grep -qxF "Long File Name.txt" && echo "$out" | grep -qxF "hello from the second CD" && ok "its names (Rock Ridge's) and its files" || bad "the disc's names and files"
  [ "$(echo "$out" | grep -cE "^$want /(mnt/cdrom|home/t)/sub/deeper/deep.bin")" = 2 ] && ok "a file read off it and copied off it, byte for byte ($want)" || bad "the copy off the disc"
  echo "$out" | grep -qF "umount: /mnt/cdrom let go; drive 2's disc ejected" && ok "umount lets it go and ejects it" || bad "umount"
else
  echo "  skip  a CD to copy from (no xorriso)"
fi
if command -v mke2fs > /dev/null && command -v debugfs > /dev/null; then
  echo "  ---   a disk of C4IX's own: ext2, 64-bit"
  rm -rf $W/e264 && mkdir -p $W/e264/src/docs
  head -c 1500000 /dev/urandom > $W/e264/src/docs/big.bin; echo hi > $W/e264/src/hi.txt
  mke2fs -q -t ext2 -b 4096 -d $W/e264/src $W/e264/disk.img 16M
  truncate -s 24M $W/e264/new.img
  want=$(python3 -I -c "import zlib,sys; d=open(sys.argv[1],'rb').read(); print('%08x' % zlib.crc32(d))" $W/e264/src/docs/big.bin)
  printf 'mount hd0 /mnt/a\ncrc32 /mnt/a/docs/big.bin\nmkdir /mnt/a/made\ncp /mnt/a/docs/big.bin /mnt/a/made/copy.bin\nrm /mnt/a/hi.txt\nchmod 600 /mnt/a/made/copy.bin\numount /mnt/a\nmkfs hd1\nmount hd1 /mnt/b\nmkdir /mnt/b/home\necho made > /mnt/b/home/n.txt\nexit\n' > $W/e264/in.txt
  timeout 600 ./c4pc-host64w -m 64 -d $D --disk $W/e264/disk.img --disk $W/e264/new.img $D/c4ix.c4r < $W/e264/in.txt > $W/e264/out.txt 2>&1
  grep -qE "^$want 1500000 /mnt/a/docs/big.bin" $W/e264/out.txt && ok "a Linux-made disk mounted and read" || bad "a Linux-made disk mounted and read"
  debugfs -R "dump /made/copy.bin $W/e264/copy.bin" $W/e264/disk.img 2> /dev/null
  cmp -s $W/e264/copy.bin $W/e264/src/docs/big.bin && ok "a file written to it, byte for byte (debugfs)" || bad "the file written to it"
  debugfs -R "stat /made/copy.bin" $W/e264/disk.img 2> /dev/null | grep -q "Mode:  0600" && ok "chmod kept in its inode" || bad "chmod"
  e2fsck -fn $W/e264/disk.img > $W/e264/fsck1.txt 2>&1 && ok "e2fsck finds nothing wrong with it" || { bad "e2fsck on the written disk"; tail -4 $W/e264/fsck1.txt; }
  e2fsck -fn $W/e264/new.img > $W/e264/fsck2.txt 2>&1 && debugfs -R "cat /home/n.txt" $W/e264/new.img 2> /dev/null | grep -qx made &&
    ok "a disk mkfs made, written and left mounted at shutdown: clean, its file there" || { bad "the disk mkfs made"; tail -4 $W/e264/fsck2.txt; }
else
  echo "  skip  a disk of C4IX's own (no e2fsprogs)"
fi
# the MMU's faults (test-c4pc-mmu.sh's cases, W64=1): wild.c compiled inside
# the 64-bit C4IX by its cc, each wild write, jump and read killed by a page
# fault, the victim untouched; c4pcw64.elf in QEMU byte and cycle for cycle
echo "  ---   the MMU's faults, 64-bit (test-c4pc-mmu.sh W64=1)"
out=$(W64=1 bash src/c4pc/test-c4pc-mmu.sh); r=$?
echo "$out" | grep -v '^test-c4pc-mmu: '
[ $r = 0 ] || fail=1
[ $fail = 0 ] && echo "test-c4pc64: OK" || { echo "test-c4pc64: FAILED"; exit 1; }
