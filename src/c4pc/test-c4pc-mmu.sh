#!/bin/bash
# test-c4pc-mmu.sh -- libjs/tests/test-c4ix-mmu.mjs's cases on c4pc-host
# and libjs, both with the MMU, and on c4pc.elf in QEMU where it is
# installed (test-c4pc.sh runs it; NO_QEMU=1 as there).
#
# The same program, wild.c (taken from the .mjs), does the damage inside
# C4IX: it writes into the kernel, into another task's word, past its stack,
# over its own code, jumps into its data, reads through a null pointer and
# hands the drive a name in the kernel's memory. Each must be killed by a
# page fault while the victim, in the background throughout, keeps its word
# and the shell carries on. The .mjs types at a running machine; here the
# commands are piped up front, so the victim's address is found by a first
# run, and the second run, which has it, is held against libjs byte for
# byte. The .mjs's two "ring" cases need the display device, which c4pc
# has not yet.
set -u
IMAGES=${IMAGES:-src/c4bb/images}
W=.c4pc/mmu-cases
CYC=${CYC:-20000000000}
fail=0
ok () { echo "  ok    $*"; }
bad () { echo "  FAIL  $*"; fail=1; }
mkdir -p $W
# the boot disk, and wild.c beside it: a tree of links to the files
if [ ! -f $W/disk/wild.c ]; then
  rm -rf $W/disk
  cp -rs "$(realpath $IMAGES/disk)" $W/disk
  node -e '
    const s = require("fs").readFileSync("libjs/tests/test-c4ix-mmu.mjs", "utf8");
    const m = /files\.set\(.wild\.c., new TextEncoder\(\)\.encode\(`([\s\S]*?)`\)\);/.exec(s);
    if (!m) { console.error("no wild.c in test-c4ix-mmu.mjs"); process.exit(1); }
    process.stdout.write(new Function("return `" + m[1] + "`")());' > $W/disk/wild.c || exit 1
fi
cp $IMAGES/c4ix32.c4r $W/c4ix32.c4r
host () { timeout 900 ./c4pc-host -s --mmu --no-accel --no-osaccel -m 64 -c $CYC -d $W/disk $W/c4ix32.c4r < $1 > $2 2> $3; }

# every case, the victim's address and its physical one in the pokes
cases () {
  printf 'wait 4\ncc -l c4ix -o /tmp/wild.c4r wild.c\n/tmp/wild.c4r victim 8 &\n'
  for c in kernel "poke $1" stack text exec null "disk 0x2000" "poke $2"; do printf '/tmp/wild.c4r %s\n' "$c"; done
  printf 'echo the kernel is fine\ndmesg\nwait\nexit\n'
}
victim () { sed -n 's/.*victim: pid [0-9]*, its word at \(0x[0-9a-f]*\).*/\1/p' $1 | head -1; }
# the first run: where the victim keeps its word. The victim allocates while
# the next command starts, so the lines until it says are the second run's
cases 0x10000000 0x0 > $W/in1.txt
host $W/in1.txt $W/host1.out $W/host1.err
va=$(victim $W/host1.out)
if [ -z "$va" ]; then bad "the victim says where its word is"; tail -20 $W/host1.out; exit 1; fi
ok "the victim keeps a word of its own, at $va"
[ $((va)) -ge $((0x10000000)) ] && ok "... a user address, not a physical one" || bad "the victim's word is at $va, not a user address"
pa=$(printf '0x%x' $((va - 0x10000000)))

# the second: the pokes at the victim's word
cases $va $pa > $W/in.txt
host $W/in.txt $W/host.out $W/host.err
st=$?
[ "$(victim $W/host.out)" = "$va" ] && ok "the victim's word is where it was" || bad "the victim's word moved, to $(victim $W/host.out)"
[ $st = 0 ] && ok "c4pc-host: the session ends with status 0" || bad "c4pc-host: status $st"
timeout 1800 node libjs/cli.js -s --mmu --no-accel --no-osaccel -m 64 -c $CYC -d $W/disk $W/c4ix32.c4r \
  < $W/in.txt > $W/libjs.out 2> $W/libjs.err
st=$?
[ $st = 0 ] && ok "libjs: the session ends with status 0" || bad "libjs: status $st"

o=$W/host.out
has () { grep -qE "$2" $o && ok "$1" || bad "$1"; }
grep -qF "/tmp/wild.c4r: error" $o && bad "wild.c compiles inside C4IX" || ok "wild.c compiles inside C4IX"
has "wild kernel: a page fault" "page fault: write to 0x00002000 \(the kernel's image, not mapped\).*killed"
has "wild poke (the victim's word): a page fault" "page fault: write to 0x0*${va#0x} \(task wild\.c4r, not mapped\).*killed"
has "wild stack: a page fault" "page fault: write to 0x[0-9a-f]+ \(the stack guard of task wild\.c4r, not mapped\).*killed"
has "wild text: a page fault" "page fault: write to 0x[0-9a-f]+ \(task wild\.c4r, not allowed\).*killed"
has "wild exec: a page fault" "page fault: jump to 0x[0-9a-f]+ \(task wild\.c4r, not allowed\).*killed"
has "wild null: a page fault" "page fault: read of 0x00000004 \(page 0, not allowed\).*killed"
has "wild disk (a name in the kernel's memory): a device's page fault" "page fault: a device's read of 0x00002000 \(the kernel's image, not mapped\).*killed"
has "wild poke (the victim's word's physical address): a page fault" "page fault: write to 0x0*${pa#0x} \([^)]*, not mapped\).*killed"
grep -qE "wild: (wrote it|came back|read )" $o && bad "no wild write, jump or read got through" || ok "no wild write, jump or read got through"
has "the kernel carries on" "^the kernel is fine$"
has "dmesg has the MMU coming on" "\] mmu: "
has "dmesg has the faults" "\] c4ix: task [0-9]+ \(wild\.c4r\): page fault: write to 0x00002000"
has "the victim's word was never touched" "victim: intact"
if cmp -s $W/libjs.out $W/host.out; then ok "c4pc-host's output is libjs's, byte for byte"
else bad "c4pc-host's output differs from libjs's:"; diff $W/libjs.out $W/host.out | head -20; fi
cl=$(grep -o '^libjs: [0-9]* cycles' $W/libjs.err | grep -o '[0-9]*')
ch=$(sed -n 's/^c4pc: \([0-9]*\) cycles.*/\1/p' $W/host.err)
[ -n "$cl" ] && [ "$cl" = "$ch" ] && ok "the same cycles on both ($cl)" || bad "cycles: libjs $cl, c4pc-host $ch"
grep -h 'inst/s' $W/libjs.err $W/host.err | sed 's/^/        /'

# the second run's cases on c4pc.elf in QEMU: the bare PC's way out of a
# fault (pc/boot.S c4_longjmp), which the session alone may never take
Q=${QEMU:-$(command -v qemu-system-i386 || echo ~/opt/pctools/bin/qemu-system-i386)}
if [ -n "${NO_QEMU:-}" ] || [ ! -x "$Q" ] || [ ! -f c4pc.elf ]; then
  echo "  skip  the cases on c4pc.elf in QEMU"
else
  [ $W/disk.tar -nt $W/disk/wild.c ] || tar --format=ustar -h -C $W/disk -cf $W/disk.tar .
  rm -f $W/serial.txt
  timeout 1800 "$Q" -m 512 -display none -no-reboot -serial file:$W/serial.txt \
    -device isa-debug-exit,iobase=0xf4,iosize=4 -kernel c4pc.elf \
    -append "--no-cdrom -s --mmu --no-accel --no-osaccel -m 64 -c $CYC" -initrd "$W/c4ix32.c4r,$W/disk.tar drive,$W/in.txt stdin"
  st=$?
  [ $st = 1 ] && ok "c4pc.elf in QEMU: the session ends with status 0" || bad "c4pc.elf in QEMU: QEMU's status $st"
  tr -d '\r' < $W/serial.txt | grep -v '^c4pc: ' > $W/qemu.out
  if cmp -s $W/libjs.out $W/qemu.out; then ok "c4pc.elf's output in QEMU is libjs's, byte for byte"
  else bad "c4pc.elf's output in QEMU differs from libjs's:"; diff $W/libjs.out $W/qemu.out | head -20; fi
  cq=$(tr -d '\r' < $W/serial.txt | sed -n 's/^c4pc: \([0-9]*\) cycles.*/\1/p')
  [ "$cl" = "$cq" ] && ok "the same cycles in QEMU ($cq)" || bad "cycles: libjs $cl, QEMU $cq"
  tr -d '\r' < $W/serial.txt | grep 'inst/s' | sed 's/^/        /'
fi
[ $fail = 0 ] && echo "test-c4pc-mmu: OK" || { echo "test-c4pc-mmu: FAILED"; exit 1; }
