#!/bin/bash
# test-libjs.sh - the libjs c4m against native c4m, and both kernels.
# Run from the repo root (make test-libjs). Modelled on
# src/c4bb/tests/test-c4bb.sh, whose images and oracles it shares.
#
# The oracle for each image is native 32-bit c4m (./c4m32 load-c4r.c),
# except where c4m32 cannot be trusted:
#   - %lld reads two words on a 32-bit libc where the program pushed one,
#     so those images compare against 64-bit c4m;
#   - test_args and test_printf segfault c4m32 in its missed-trap path.
# Trap chatter from kernel-less images and %p lines are filtered.
#
# Out of contract, as on c4bb: test_customop test_signal test_timekeeping
# test_fread (need a kernel or a host fd), test_crash (STRC needs symbols).

LIBJS="node libjs/cli.js"
C4M32="./c4m32 load-c4r.c --"
C4M64="./c4m load-c4r.c --"
IMAGES=src/c4bb/images
FILTER='/^Trap type/d; /missed a trap/d'
NORM='s/rendered in [0-9]*ms/rendered in Xms/; s|f/s [ 0-9]\{5\}|f/s XXXXX|g; s/kc [ 0-9]\{5\}/kc XXXXX/g'
fail=0

ok ()   { echo "test-libjs: $1 OK"; }
bad ()  { echo "test-libjs: $1 FAILED"; fail=1; }

check () {
    name="$1"; shift
    a=$(timeout 300 $LIBJS "$@" 2>/dev/null | sed "$FILTER" | sed "$NORM")
    b=$(timeout 300 $C4M32 "$@" 2>&1 | sed "$FILTER" | sed "$NORM")
    if [ "$a" = "$b" ]; then ok "$name"; else
        bad "$name"; diff <(echo "$a") <(echo "$b") | head -10; fi
}

# The images the Code Explorer lists carry source maps (Makefile, IMG_G):
# C4IX's kernel and every c4ix-*.c4r, and the games. An image with a map
# is longer than its body, which ends at S (c4r-body.py).
if [ "$(sed -n 's/^IMG_G := //p' Makefile)" = "-g" ]; then
    nomap=""
    for f in $IMAGES/c4ix32.c4r $IMAGES/disk/c4ix-*.c4r $IMAGES/games/*.c4r; do
        [ -f "$f" ] || continue
        [ "$(python3 -I libjs/tests/c4r-body.py $f | cut -d' ' -f2)" -lt "$(stat -c %s $f)" ] || nomap="$nomap ${f#$IMAGES/}"
    done
    if [ -z "$nomap" ]; then ok "the explorer's images carry source maps"
    else bad "the explorer's images carry source maps (none in:$nomap)"; fi
fi

check64 () {
    name="$1"; img32="$2"; img64="$3"
    a=$(timeout 300 $LIBJS "$img32" 2>/dev/null | sed "$FILTER")
    b=$(timeout 300 $C4M64 "$img64" 2>&1 | sed "$FILTER")
    if [ "$a" = "$b" ]; then ok "$name (vs 64-bit baseline)"; else
        bad "$name"; diff <(echo "$a") <(echo "$b") | head -10; fi
}

# ---- pure units ---------------------------------------------------------
node libjs/tests/test-printf.mjs || fail=1
node libjs/tests/test-c4r.mjs    || fail=1
node libjs/tests/test-opdoc.mjs  || fail=1
node libjs/tests/test-heap.mjs   || fail=1
node libjs/tests/test-sheap.mjs  || fail=1
node libjs/tests/test-gui.mjs    || fail=1
node libjs/tests/test-shared.mjs || fail=1
node libjs/tests/test-desktop.mjs || fail=1
node libjs/tests/test-boot-desktop.mjs || fail=1
node libjs/tests/test-c4ix-zombie.mjs || fail=1
node libjs/tests/test-c4ix-cpu.mjs || fail=1
node libjs/tests/test-c4ix-c4m.mjs || fail=1
node libjs/tests/test-c4ix-sigh.mjs || fail=1
node libjs/tests/test-rom.mjs    || fail=1

# ---- programs, byte for byte against native -----------------------------
for t in hello32 test_basic test_static test_vprintf tests \
         multifun test-order test-ptrs test_continue test_exit \
         test_printloop mandel test_float cycles; do
    check "$t" $IMAGES/$t.c4r
done
check raycast $IMAGES/disk/raycast.c4r 21x21 -s 7 -d -n 20 -g 80x22
# gui-demo: without a display fitted (the CLI fits none) it must say
# "not fitted" on both hosts and touch nothing.
for t in bb_customop bb_preempt bb_pm bb_mbox gui-demo fb-demo; do
    check "$t" $IMAGES/$t.c4r
done
# M4's integer-types battery, built at 32 bits by c4sp32's c4lc: here the
# oracle is not c4m32 but gcc -m32 itself, the golden test-c4lc uses --
# unsigned compares, helpers for 16-bit loads and unsigned division, and
# %hu/%lu through printf.js.
./c4sp32 src/c4sp/lisp/c4lc.lisp src/tests/c4lc_m4.c .libjs_m4.c4r >/dev/null 2>&1
if [ "$(timeout 300 $LIBJS .libjs_m4.c4r 2>/dev/null)" = "$(cat src/c4sp/tests/expected/c4lc-m4-32.txt)" ]
then ok "c4lc_m4 (gcc -m32's answers)"; else bad "c4lc_m4 (gcc -m32's answers)"; fi
rm -f .libjs_m4.c4r
check64 factorial   $IMAGES/factorial.c4r   src/tests/factorial.c4r
check64 test_malloc $IMAGES/test_malloc.c4r src/tests/test_malloc.c4r

argnorm () { sed 's|src/c4bb/images/|src/tests/|; /an int ptr/d'; }
for t in test_args test_printf; do
    extra=""; [ $t = test_args ] && extra="arg1 arg2"
    a=$(timeout 300 $LIBJS $IMAGES/$t.c4r $extra 2>/dev/null | sed "$FILTER" | argnorm)
    b=$(timeout 300 $C4M64 src/tests/$t.c4r $extra 2>&1 | sed "$FILTER" | argnorm)
    if [ "$a" = "$b" ]; then ok "$t (vs 64-bit baseline)"; else
        bad "$t"; diff <(echo "$a") <(echo "$b") | head -10; fi
done

# ---- the fused opcodes --------------------------------------------------
# All ten, each against its unfused twin, hand-assembled by c4th's
# assembler (the same differential c4bb runs).
fused_out=$(timeout 300 $LIBJS -s -m 96 -d $IMAGES/fused $IMAGES/fused/c4th32.c4r core.f asm.f fused.f 2>&1)
if echo "$fused_out" | grep -q "fused: 0 mismatches" &&
   [ "$(echo "$fused_out" | grep -c ' ok$')" = "10" ] &&
   echo "$fused_out" | grep -q "0 missed traps"; then
    ok "fused all ten"
else
    bad "fused all ten"; echo "$fused_out" | tail -5
fi

# ---- STRC, with names from the symbol section ------------------------------
strc_out=$($LIBJS $IMAGES/libjs-strc.c4r 2>&1 | sed 's/ \[0x[0-9A-F]*\]//')
strc_want=$(printf 'c4m: stacktrace:\n inner()\n  inner()\n   inner()\n    middle()\n     main()\nresult 7')
if [ "$strc_out" = "$strc_want" ]; then ok "stacktrace names every frame"
else bad "stacktrace names every frame"; echo "$strc_out"; fi

# ---- exit status ----------------------------------------------------------
$LIBJS $IMAGES/test_exit.c4r >/dev/null 2>&1; s1=$?
$C4M32 $IMAGES/test_exit.c4r >/dev/null 2>&1; s2=$?
if [ "$s1" = "$s2" ]; then ok "exit-status ($s1)"; else bad "exit-status ($s1 vs $s2)"; fi

# ---- the programmable interrupt timer -------------------------------------
pit_out=$(timeout 120 $LIBJS -m 8 $IMAGES/bb_pit.c4r 2>&1)
if [ "$(echo "$pit_out" | grep -c "real time")" = 2 ]; then ok "pit (free running and masked every pass)"
else bad "pit"; echo "$pit_out" | tail -4; fi

# ---- C4KE ----------------------------------------------------------------
# Grep assertions, as c4bb does: interleavings differ from native.
c4ke () { printf "$1" | timeout 300 $LIBJS -c "${2:-60000000}" -d $IMAGES/disk $IMAGES/c4ke32.c4r 2>/dev/null; }
out=$(c4ke 'hello.c4r\n\\q\n')
for want in "c4ke v" "entering task scheduling" "C4SH - The C4 SHell" "yello" "clean shutdown"; do
    if echo "$out" | grep -qF "$want"; then ok "c4ke-boot '$want'"; else bad "c4ke-boot '$want'"; fi
done
out=$(c4ke 'ls -a /usr/src/bin\n\\q\n')
counts=$(echo "$out" | grep -oE '[0-9]+/[0-9]+ entries loaded')
n1="${counts%%/*}"; n2="${counts#*/}"; n2="${n2%% entries*}"
if [ -n "$counts" ] && [ "$n1" = "$n2" ]; then ok "c4ke-vfs full manifest loaded ($counts)"
else bad "c4ke-vfs full manifest loaded (got '$counts')"; fi
if echo "$out" | grep -qF "/usr/src/bin/ls.c"; then ok "c4ke-vfs ls listing"; else bad "c4ke-vfs ls listing"; fi
out=$(c4ke 'ls\necho second-command\n\\q\n')
if echo "$out" | grep -qF "entries in /" && echo "$out" | grep -qF "second-command" &&
   echo "$out" | grep -qF "clean shutdown"; then ok "c4ke two commands in one session"
else bad "c4ke two commands in one session"; echo "$out" | tail -8; fi
out=$(c4ke 'badop.c4r\necho survived-the-kill\n\\q\n')
if echo "$out" | grep -qF "Custom opcode not found: 9001" && echo "$out" | grep -qF "badop.c4r:main()" &&
   echo "$out" | grep -qF "survived-the-kill" && echo "$out" | grep -qF "clean shutdown"; then
    ok "c4ke survives a task killed on a bad opcode"
else bad "c4ke survives a task killed on a bad opcode"; echo "$out" | tail -10; fi
out=$(c4ke 'cat /usr/src/bin/vfsload.c\n\\q\n')
if echo "$out" | grep -qF "C4KE boot-time filesystem loader"; then ok "c4ke-vfs cat real content"
else bad "c4ke-vfs cat real content"; fi

# ---- C4IX ----------------------------------------------------------------
# Each boot has the page's 64 MB arena (libjs/page.js), not the CLI's 32:
# the root archive unpacks in the background as the commands run, and at
# its peak holds the compressed archive, the whole tar and the files made
# from it -- about 28 MB, which left 32 too little for a program that
# loads a shared library meanwhile.
out=$(printf 'c4ix-ps.c4r\nexit\n' | timeout 300 $LIBJS -m 64 -c 120000000 -d $IMAGES/disk $IMAGES/c4ix32.c4r 2>/dev/null)
for want in "C4IX booting" "protected mode on for user tasks, preemption on" \
            "c4ix-sh" "c4ix-ps.c4r" "tasks,"; do
    if echo "$out" | grep -qF "$want"; then ok "c4ix-boot '$want'"; else bad "c4ix-boot '$want'"; fi
done
# The C4IX vfsload race (docs/c4bb-design.md) is a known issue carried
# over from c4bb, so "most entries" is the pin here too.
if echo "$out" | grep -qE '[4-9][0-9]/[4-9][0-9] entries loaded from c4ix\.vfs\.txt'; then
    ok "c4ix-vfs manifest mostly loaded ($(echo "$out" | grep -oE '[0-9]+/[0-9]+ entries loaded'))"
else bad "c4ix-vfs manifest mostly loaded"; echo "$out" | grep -E 'entries loaded|cannot open' | head -5; fi

# The root archive (c4ix-root.tgz, build-images.sh) unpacks in the
# background as task 4, which "wait 4" blocks on. Then a file deep in
# the tree must have its real content, and the libraries must be there.
out=$(printf 'wait 4\nls /usr/lib\ncat /usr/src/c4ix/user/tar.c\nexit\n' | \
      timeout 300 $LIBJS -m 64 -c 2000000000 -d $IMAGES/disk $IMAGES/c4ix32.c4r 2>/dev/null)
if echo "$out" | grep -qF "unpacking c4ix-root.tgz in the background as task 4" &&
   echo "$out" | grep -qF "[4] done (0)" && echo "$out" | grep -qF "libarc.c4l" &&
   echo "$out" | grep -qF "list or extract a tar archive, gzipped or not"; then
    ok "c4ix root archive unpacked"
else bad "c4ix root archive unpacked"; echo "$out" | grep -E 'unpack|done|tar:|cannot' | head -5; fi

# b4ke inside C4IX. The root archive carries /build already built,
# so the whole tree is up to date at boot: nothing runs. A source written
# again (an edit, as the IDE saves one) remakes what it feeds and no
# more: its object and its program. Then -B rebuilds
# the libraries from /usr, and they must be the ones the archive
# carries in /usr/lib, but for the source map those carry, which cc does
# not write yet (c4r-body.py); and a run after that must do nothing again.
# (Rebuilding the whole tree is `make test-c4ix-rebuild`: minutes.)
E=/usr/src/c4ix/user/echo.c
out=$(printf "wait 4\nb4ke -C /usr\ncat $E > /tmp/echo.c\ncat /tmp/echo.c > $E\nb4ke -C /usr/src/c4ix/user\n"'b4ke -B -C /usr/src/c4ix/lib\ncrc32 /build/lib/libc4ix.c4l\nb4ke -C /usr/src/c4ix/lib\nexit\n' | \
      timeout 900 $LIBJS -m 64 -c 20000000000 -d $IMAGES/disk $IMAGES/c4ix32.c4r 2>/dev/null)
pre=$(echo "$out" | sed '/b4ke: all built/q')
if echo "$pre" | grep -qF "b4ke: all built, in /build" && ! echo "$pre" | grep -qE ": [1-9][0-9]* ran, " &&
   echo "$pre" | grep -qE "c4ix/user: 0 ran, [0-9]+ up to date, 0 failed"; then
    ok "the root archive's /build is up to date: b4ke has nothing to do"
else bad "the root archive's /build is up to date: b4ke has nothing to do"; echo "$pre" | grep -E 'b4ke' | head -12; fi
if echo "$out" | grep -qE "c4ix/user: 2 ran, [0-9]+ up to date, 0 failed"; then
    ok "an edited source remakes only its object and its program"
else bad "an edited source remakes only its object and its program"; echo "$out" | grep -E 'c4ix/user' | head -4; fi
a=$(echo "$out" | grep -oE '^[0-9a-f]{8} [0-9]+ /build/lib/libc4ix.c4l' | cut -d' ' -f1,2)
b=$(python3 -I libjs/tests/c4r-body.py $IMAGES/libc4ix32.c4l)
if [ -n "$a" ] && [ "$a" = "$b" ] && echo "$out" | grep -qF "c4ix/lib: 13 ran, 0 up to date, 0 failed" &&
   echo "$out" | grep -qF "c4ix/lib: 0 ran, 13 up to date, 0 failed"; then
    ok "b4ke rebuilds libc4ix inside C4IX, byte for byte"
else bad "b4ke rebuilds libc4ix inside C4IX, byte for byte"; echo "$out" | grep -E 'b4ke|crc32' | head -8; fi

# cc: c4sc (c4lc, compiled) with c4lc's preprocessor, and -l linking
# against the shared libc4ix -- the userland's own recipe, so echo built
# this way is the disk's, byte for byte, up to the disk's source map. And the repo's own programs, which
# include u0.h and the repo's stdio.h: src/tests/mandel.c, as it is.
out=$(printf 'wait 4\ncc -o /tmp/hello2.c4r /usr/src/c4ix/examples/hello.c\n/tmp/hello2.c4r\ncc -O -D C4IX_SHARED -l /lib/libc4ix.c4s -o /tmp/echo.c4r /usr/src/c4ix/user/echo.c\n/tmp/echo.c4r cc built it\ncrc32 /tmp/echo.c4r\ncd /usr/src/tests\ncc -O mandel.c\n./mandel.c4r\nexit\n' | \
      timeout 300 $LIBJS -m 64 -c 8000000000 -d $IMAGES/disk $IMAGES/c4ix32.c4r 2>/dev/null)
if echo "$out" | grep -qE "^Mandelbrot rendered in [0-9]+ms"; then ok "cc builds src/tests/mandel.c with the repo's headers, and it runs"
else bad "cc builds src/tests/mandel.c with the repo's headers, and it runs"; echo "$out" | grep -E 'mandel|cannot|cc:' | head -5; fi
a=$(echo "$out" | grep -oE '^[0-9a-f]{8} [0-9]+ /tmp/echo.c4r' | cut -d' ' -f1,2)
b=$(python3 -I libjs/tests/c4r-body.py $IMAGES/disk/c4ix-echo.c4r)   # its map aside
if echo "$out" | grep -qF "hello from a program compiled inside C4IX" && echo "$out" | grep -qxF "cc built it" &&
   [ -n "$a" ] && [ "$a" = "$b" ]; then
    ok "cc compiles inside C4IX, and links libc4ix"
else bad "cc compiles inside C4IX, and links libc4ix"; echo "$out" | grep -E 'cc|c4lc|crc|hello' | head -8; fi

# Shared libraries: ls is linked with -l libc4ix.c4s, so it runs only if
# the loader finds /lib/libc4ix.c4s and resolves what it needs from it.
out=$(printf 'c4rdump /bin/ls\nls /lib\nexit\n' | \
      timeout 300 $LIBJS -m 64 -c 400000000 -d $IMAGES/disk $IMAGES/c4ix32.c4r 2>/dev/null)
if echo "$out" | grep -qF "Needs /lib/libc4ix.c4s" && echo "$out" | grep -qF "libc4ix.c4s" &&
   echo "$out" | grep -qE "symbol references to resolve"; then
    ok "ls loads libc4ix as a shared library"
else bad "ls loads libc4ix as a shared library"; echo "$out" | tail -8; fi

# libc: libctest is linked with -l libc.c4s; what it prints, line by line,
# is what libc's functions did (src/c4ix/libc/tests/libctest.txt)
out=$(printf 'libctest -v -n 3 one two\nexit\n' | \
      timeout 300 $LIBJS -m 64 -c 600000000 -d $IMAGES/disk $IMAGES/c4ix32.c4r 2>/dev/null)
if echo "$out" | sed -n '/^printf: \[42/,/libctest: done/p' | cmp -s - src/c4ix/libc/tests/libctest.txt; then
    ok "libc: libctest's output is the reference"
else bad "libc: libctest's output is the reference"
     echo "$out" | sed -n '/^printf: \[42/,/libctest: done/p' | diff - src/c4ix/libc/tests/libctest.txt | head -10; fi

# Shared code: libc is -fpic, so two programs at once run one copy of its
# code, and each must still see its own errno, environment and stdout
# buffer -- the scheduler repoints libc's data at every switch
out=$(printf 'pictest A 15 > /tmp/a &\npictest B 15 > /tmp/b &\nwait\ncat /tmp/a\ncat /tmp/b\nexit\n' | \
      timeout 300 $LIBJS -m 64 -c 800000000 -d $IMAGES/disk $IMAGES/c4ix32.c4r 2>/dev/null)
if echo "$out" | grep -qx "A: ok" && echo "$out" | grep -qx "B: ok" && ! echo "$out" | grep -qF "crossed"; then
    ok "two tasks share libc's code and keep their own data"
else bad "two tasks share libc's code and keep their own data"; echo "$out" | grep -E 'ok|crossed|loader' | head -6; fi

# The POSIX calls Busybox needs (M7c, docs/busybox-port.md §4): lseek,
# fstat and stat, O_APPEND and O_EXCL, link and truncate, waitpid, and
# vfork with execve and an environment -- among them a vfork child that
# redirects its stdout, writes, and execs. What unixtest prints, line by
# line, is src/c4ix/libc/tests/unixtest.txt.
out=$(printf 'unixtest\nexit\n' | \
      timeout 300 $LIBJS -m 64 -c 1000000000 -d $IMAGES/disk $IMAGES/c4ix32.c4r 2>/dev/null)
if echo "$out" | sed -n '/^lseek: set/,/unixtest: done/p' | cmp -s - src/c4ix/libc/tests/unixtest.txt; then
    ok "unixtest: vfork, execve, waitpid, lseek, fstat, links and truncate"
else bad "unixtest: vfork, execve, waitpid, lseek, fstat, links and truncate"
     echo "$out" | sed -n '/^lseek: set/,/unixtest: done/p' | diff - src/c4ix/libc/tests/unixtest.txt | head -10; fi

# The sound card (snd-device.js). Its AdLib, opl.js, is DOSBox's dbopl
# ported line for line, so the C++ (libjs/tests/src/oplref) renders random
# register streams and opl.js must make the same samples, bit for bit: at
# three rates, as an OPL2 and as an OPL3. And its tables are the C++'s.
if command -v g++ > /dev/null 2>&1; then
    T=$(mktemp -d)
    g++ -O2 -w -I libjs/tests/src/oplref -o $T/oplref libjs/tests/src/oplref/oplref.cpp
    same=1
    for c in "48000 48000 12345 0" "48000 48000 12345 1" "44100 96000 7 0" "49716 96000 99 1"; do
        $T/oplref $c > $T/ref.raw
        node libjs/tests/src/opl-stream.mjs $c > $T/js.raw
        cmp -s $T/ref.raw $T/js.raw || { same=0; echo "  opl.js differs: $c"; }
    done
    $T/oplref -t > $T/tables.txt
    python3 -I libjs/tests/src/oplref/mktables.py $T/tables.txt $T/opl-tables.js
    cmp -s $T/opl-tables.js libjs/opl-tables.js || { same=0; echo "  libjs/opl-tables.js is not oplref -t's"; }
    rm -rf $T
    if [ $same = 1 ]; then ok "opl.js makes dbopl's samples, bit for bit"
    else bad "opl.js makes dbopl's samples, bit for bit"; fi
else echo "test-libjs: no g++, so opl.js is not compared with dbopl"; fi

# snd-demo on the bare machine, its sound written to a file: the same file
# every time (the machine's clock), sound where the tune and the sample
# are, and quiet once the last note has died away
T=$(mktemp -d)
out=$($LIBJS -s --wav $T/a.wav $IMAGES/snd-demo.c4r 2>&1)
$LIBJS --wav $T/b.wav $IMAGES/snd-demo.c4r > /dev/null 2>&1
lev=$(node libjs/tests/src/wav-levels.mjs $T/a.wav 0.05 0.5 1.7 3.3)
if echo "$out" | grep -qF "snd-demo: 8 notes" && echo "$out" | grep -qE "sound: [0-9]+ OPL writes, 1 samples played" &&
   cmp -s $T/a.wav $T/b.wav && [ "$lev" = "loud loud loud quiet" ]; then
    ok "snd-demo plays, the same every time"
else bad "snd-demo plays, the same every time"; echo "$out" | tail -3; echo "levels: $lev"; fi
rm -rf $T

# Persistence (src/c4ix/user/persist.c): drive 1 keeps what changed. One
# session writes a file, makes a directory, deletes a file the root
# archive brought and syncs; the next, on the same drive, finds all three
# as they were left. A third, after persist forget, starts clean.
T=$(mktemp -d)
out=$(printf 'wait 4\necho kept > /usr/kept.txt\nmkdir /usr/kdir\nrm /usr/README.md\nsync\nexit\n' | \
      timeout 300 $LIBJS -m 64 -c 3000000000 -d $IMAGES/disk -w $T $IMAGES/c4ix32.c4r 2>/dev/null)
out2=$(printf 'wait 4\ncat /usr/kept.txt\nls /usr\npersist forget\nexit\n' | \
      timeout 300 $LIBJS -m 64 -c 3000000000 -d $IMAGES/disk -w $T $IMAGES/c4ix32.c4r 2>/dev/null)
out3=$(printf 'wait 4\nls /usr\nexit\n' | \
      timeout 300 $LIBJS -m 64 -c 3000000000 -d $IMAGES/disk -w $T $IMAGES/c4ix32.c4r 2>/dev/null)
if echo "$out" | grep -qE "persist: [0-9]+ changes and 1 deletions kept" && [ -s $T/c4ix-persist.tar ] &&
   echo "$out2" | grep -qE "persist: [0-9]+ files kept from the last session, 1 deleted" &&
   echo "$out2" | grep -qx "kept" && echo "$out2" | grep -qx "kdir/" && ! echo "$out2" | grep -qx "README.md" &&
   echo "$out3" | grep -qx "README.md" && ! echo "$out3" | grep -qx "kdir/"; then
    ok "persist keeps a session's changes on drive 1 for the next"
else bad "persist keeps a session's changes on drive 1 for the next"
     echo "$out" | grep -E "persist|sync|rm:" | head -4; echo "$out2" | grep -E "persist|kept|kdir|README" | head -6; fi
rm -rf $T

# Processors (smp.js, docs/c4ix-smp.md S1): c4mp's own guests, built for
# 32 bits, print what c4mp prints -- its pinned transcripts, cycle counts
# and all -- since the round robin is c4mp's, quantum for quantum. The
# deadlock is diagnosed and exits 255, as c4mp's does. And the CPU-local
# window: each CPU sees its own words at 0x800.
for g in smp0 smp1; do
    if $LIBJS --cpus 4 $IMAGES/c4mp-$g.c4r 2>&1 | cmp -s - src/c4mp/tests/$g-4cpu.txt; then
        ok "--cpus 4 runs c4mp's $g as c4mp does"
    else bad "--cpus 4 runs c4mp's $g as c4mp does"; $LIBJS --cpus 4 $IMAGES/c4mp-$g.c4r 2>&1 | diff - src/c4mp/tests/$g-4cpu.txt | head -6; fi
done
out=$($LIBJS --cpus 4 $IMAGES/c4mp-deadlock.c4r 2>&1); st=$?
if [ $st = 255 ] && echo "$out" | grep -qF "deadlock: every processor is parked and none can be woken" &&
   [ "$(echo "$out" | grep -c "waiting on 0x")" = 4 ]; then ok "a deadlock is diagnosed, each CPU named"
else bad "a deadlock is diagnosed, each CPU named"; echo "$out" | tail -6; fi
out=$($LIBJS --cpus 2 $IMAGES/libjs-smpwin.c4r 2>&1)
if echo "$out" | grep -qx "smpwin: cpu 1 saw 111, cpu 0 sees 1 and 9" && $LIBJS $IMAGES/libjs-smpwin.c4r 2>&1 | grep -qx "smpwin: no SMP"; then
    ok "each CPU sees its own words in the CPU-local window"
else bad "each CPU sees its own words in the CPU-local window"; echo "$out" | tail -3; fi

# The parallel engine (par.js, ap.js, docs/c4ix-smp.md S2): each CPU on a
# thread of its own, over a SharedArrayBuffer. The interleaving is the
# host's, so the cycle counts are not c4mp's; the answers are.
out=$($LIBJS --cpus 4 --parallel $IMAGES/c4mp-smp0.c4r 2>&1)
if echo "$out" | grep -qx "smp0: total 48000 across 4 cpu(s), expected 48000"; then ok "--parallel: smp0's counters add up"
else bad "--parallel: smp0's counters add up"; echo "$out" | tail -3; fi
out=$($LIBJS --cpus 4 --parallel $IMAGES/c4mp-smp1.c4r 2>&1)
if echo "$out" | grep -qx "smp1: all OK"; then ok "--parallel: smp1's atomics, parking and IPIs"
else bad "--parallel: smp1's atomics, parking and IPIs"; echo "$out" | tail -4; fi
out=$($LIBJS --cpus 4 --parallel $IMAGES/c4mp-deadlock.c4r 2>&1); st=$?
if [ $st = 255 ] && echo "$out" | grep -qF "deadlock: every processor is parked and none can be woken" &&
   [ "$(echo "$out" | grep -c "waiting on 0x")" = 4 ]; then ok "--parallel: a deadlock is diagnosed, each CPU named"
else bad "--parallel: a deadlock is diagnosed, each CPU named"; echo "$out" | tail -6; fi
out=$($LIBJS --cpus 2 --parallel $IMAGES/libjs-smpwin.c4r 2>&1)
if echo "$out" | grep -qx "smpwin: cpu 1 saw 111, cpu 0 sees 1 and 9"; then ok "--parallel: the CPU-local window"
else bad "--parallel: the CPU-local window"; echo "$out" | tail -3; fi
out=$($LIBJS --cpus 4 --parallel $IMAGES/libjs-spinmp.c4r 2>&1)
if echo "$out" | grep -qx "spinmp: 4 cpus each summed 70000000"; then ok "--parallel: four CPUs work at once"
else bad "--parallel: four CPUs work at once"; echo "$out" | tail -3; fi

if [ $fail = 0 ]; then echo "test-libjs: OK"; else echo "test-libjs: FAILURES"; exit 1; fi
