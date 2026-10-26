#!/bin/bash
# build-images.sh - build the 32-bit .c4r corpus for c4bb testing.
# Run from the repo root; expects c4cc32 to exist (make c4bb-32bit).
set -e

CC=./c4cc32
OUT=src/c4bb/images
U0=include/u0.h
PREPROC="gcc -E -Iinclude -I. -DC4CC=1 -D__c4__=1 -D__C4CC__=1 -D__c4cc__=1 -C"
# ... and gcc -E standing in for c4lc's own preprocessor, which predefines
# __c4_inttypes__ (real unsigned/long; stddef.h then leaves them alone)
PREPROC_LC="$PREPROC -D__c4_inttypes__=1"
# Our own preprocessor (src/c4dos/cpp.c), same flags. It is pinned
# against gcc -E for byte-identical images over the whole corpus
# (src/c4dos/tests/test-cpp.sh), so choosing it costs nothing and means
# the images a player can rebuild INSIDE the machine are built by the
# tool that is in there with them.
OURCPP="./cpp -Iinclude -I. -DC4CC=1 -D__c4__=1 -D__C4CC__=1 -D__c4cc__=1"
if [ ! -x ./cpp ]; then
    echo "c4bb: ./cpp is required (run 'make cpp') -- the shared disk's" >&2
    echo "      c4m.c4r is built with it, not with gcc -E" >&2
    exit 1
fi
mkdir -p $OUT
# Progress: a line a stage, so a build says what it is doing. A long
# loop's stage is one line, with each item named as it is done.
say () { echo "c4bb: $*"; }
step () { printf 'c4bb: %s' "$*"; }
item () { printf ' %s' "$*"; }

# firmware: built with c4lc (the nicer compiler; real preprocessor).
# c4lc emits for its host's word size, so the 32-bit c4sp gives 32-bit
# images. The corpus below builds with c4cc32 so the parity suite
# exercises both compilers.
# The firmware, in its four stages (docs/c4bb-storage.md M6). One
# source; what makes them different is which -D each is handed, and the
# missing pieces are missing from the image rather than skipped at run
# time. At least one -D always: c4lc turns its preprocessor on only
# when there is one, and with none at all both sides of every #ifdef
# would be compiled.
#
#   fw-hello    a banner. The board is alive and nothing else is built.
#   fw-ram      + the memory probe
#   fw-drives   + the drive probe: it can see a medium, not load one
#   fw          + the loader and the retry loop -- the BIOS
fwbuild () {                                  # fwbuild <out> <flags...>
    out=$1; shift
    ./c4sp32 -c 64000000 src/c4sp/lisp/c4lc.lisp -O "$@" \
        src/c4bb/fw/fw.c src/c4bb/fw/$out.c4r > /dev/null
}
step "the firmware:"
item fw-hello; fwbuild fw-hello  -D FW_STAGE=0
item fw-ram; fwbuild fw-ram    -D FW_STAGE=1 -D FW_RAM=1
item fw-drives; fwbuild fw-drives -D FW_STAGE=2 -D FW_RAM=1 -D FW_DRIVES=1 -D FW_SEEONLY=1
item fw; fwbuild fw        -D FW_STAGE=3 -D FW_RAM=1 -D FW_DRIVES=1 -D FW_BOOT=1
echo

say "the test corpus (c4cc32), and the fused-opcode tests"
# hello links without u0 (Makefile:860)
$CC -o $OUT/hello32.c4r src/tests/hello.c > /dev/null

# standard u0-linked tests
for t in factorial test_basic test_malloc test_static tests multifun \
         test-order test-ptrs test_continue mandel rps test_args test_exit \
         cycles test_printloop test_printf; do
    $CC -o $OUT/$t.c4r $U0 src/tests/$t.c > /dev/null
done

# ---- the fused opcodes (79-88) --------------------------------------
# c4bb executes all ten (hw/microcode.uc), so it can run the differential
# c4th's assembler was written for: every fused opcode against the exact
# instruction sequence it replaces, both hand-assembled into tiny
# functions and called. On the host that runs under c4mp, because c4m
# implements only three of them.
FUSEDISK=$OUT/fused
mkdir -p $FUSEDISK
cp src/c4th/forth/core.f src/c4th/forth/asm.f src/c4th/tests/fused.f $FUSEDISK/
cp c4th32.c4r $FUSEDISK/c4th32.c4r
# A fused image, for the lockstep and same-answers checks: identical
# program, fewer instructions, and it must still agree with its unfused
# twin step for step between the two engines.
./c4sp32 src/c4sp/lisp/c4opt-run.lisp -mfuse $OUT/factorial.c4r $OUT/factorial-fused.c4r > /dev/null

say "c4bb's trap tests, and libjs's demos"
# c4bb's own trap-machinery tests (src/tests/test_customop predates
# TLEV/DBG and uses colliding opcode numbers, so it cannot be used)
for t in bb_customop bb_preempt bb_pm bb_pit bb_whowrote; do
    $CC -o $OUT/$t.c4r src/c4bb/tests/src/$t.c > /dev/null
done
# the mailbox probe: board-only header passed as a source (c4cc has no #include)
$CC -o $OUT/bb_mbox.c4r include/c4bb_mbox.h src/c4bb/tests/src/bb_mbox.c > /dev/null
# libjs's display demo (docs/libjs-design.md): prints "not fitted" on any
# machine without the display, so it is safe in every corpus.
$CC -o $OUT/gui-demo.c4r libjs/guest/gui.h libjs/guest/gui.c libjs/guest/gui-demo.c > /dev/null
$CC -o $OUT/fb-demo.c4r libjs/guest/gui.h libjs/guest/gui.c libjs/guest/fb-demo.c > /dev/null
# and its sound card's: "no sound card" without one
$CC -o $OUT/snd-demo.c4r libjs/guest/snd.h libjs/guest/snd.c libjs/guest/snd-demo.c > /dev/null
$CC -o $OUT/libjs-strc.c4r libjs/tests/src/strc.c > /dev/null

# these include real headers, so they go through the preprocessor
$PREPROC $U0 src/tests/test_vprintf.c 2>/dev/null | $CC -o $OUT/test_vprintf.c4r - > /dev/null
$PREPROC -Isrc/tests $U0 src/tests/test_float.c 2>/dev/null | $CC -o $OUT/test_float.c4r - > /dev/null

# ---- C4KE and its userland ------------------------------------------
# kernel: preprocess + c4lc under the 32-bit c4sp (like the Makefile's
# c4ke-lc.c4r rule, at 32 bits). Skipped when already newer.
DISK=$OUT/disk
mkdir -p $DISK
# Rebuild when c4ke.c OR its embedded loader (#include "./load-c4r.c",
# resolving to the repo-root load-c4r.c) is newer -- else a load-c4r.c
# change (e.g. a new .c4r format version) silently leaves a stale
# kernel that rejects freshly-built images.
# (the kernel #includes its extensions and the mailbox helpers: a change to any of
#  them is a change to the kernel, or an edit there ships in an image built before it)
kernel_stale=0
[ -f $OUT/c4ke32.c4r ] || kernel_stale=1
for dep in src/c4ke/c4ke.c load-c4r.c include/c4bb_mbox.h src/c4ke/extensions/*.c; do
    [ "$dep" -nt $OUT/c4ke32.c4r ] && kernel_stale=1
done
if [ $kernel_stale = 1 ]; then
    say "C4KE's kernel (c4lc)"
    $PREPROC_LC src/c4ke/c4ke.c > .c4bb_klc.c
    ./c4sp32 -c 64000000 src/c4sp/lisp/c4lc.lisp -O .c4bb_klc.c $OUT/c4ke32.c4r > /dev/null
    rm -f .c4bb_klc.c
else
    say "C4KE's kernel is up to date"
fi

say "C4KE's userland (c4cc32)"
# userland (c4cc32, same recipes as the Makefile's bin rules)
BIN=src/c4ke/bin
$CC -o $DISK/init.c4r  $U0 $BIN/ps.c $BIN/eshell.c src/c4ke/services/init.c > /dev/null
$CC -o $DISK/c4sh.c4r  $U0 $BIN/ps.c src/c4sh/c4sh.c src/c4sh/c4sh_builtins.c src/c4sh/c4sh_scripting.c > /dev/null
$CC -o $DISK/eshell.c4r $U0 $BIN/ps.c $BIN/eshell.c > /dev/null
$CC -o $DISK/c4ke.vfs.c4r $U0 src/c4ke/include/service.h src/c4ke/services/c4ke.vfs.c > /dev/null
for t in ls ps cat echo kill spin c4le type xxd badop overrun; do
    $CC -o $DISK/$t.c4r $U0 $BIN/$t.c > /dev/null
done
$CC -o $DISK/top.c4r $U0 $BIN/ps.c $BIN/top.c > /dev/null
# the mailbox from a task: bound to the host, and task to task
for t in mbecho mbpair; do
    $CC -o $DISK/$t.c4r $U0 include/c4ke_mbox.h $BIN/$t.c > /dev/null
done > /dev/null

say "C4KE's vfsload and raycast (c4lc)"
# vfsload: built with c4lc (needs real block scoping, not just c4cc's
# top-of-function declarations). c4lc's own preprocessor hangs on
# u0.h (a pre-existing issue, unrelated to word size - reproduces at
# 64-bit too), so preprocess with gcc -E first, same as the kernel.
$PREPROC_LC $BIN/vfsload.c > .c4bb_vfsload_pp.c
./c4sp32 -c 16000000 src/c4sp/lisp/c4lc.lisp -O .c4bb_vfsload_pp.c $DISK/vfsload.c4r > /dev/null
rm -f .c4bb_vfsload_pp.c

# raycast: also c4lc, also no u0, and additionally -conforming so it
# can write ANSI escapes as "\033[" instead of poking 27 in as an
# integer. No gcc -E here -- it includes nothing, so c4lc's own
# preprocessor handles the one -D and the #ifdef it guards.
./c4sp32 -c 16000000 src/c4sp/lisp/c4lc.lisp -O -conforming -D RC_KE=1 \
    src/tests/raycast.c $DISK/raycast.c4r > /dev/null

say "c4, c4m, c4cc, c4rdump and c4rlink (c4cc32), and the benchmarks"
# the self-hosted core (c4, c4m, c4cc) and the C4R toolchain, same
# recipes as the Makefile's native (64-bit) rules, just via c4cc32
#
# c4.c4r takes NO u0, the way c4m.c4r below it never did. THIS DISK IS
# BOOTED BY C4DOS TOO (that is the whole point of shipping c4dos32.c4r
# here), and u0 is the C4KE runtime: its constructor asks the kernel
# for 38 opcodes, and under DOS there is no kernel to ask. It used to
# "work" only because a missed trap on the board is silent and c4.c
# never calls a C4KE service anyway -- so the failures were invisible
# rather than absent. Now u0 declines outright ("This application
# requires C4KE"), which makes the wrong build visible instead. c4.c is
# self-contained, so the unadorned image is the one that runs on every
# rung -- the same call the Makefile makes for c4-dos32.c4r.
$CC -o $DISK/c4.c4r c4.c > /dev/null
# c4m, through OUR OWN preprocessor.
#
# It was built by raw c4cc for one round, because c4cc skips '#' lines
# and that switched the DOS branch on (F7, F11) where gcc -E had
# compiled it out. It also skips '#include', which nobody noticed: that
# image had no u0.h, no c4.h and no c4m_float.h, and measured from
# inside the machine its __c4_info() had gone from 242 to 131 -- float,
# the high-resolution timer and signals gone, and the C4I_C4 bit that
# makes c4r_load pick c4r_load_opt_pure for every nested load turned on.
# "Compile it with the compiler that ignores the question" is not a way
# to answer a question (docs/dos-rung-fixes.md round four).
#
# ./cpp is a real preprocessor and it is ours -- the same one BUILD.BAT
# runs inside the machine -- so this asks for exactly what it wants:
#   -DC4M_DOS=1     the DOS file API and the DOS clock
#   -DC4M_NO_U0=1   because THIS DISK IS BOOTED BY C4DOS, and u0 declines
#                   to run there by design (F5)
# and everything else -- float, the timer, signals, the host memcpy --
# arrives the way it does in every other build, because the #if that
# selects it is now evaluated rather than skipped.
#
# c4dos.h rides along as a second input file rather than an #include:
# it is a source file by convention (see its own header), and ./cpp
# concatenates its inputs the way gcc -E does.
$OURCPP -DC4M_DOS=1 -DC4M_NO_U0=1 include/c4dos.h c4m.c > .c4bb_c4m_pp.c
$CC -o $DISK/c4m.c4r .c4bb_c4m_pp.c > /dev/null
# kept: src/c4cc/BUILD.B4K makes the same file, and the root archive
# ships what the BUILD.B4K files make (below)
mv .c4bb_c4m_pp.c $OUT/c4m.pp.c
# include/c4dos.h rides along as a source file (c4cc has no
# preprocessor): c4cc reads its input through the DOS API when it is
# running as a transient, so a source another tool just wrote to the
# RAM disk is findable, and writes its output back the same way.
CCSRC="$U0 include/c4dos.h load-c4r.c src/c4cc/c4cc.c src/c4cc/asm-c4r.c"
$CC -o $DISK/c4cc.c4r $CCSRC > /dev/null
$CC -o $DISK/c4rdump.c4r $CCSRC $BIN/c4rdump.c > /dev/null
$CC -o $DISK/c4rlink.c4r $CCSRC $BIN/c4rlink.c > /dev/null

# benchmarks
for t in bench benchtop innerbench; do
    $CC -o $DISK/$t.c4r $U0 src/bench/$t.c > /dev/null
done

cp $OUT/hello32.c4r $DISK/hello.c4r
for t in tests factorial multifun test-order test-ptrs test_continue \
         test_args test_exit test_printloop test_basic test_malloc \
         test_float mandel rps; do
    cp $OUT/$t.c4r $DISK/$t.c4r 2>/dev/null || true
done

say "the C4KE disk's sources and manifest"
# ---- the VFS manifest itself, and the plain-text sources it names --
# c4ke.vfs.txt is curated SOURCE (see src/c4bb/fs/), not a build
# artifact, so it is copied in rather than regenerated - unlike the
# rest of images/, which is safe to rm -rf and rebuild from scratch.
# The other names here are flat host filenames matching what
# BIN_ALL/BIN_TEST name in the manifest; vfsload reads these at boot
# via the same disk device, not ramfs, so they need no compilation -
# cat just shows the bytes.
cp src/c4bb/fs/c4ke.vfs.txt $DISK/

# innerbench spawns a NESTED c4/c4m that itself opens plain source
# files (default mode: "c4m load-c4r.c src/c4ke/c4ke.c --") - these
# are read straight off the disk device by that nested interpreter,
# not through the vfs.txt manifest, so they need to exist under the
# exact paths it opens them by. Our disk keys are plain strings and
# happily hold slashes, so "src/c4ke/c4ke.c" is just another entry
# alongside the flat "c4ke.c" the manifest already places.
cp load-c4r.c $DISK/load-c4r.c
mkdir -p $DISK/src/c4ke
cp src/c4ke/c4ke.c $DISK/src/c4ke/c4ke.c
BIN_ALL_SRC="c4 c4m c4cc bench benchtop innerbench c4rdump c4rlink \
             type xxd top ls ps echo cat kill spin c4le \
             c4ke c4ke.vfs init vfsload c4sh eshell"
for n in $BIN_ALL_SRC; do
    case $n in
        c4) cp c4.c $DISK/c4.c ;;
        c4m) cp c4m.c $DISK/c4m.c ;;
        c4cc) cp src/c4cc/c4cc.c $DISK/c4cc.c ;;
        bench|benchtop|innerbench) cp src/bench/$n.c $DISK/$n.c ;;
        c4rdump|c4rlink) cp $BIN/$n.c $DISK/$n.c ;;
        type|xxd|top|ls|ps|echo|cat|kill|spin|c4le) cp $BIN/$n.c $DISK/$n.c ;;
        c4ke) cp src/c4ke/c4ke.c $DISK/c4ke.c ;;
        c4ke.vfs) cp src/c4ke/services/c4ke.vfs.c $DISK/c4ke.vfs.c ;;
        init) cp src/c4ke/services/init.c $DISK/init.c ;;
        vfsload) cp $BIN/vfsload.c $DISK/vfsload.c ;;
        c4sh) cp src/c4sh/c4sh.c $DISK/c4sh.c ;;
        eshell) cp $BIN/eshell.c $DISK/eshell.c ;;
    esac
done
BIN_TEST_SRC="hello tests factorial multifun test-order test-ptrs \
              test_continue test_args test_exit test_printloop \
              test_basic test_malloc test_float mandel rps"
cp src/tests/raycast.c $DISK/raycast.c
for n in $BIN_TEST_SRC; do
    cp src/tests/$n.c $DISK/$n.c 2>/dev/null || true
done
cp include/c4.h include/c4ke.h include/c4m.h $DISK/
cp src/c4ke/include/service.h $DISK/

# ---- C4IX and its userland ------------------------------------------
# modules compiled by 32-bit c4lc (its own preprocessor), linked by
# the 32-bit c4rlink; userland links against libc4ix
C4IX_MODS="boot console va host sl4b task sched vfs sys c4ke loader init"
C4IX_USER="hello uhello echo wc cat sh ps bench cycles ls mkdir top spin pin nice fmt gui b4ke cc"
# userland that also links libarc (gzip and tar): the root archive's unpacker
C4IX_ARC_USER="tar gunzip crc32 splash persist"
# c4sp arena, in cells. 8000000 was a guess with no measurement behind
# it, and at 21 bytes a cell (32-bit: gc.h/cell.h) that is ~168MB --
# more than c4bb has (32MB default), so nothing built this way could
# ever be built INSIDE the machine. The measured floor is 100k-200k
# cells per module (sys, c4ke and libc4ix are the 200k ones); this is
# 2x the worst, and every object it produces is byte-identical to the
# 8000000 one. ~8.4MB, which fits.
# The native (64-bit) build in the Makefile still says 4000000: these
# floors were measured at 32 bits, and an unmeasured change there would
# be the same guess with a smaller number.
C4IX_CELLS=400000
# Source maps (docs/c4-explorer.md, track S): C4IX is compiled -g, so
# that its kernel, libraries and programs carry maps for the Code
# Explorer. A map changes no byte of an image, only adds a trailer. The
# Makefile passes IMG_G (`make IMG_G=-g0` builds without), and writes it
# to .img_g, so a change to it rebuilds C4IX below.
G=${IMG_G--g}
say "c4mp's SMP guests"
# c4mp's SMP guests, and the CPU-local window's, as 32-bit images for
# libjs's processors (cli.js --cpus, docs/c4ix-smp.md); test-libjs runs them
for g in smp0 smp1 deadlock; do
    ./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O src/c4mp/guest/$g.c $OUT/c4mp-$g.c4r > /dev/null
done
./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O libjs/tests/src/smpwin.c $OUT/libjs-smpwin.c4r > /dev/null
# the same work on every CPU, for the parallel engine (cli.js --parallel)
./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O libjs/tests/src/spinmp.c $OUT/libjs-spinmp.c4r > /dev/null
# What `b4ke -C /usr` makes in /build, laid out as the
# BUILD.B4K files lay it out: each object and image below is kept there
# as it is built (they are the same bytes; test-c4ix-rebuild checks the
# images), and the root archive ships the tree, so a build inside C4IX
# starts with everything up to date.
PB=$OUT/build
keep() { cp "$1" "$PB/$2"; }
# Rebuilt when ANY of C4IX changed -- kernel, libc4ix, headers, user
# programs -- or the display header its programs include, or IMG_G. This used to
# list four kernel files, so an edit to sys.c, vfs.c or a user program
# was silently not built.
if [ ! -f $OUT/c4ix32.c4r ] || [ ! -f $PB/c4ix.c4r ] || \
   [ -n "$(find src/c4ix src/winkit libjs/guest/gui.h libjs/guest/gui.c libjs/guest/snd.h libjs/guest/snd.c $(ls .img_g 2>/dev/null) -newer $OUT/c4ix32.c4r -type f -print -quit)" ]; then
    say "C4IX, compiled $G (c4lc)"
    rm -rf $PB
    mkdir -p $PB/kernel $PB/lib $PB/libc $PB/user $PB/desktop $PB/bin
    objs=""
    step "C4IX's kernel:"
    for m in $C4IX_MODS; do
        item $m
        ./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O $G -c -I src/c4ix src/c4ix/$m.c .c4bb_ix_$m.c4o > /dev/null
        objs="$objs .c4bb_ix_$m.c4o"
        keep .c4bb_ix_$m.c4o kernel/$m.c4o
    done
    echo
    ./c4rlink32 $objs -o $OUT/c4ix32.c4r
    keep $OUT/c4ix32.c4r c4ix.c4r
    say "C4IX's libraries: libc4ix and libarc, static and shared, and libgui"
    ./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O $G -c -I src/c4ix/include src/c4ix/lib/libc4ix.c .c4bb_ix_lib.c4o > /dev/null
    ./c4rlink32 -r .c4bb_ix_lib.c4o -o .c4bb_libc4ix32.c4l
    keep .c4bb_ix_lib.c4o lib/libc4ix.c4o
    keep .c4bb_libc4ix32.c4l lib/libc4ix.c4l
    # ... and as C4IX's first shared library, /lib/libc4ix.c4s: the same
    # image, which programs linked with -l load at run time. c4rlink
    # records /lib/ and the base name, so the copy it reads is named so.
    mkdir -p .c4bb_lib
    # libarc: inflate, CRC-32 and tar, for vfsload's root archive. -mfuse
    # like c4sp.c4r below: unpacking the archive is the one long job a
    # boot starts, and both machines that read this disk run fused code.
    ./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O $G -mfuse -c -I src/c4ix/include src/c4ix/lib/archive.c .c4bb_ix_arc.c4o > /dev/null
    ./c4rlink32 -r .c4bb_ix_arc.c4o -o .c4bb_libarc32.c4l
    keep .c4bb_ix_arc.c4o lib/libarc.c4o
    keep .c4bb_libarc32.c4l lib/libarc.c4l
    # ... and C4IX's shared libraries, which its programs load (c4rlink -l):
    # -fpic, so their code is loaded once and shared by every task. libc4ix
    # has window.h's and cpustat.h's functions too, and libgui the
    # display's (gui.h) and the sound card's (snd.h), so a program built
    # -D C4IX_SHARED carries none of
    # them itself (docs/c4ix-design.md, "Code out of the headers").
    LCPIC="./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O $G -fpic -c -D C4IX_SHARED -I src/c4ix/include"
    $LCPIC src/c4ix/lib/libc4ix.c .c4bb_ix_s_libc4ix.c4o > /dev/null
    $LCPIC src/c4ix/include/window.c .c4bb_ix_s_window.c4o > /dev/null
    $LCPIC src/c4ix/include/cpustat.c .c4bb_ix_s_cpustat.c4o > /dev/null
    ./c4rlink32 -r .c4bb_ix_s_libc4ix.c4o .c4bb_ix_s_window.c4o .c4bb_ix_s_cpustat.c4o -o .c4bb_lib/libc4ix.c4s > /dev/null
    $LCPIC -mfuse src/c4ix/lib/archive.c .c4bb_ix_s_arc.c4o > /dev/null
    ./c4rlink32 -r .c4bb_ix_s_arc.c4o -o .c4bb_lib/libarc.c4s > /dev/null
    $LCPIC -I libjs/guest libjs/guest/gui.c .c4bb_ix_s_gui.c4o > /dev/null
    $LCPIC -I libjs/guest libjs/guest/snd.c .c4bb_ix_s_snd.c4o > /dev/null
    ./c4rlink32 -r .c4bb_ix_s_gui.c4o .c4bb_ix_s_snd.c4o -o .c4bb_lib/libgui.c4s > /dev/null
    for m in libc4ix window cpustat arc gui snd; do keep .c4bb_ix_s_$m.c4o lib/shared-$m.c4o; done
    for m in libc4ix libarc libgui; do
        keep .c4bb_lib/$m.c4s lib/$m.c4s
        cp .c4bb_lib/$m.c4s $DISK/c4ix-$m.c4s
    done
    # libc: C4IX's C library (src/c4ix/libc), the second shared library,
    # /lib/libc.c4s. Built -fno-builtin, so printf, malloc and the rest
    # are its own -- except opcodes.c, which is where the VM's opcodes
    # are -- and with libc4ix's syscall layer linked in. Built -fpic,
    # all of it, so its code is loaded once and shared by every task.
    # fp.c (printf's %f, strtod) and math.c are the units that say double.
    step "C4IX's libc:"
    for m in string stdlib stdio unix fp math; do
        item $m
        ./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O $G -fno-builtin -fpic -c -I src/c4ix/libc/include -I src/c4ix/libc src/c4ix/libc/$m.c .c4bb_ix_libc_$m.c4o > /dev/null
        keep .c4bb_ix_libc_$m.c4o libc/$m.c4o
    done
    ./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O $G -fpic -c src/c4ix/libc/opcodes.c .c4bb_ix_libc_opcodes.c4o > /dev/null
    ./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O $G -fpic -c -I src/c4ix/include src/c4ix/lib/libc4ix.c .c4bb_ix_libc_u.c4o > /dev/null
    ./c4rlink32 -r .c4bb_ix_libc_string.c4o .c4bb_ix_libc_stdlib.c4o .c4bb_ix_libc_stdio.c4o .c4bb_ix_libc_unix.c4o \
        .c4bb_ix_libc_fp.c4o .c4bb_ix_libc_math.c4o .c4bb_ix_libc_opcodes.c4o .c4bb_ix_libc_u.c4o -o .c4bb_lib/libc.c4s > /dev/null
    echo
    keep .c4bb_ix_libc_opcodes.c4o libc/opcodes.c4o
    keep .c4bb_ix_libc_u.c4o libc/libc4ix.c4o
    keep .c4bb_lib/libc.c4s lib/libc.c4s
    cp .c4bb_lib/libc.c4s $DISK/c4ix-libc.c4s
    say "libc's tests: libctest, pictest, unixtest"
    # libctest exercises it (libjs/tests/test-libjs.sh compares its output)
    ./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O $G -fno-builtin -c -I src/c4ix/libc/include src/c4ix/libc/tests/libctest.c .c4bb_ix_u.c4o > /dev/null
    ./c4rlink32 .c4bb_ix_u.c4o -l .c4bb_lib/libc.c4s -o $DISK/c4ix-libctest.c4r > /dev/null
    keep .c4bb_ix_u.c4o libc/libctest.c4o
    keep $DISK/c4ix-libctest.c4r bin/c4ix-libctest.c4r
    # pictest: two at once must each keep their own libc data
    ./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O $G -fno-builtin -c -I src/c4ix/libc/include src/c4ix/libc/tests/pictest.c .c4bb_ix_u.c4o > /dev/null
    ./c4rlink32 .c4bb_ix_u.c4o -l .c4bb_lib/libc.c4s -o $DISK/c4ix-pictest.c4r > /dev/null
    keep .c4bb_ix_u.c4o libc/pictest.c4o
    keep $DISK/c4ix-pictest.c4r bin/c4ix-pictest.c4r
    # unixtest: lseek, fstat, links, waitpid, vfork and execve (M7c)
    ./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O $G -fno-builtin -c -I src/c4ix/libc/include src/c4ix/libc/tests/unixtest.c .c4bb_ix_u.c4o > /dev/null
    ./c4rlink32 .c4bb_ix_u.c4o -l .c4bb_lib/libc.c4s -o $DISK/c4ix-unixtest.c4r > /dev/null
    keep .c4bb_ix_u.c4o libc/unixtest.c4o
    keep $DISK/c4ix-unixtest.c4r bin/c4ix-unixtest.c4r
    step "C4IX's programs:"
    for u in $C4IX_USER; do
        item $u
        ./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O $G -c -D C4IX_SHARED -I src/c4ix/include src/c4ix/user/$u.c .c4bb_ix_u.c4o > /dev/null
        # each loads libc4ix at run time; gui, the display's demo, libgui too
        if [ $u = gui ]; then
            ./c4rlink32 .c4bb_ix_u.c4o -l .c4bb_lib/libgui.c4s -l .c4bb_lib/libc4ix.c4s -o $DISK/c4ix-$u.c4r > /dev/null
        else
            ./c4rlink32 .c4bb_ix_u.c4o -l .c4bb_lib/libc4ix.c4s -o $DISK/c4ix-$u.c4r > /dev/null
        fi
        keep .c4bb_ix_u.c4o user/$u.c4o
        keep $DISK/c4ix-$u.c4r bin/c4ix-$u.c4r
    done
    for u in $C4IX_ARC_USER; do
        item $u
        # splash runs while the boot unpacks the root archive, before
        # anything else may have been: it carries its libraries itself
        if [ $u = splash ]; then
            ./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O $G -c -I src/c4ix/include src/c4ix/user/$u.c .c4bb_ix_u.c4o > /dev/null
            ./c4rlink32 .c4bb_ix_u.c4o .c4bb_libarc32.c4l .c4bb_libc4ix32.c4l -o $DISK/c4ix-$u.c4r > /dev/null
        else
            ./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O $G -c -D C4IX_SHARED -I src/c4ix/include src/c4ix/user/$u.c .c4bb_ix_u.c4o > /dev/null
            ./c4rlink32 .c4bb_ix_u.c4o -l .c4bb_lib/libarc.c4s -l .c4bb_lib/libc4ix.c4s -o $DISK/c4ix-$u.c4r > /dev/null
        fi
        keep .c4bb_ix_u.c4o user/$u.c4o
        keep $DISK/c4ix-$u.c4r bin/c4ix-$u.c4r
    done
    echo
    # the desktop: several modules, linked together (docs/c4ix-desktop.md)
    dobjs=""
    step "C4IX's desktop:"
    # (the C4IX shell's own, then the window kit's, src/winkit, which grin shares)
    for m in shell tools host term files taskmgr apps edit wall draw ui wmcore dialog look95 look31 textgrid savers calc notepad clock; do
        item $m
        src=src/c4ix/user/desktop/$m.c
        [ -f $src ] || src=src/winkit/$m.c
        ./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O $G -c -D C4IX_SHARED -I src/c4ix/include -I src/c4ix/user/desktop \
            -I src/winkit $src .c4bb_ix_d_$m.c4o > /dev/null
        dobjs="$dobjs .c4bb_ix_d_$m.c4o"
        keep .c4bb_ix_d_$m.c4o desktop/$m.c4o
    done
    # the Calculator's decimal conversions are libc's fp.c, linked in
    ./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O $G -c src/c4ix/libc/fp.c .c4bb_ix_d_fp.c4o > /dev/null
    dobjs="$dobjs .c4bb_ix_d_fp.c4o"
    echo
    keep .c4bb_ix_d_fp.c4o desktop/fp.c4o
    ./c4rlink32 $dobjs -l .c4bb_lib/libgui.c4s -l .c4bb_lib/libarc.c4s -l .c4bb_lib/libc4ix.c4s -o $DISK/c4ix-desktop.c4r > /dev/null
    keep $DISK/c4ix-desktop.c4r bin/c4ix-desktop.c4r
    rm -f $dobjs .c4bb_ix_d_*.pp.c
    # the VFS loader: c4ix-vfsload.c4r, NOT plain vfsload.c4r - C4KE
    # has its own boot-time loader of the same name on this shared
    # disk (src/c4ke/bin/vfsload.c), and the two would otherwise
    # silently overwrite each other (they did, until this was caught:
    # C4IX's build ran second, so C4KE started running C4IX's loader
    # under itself - "c4ix-" keeps it consistent with every other
    # C4IX binary here, all of which face the same shared-disk risk).
    say "C4IX's vfsload"
    ./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O $G -c -I src/c4ix/include src/c4ix/user/vfsload.c .c4bb_ix_u.c4o > /dev/null
    ./c4rlink32 .c4bb_ix_u.c4o .c4bb_libc4ix32.c4l -o $DISK/c4ix-vfsload.c4r > /dev/null
    keep .c4bb_ix_u.c4o user/vfsload.c4o
    keep $DISK/c4ix-vfsload.c4r bin/c4ix-vfsload.c4r
    # the libraries are kept: the root archive ships them in /usr/lib,
    # so a program can be linked inside the machine
    mv .c4bb_libc4ix32.c4l $OUT/libc4ix32.c4l
    rm -rf .c4bb_lib
    mv .c4bb_libarc32.c4l $OUT/libarc32.c4l
    rm -f .c4bb_ix_*.c4o .c4bb_ix_*.pp.c
else
    say "C4IX is up to date"
fi

# The games (games/README.md): each one checked out links itself for C4IX
# here, from the objects its c4.mk made, against the libc kept from the
# last time C4IX was built, and puts its data on the disk
# (games/NAME/c4-images.sh).
GAMES=$(for g in games/*/c4-images.sh; do if [ -f "$g" ]; then dirname "$g"; fi; done)
for g in $GAMES; do . $g/c4-images.sh c4ix; done

# ... and with no C4IX under them: the bare machine, and C4DOS (c4's bare
# libc, make libc-bare-objs). The same game objects, but for the C4IX
# platform's (plat-bare.c, dg-bare.c), with libc. Their disk, $OUT/games,
# has the games' files unpacked, since neither the bare machine nor DOS
# can write to a drive, and C4DOS's own files, so it boots C4DOS too.
GAMESDISK=$OUT/games
GAMES_ON_DISK=
GAMEFILES=$OUT/.gamefiles
rm -rf $OUT/wolf3d $GAMESDISK $GAMEFILES
mkdir -p $GAMEFILES
# A bare program carries what it uses of libc and no more: libc's objects
# are library members, linked only if something needs what they define,
# and --gc drops every function nothing reaches (c4rlink.c's header;
# docs/c4dos-design.md "Linking a bare program"). The games' own scripts
# name ./c4rlink32 and .libc-bare's objects; while they run, this stands
# in for it and adds the two, so they need no change for it.
./c4rlink32() {
    local a lib= args=()
    case " $* " in *" --start-lib "*) command ./c4rlink32 "$@"; return ;; esac
    for a in "$@"; do
        case "$a" in .libc-bare/*) if [ -z "$lib" ]; then args+=(--start-lib); lib=1; fi ;; esac
        args+=("$a")
    done
    if [ -n "$lib" ]; then command ./c4rlink32 --gc "${args[@]}"; else command ./c4rlink32 "$@"; fi
}
if [ -n "$GAMES" ] && ls .libc-bare/*.c4o > /dev/null 2>&1; then
    mkdir -p $GAMESDISK
    step "linking the games for the bare machine and C4DOS:"
    # The games disk stays flat (the bare machine has no directories), but
    # the page's C4DOS disks give each game a directory of its own
    # (\GAMES\DOOM), so what each one put here is noted: $GAMEFILES/NAME.
    for g in $GAMES; do
        (cd $GAMESDISK && find . -type f | LC_ALL=C sort) > $GAMEFILES.before
        . $g/c4-images.sh bare
        (cd $GAMESDISK && find . -type f | LC_ALL=C sort) | LC_ALL=C comm -13 $GAMEFILES.before - | sed 's|^\./||' \
            > $GAMEFILES/$(basename $g | tr a-z A-Z)
    done
    rm -f $GAMEFILES.before
    echo
fi
unset -f ./c4rlink32
if [ -n "$GAMES_ON_DISK" ]; then
    say "the games disk: their data, C4DOS's files, the indexes"
    # (C4DOS's own CONFIG.SYS, made as the C4DOS step below makes the
    # shared disk's, which in a fresh tree is not there yet)
    sed 's/SIZE=[0-9]*/SIZE=16777216/' src/c4dos/fs/CONFIG.SYS > $GAMESDISK/config.sys
    printf 'DEVICE=CONSOLE.SYS FLUSH\nDEVICE=DRIVES.SYS\n' >> $GAMESDISK/config.sys
    # The page runs C4DOS on libjs, whose heap keeps generations, so on
    # this disk a game's memory goes back when it quits (DEVICE=HEAP.SYS,
    # docs/c4dos-design.md "Memory"). Without the line Doom left 14 MB
    # behind each time it ran.
    echo 'DEVICE=HEAP.SYS' >> $GAMESDISK/config.sys
    list=$(echo $GAMES_ON_DISK | sed 's/ /, /g')
    printf '@ECHO OFF\nECHO Welcome to C4DOS\nVER\nECHO These games are on this disk: type %s to play.\n' "$list" > $GAMESDISK/autoexec.bat
    (cd $GAMESDISK && ls -p | grep -v '/$' | grep -v '^manifest.json$' > c4dos.dir)
    (cd $GAMESDISK && find . -type f -not -name manifest.json | sed 's|^\./||') | \
        awk 'BEGIN{printf "["} NR>1{printf ","} {printf "\"%s\"", $0} END{print "]"}' > $GAMESDISK/manifest.json
    # and lazy for the web page, as $DISK is below: a file is fetched the
    # first time the machine opens it, so a package's zip costs nothing
    (cd $GAMESDISK && find . -type f -not -name manifest.json -not -name index.json -printf '%s %P\n' | sort -k2) | \
        awk 'BEGIN{printf "["} NR>1{printf ","} {n=$0; sub(/^[0-9]+ /,"",n); printf "[\"%s\",%s]", n, $1} END{print "]"}' \
        > $GAMESDISK/index.json
else
    rm -rf $GAMESDISK
fi

# the C4IX VFS manifest and the plain-text sources it references -
# c4ix.vfs.txt is curated SOURCE (see src/c4bb/fs/), copied in like
# c4ke.vfs.txt above, not regenerated with the rest of images/.
# Sources are named c4ix-NAME.c, the same prefix the binaries already
# use, because C4KE and C4IX share this one flat disk and both have a
# "cat.c"/"ps.c"/"top.c" etc of their own - without the prefix the
# two systems' vfsload runs would silently overwrite each other's
# source copies.
say "C4IX's sources on the disk, and c4m as a C4IX program (c4lc)"
# with the block of each game that is on the disk (games/NAME/c4ix.vfs.txt)
python3 -I src/c4bb/tests/vfs-games.py src/c4bb/fs/c4ix.vfs.txt $DISK/c4ix.vfs.txt \
    $(for g in $GAMES; do if [ -f $DISK/c4ix-$(basename $g).c4r ]; then echo $g; fi; done)
# the boot logo, for splash (src/c4ix/art/mkc4p.py made it from Kiln's PNG)
cp src/c4ix/art/c4ix.c4p.gz $DISK/c4ix-splash.c4p.gz
for n in $C4IX_USER $C4IX_ARC_USER; do
    cp src/c4ix/user/$n.c $DISK/c4ix-$n.c
done
# the desktop's modules, one after another, as its one source file
cat src/winkit/winkit.h src/winkit/wkwin.h src/winkit/textgrid.h src/c4ix/user/desktop/desktop.h \
    src/c4ix/user/desktop/*.c src/winkit/*.c > $DISK/c4ix-desktop.c
cp src/c4ix/c4ix.h $DISK/c4ix.h
cp src/c4ix/include/c4ix_user.h $DISK/c4ix_user.h
# c4m as a C4IX program: c4m.c built the way cc builds it inside the
# machine (a C4KE program, run through C4IX's compatibility layer). It
# runs an image as c4m always has -- c4m load-c4r.c -- /usr/lib/c4ke.c4r --
# and lends its guest the FLT of the machine under it.
./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O $G -P -I src/c4ix/include -I include -I . -D C4CC=1 -D __c4cc__=1 c4m.c $DISK/c4ix-c4m.c4r > /dev/null
# what C4IX's own compilers are for (docs/c4ix-desktop.md, part three)
cp src/c4ix/include/window.h $DISK/c4ix-window.h
for f in README.txt hello.c bounce.c; do cp src/c4ix/examples/$f $DISK/c4ix-examples-$f; done
# c4th, the Forth that compiles images to stdout (the C4IX shell's
# redirection is its file write), with the sources it reads.
cp c4th32.c4r $DISK/c4th.c4r
mkdir -p $DISK/src/c4th/forth $DISK/src/c4th/tests
cp src/c4th/forth/*.f $DISK/src/c4th/forth/
cp src/c4th/tests/self1.f src/c4th/tests/self1-c4r.f src/c4th/tests/run-main.f $DISK/src/c4th/tests/

# ---- the ladder ------------------------------------------------------
# The ladder's premise is that you climb this disk: C4DOS boots, C4DOS
# boots C4KE, C4KE's RAM filesystem is what finally lets a compiler
# WRITE, and that is what makes C4IX buildable. So the systems above
# each rung have to BE here, as images the machine can load.
#
# The rung is closed as of the DOS API work: C4DOS has a RAM disk
# (DEVICE=RAMDISK.SYS), transients reach it through __c4dos_api
# (include/c4dos.h), so cpp and c4cc can WRITE under DOS -- and the
# kernel extension c4ke_dos.c copies that whole RAM disk into C4KE's
# own RAM filesystem at boot, so the kernel and init C4DOS just built
# are what actually run. c4sp/c4lc still only know how to find C4KE's
# ramfs (custom opcodes a trap-free DOS does not offer), which is
# correct: they are the C4KE rung's tools, used to build C4IX.
# docs/ladder-plan.md tracked the whole climb.
say "the ladder: c4sp (c4lc -O -mfuse), cpp, dostar, dosload and the build kit"
cp $OUT/c4ke32.c4r $DISK/c4ke.c4r
cp $OUT/c4ix32.c4r $DISK/c4ix.c4r

# The toolchain, as machine-loadable images. c4/c4m/c4cc/c4rlink/
# c4rdump are copied by the BIN_ALL loop above; these two were only
# ever host binaries.
# c4sp is the compiler's host, so it is the one image on this disk worth
# building the expensive way: c4lc -O rather than c4cc (A1.5), and -mfuse
# because THIS MACHINE HAS ALL TEN FUSED OPCODES and nothing else that
# reads this disk does. Measured on a real C4IX module, c4lc -R -O -c:
# 1,646,519,810 instructions unfused against 888,446,891 fused, 81.20s
# against 51.84s. The image is smaller too, 124,361 -> 105,864 bytes.
# docs/fused-opcodes.md F7.
$PREPROC -DC4SP_DOS=1 src/c4sp/c4sp.c > .c4bb_c4sp_pp.c
./c4sp32 -R src/c4sp/lisp/c4lc.lisp -O -mfuse .c4bb_c4sp_pp.c $DISK/c4sp.c4r > /dev/null
rm -f .c4bb_c4sp_pp.c
$CC -o $DISK/cpp.c4r include/c4dos.h src/c4dos/cpp.c > /dev/null

# c4lc is not an image at all -- it is Lisp that c4sp reads at runtime,
# by bare filename, from the working directory. On this machine that
# directory is the disk, so the whole toolchain has to sit at the top
# level beside c4sp.c4r or none of it loads.
cp src/c4sp/lisp/*.lisp $DISK/

# u0.h is what a C4KE program is compiled against, and the C4IX kernel
# sources are what c4lc is FOR. Both go under src/, mirroring the repo:
# the disk is flat but its keys hold slashes, so a path is just a
# longer name, and src/c4ix/init.c stops colliding with C4KE's own
# init.c at the top level.
cp include/u0.h $DISK/u0.h
mkdir -p $DISK/src/c4ix/include $DISK/src/c4ix/lib $DISK/src/c4ix/user $DISK/src/c4sp
# Two sources the disk carried the BINARIES of but not the source: you
# could run c4sp and C4IX's vfsload on this machine and not rebuild
# either of them, which is exactly the thing the ladder is about.
cp src/c4sp/c4sp.c              $DISK/src/c4sp/c4sp.c
cp src/c4ix/user/vfsload.c      $DISK/src/c4ix/user/vfsload.c
for m in $C4IX_MODS; do cp src/c4ix/$m.c $DISK/src/c4ix/$m.c; done
cp src/c4ix/c4ix.h              $DISK/src/c4ix/c4ix.h
cp src/c4ix/include/c4ix_user.h $DISK/src/c4ix/include/c4ix_user.h
cp src/c4ix/lib/libc4ix.c       $DISK/src/c4ix/lib/libc4ix.c

# The build kit: one archive of every source a kernel build reaches
# for, the tool that unpacks it, and the batch that drives the whole
# thing. This is the ladder made runnable -- boot c4dos32.c4r on this
# disk, type BUILD, and the machine compiles its own next operating
# system out of the RAM disk.
$CC -o $DISK/dostar.c4r include/c4dos.h src/c4dos/dostar.c > /dev/null
# dosload: LOADLIN for C4DOS. Not required to boot a kernel (plain RUN
# works and injects the API too) -- it is what frees DOS's 4MB scratch
# first and gives the kernel a command line of its own.
$CC -o $DISK/dosload.c4r include/c4dos.h src/c4dos/dosload.c > /dev/null
cp c4ke-src.tar $DISK/ 2>/dev/null || make c4ke-src.tar > /dev/null 2>&1 && cp c4ke-src.tar $DISK/
cp src/c4dos/fs/BUILD.BAT $DISK/build.bat

# ---- C4DOS ----------------------------------------------------------
# The third system on this disk. Built with our own cpp rather than
# gcc -E, because c4dos.c's whole point is that the toolchain can eat
# its own food; raw c4cc32 would skip the '#' lines and compile both
# sides of the clock #if into one image. The clock build is the one
# worth shipping: c4bb HAS a TIME device, and DEVICE=CLOCK.SYS gates it
# at runtime anyway.
#
# config.sys/autoexec.bat/c4dos.dir go on the disk so c4dos32.c4r boots
# against the SAME disk as c4ke32 and c4ix32 -- an embedder that
# already serves this directory gets DOS for the cost of one more
# image. c4dos.dir is generated last, below, because it has to list
# whatever actually ended up here.
if [ -x ./cpp ]; then
    say "C4DOS"
    ./cpp -DC4DOS_CLOCK=1 src/c4dos/c4dos.c > .c4bb_dos_pp.c
    $CC -o $OUT/c4dos32.c4r .c4bb_dos_pp.c > /dev/null
    rm -f .c4bb_dos_pp.c
    sed 's/SIZE=[0-9]*/SIZE=16777216/' src/c4dos/fs/CONFIG.SYS > $DISK/config.sys
    # The board's UART emits each byte as it is written, so the tight
    # A> prompt works here. DOS defaults to a prompt on its own line,
    # because a host libc buffers a partial one.
    echo 'DEVICE=CONSOLE.SYS FLUSH' >> $DISK/config.sys
    # ...and the drives, for the same announced-never-probed reason.
    echo 'DEVICE=DRIVES.SYS' >> $DISK/config.sys
    cp src/c4dos/fs/AUTOEXEC.BAT $DISK/autoexec.bat
else
    echo "c4bb: no ./cpp, skipping c4dos32 (run 'make cpp')" >&2
fi

# C4DOS's directory service IS a file: DIR types c4dos.dir, because the
# raw disk cannot enumerate itself. It is also the name resolver --
# dos_open falls back to a case-insensitive scan of this listing and
# opens the spelling it finds -- so it must carry TRUE on-disk names.
# Generated here, after every other copy.
if [ -f $DISK/config.sys ]; then
    (cd $DISK && ls -p | grep -v '/$' | grep -v '^manifest.json$' > c4dos.dir)
fi

# ---- the page's C4DOS disk: libjs/images/dos -------------------------------
# What ?system=c4dos boots (docs/grin-plan.md 3.9): C4DOS, starting grin,
# its graphical shell, from AUTOEXEC.BAT (File > Exit grin comes back to
# A>, where GRIN starts it again), and the games with their data, linked
# from the games disk where the filesystem allows. CONFIG.SYS is its own:
# HEAP.SYS, because libjs's heap keeps generations and a game's memory
# goes back when it quits; DRIVES.SYS HOME=B:, so that a file saved with
# no drive goes to B:, which the page keeps in the browser (the c4dos
# store), and is still there at the next visit; REDIR.SYS TAP, so that
# > >> < and | work at A> and in grin's DOS Prompt windows, for c4cc
# programs too (the machine's console tap). grin is linked whole with the
# bare libc, as the games are, from the objects make grin-objs compiled.
#
# Both page disks are in directories (C4DOS's API v7, docs/c4dos-design.md
# "Directories"): every directory has its own c4dos.dir, a subdirectory a
# "NAME/" line in its parent's, made here by `listings`. lj_put puts a
# file onto one of them at a path, linked where the filesystem allows;
# games_put puts each game's files into GAMES/NAME.
listings () {           # listings DIR: a c4dos.dir in every directory under DIR
    (cd $1 && find . -type d | while read -r d; do
        (cd "$d" && { ls -p | grep '/$' || true; ls -p | grep -v '/$' | grep -v '^manifest.json$\|^index.json$' || true; } > c4dos.dir)
    done)
}
lj_put () {             # lj_put SRC DISK PATH
    mkdir -p "$2/$(dirname "$3")"
    rm -f "$2/$3"
    ln "$1" "$2/$3" 2> /dev/null || cp "$1" "$2/$3"
}
game_of () {            # game_of PATH: the game that put PATH on the games disk, or nothing
    for gf in $GAMEFILES/*; do
        if [ -f "$gf" ] && grep -qxF "$1" "$gf"; then basename "$gf"; return 0; fi
    done
    return 0
}
games_put () {          # games_put DISK: each game's files into DISK/GAMES/NAME
    for gf in $GAMEFILES/*; do
        [ -f "$gf" ] || continue
        while read -r f; do
            if [ -f "$GAMESDISK/$f" ]; then lj_put "$GAMESDISK/$f" $1 "GAMES/$(basename $gf)/$f"; fi
        done < $gf
    done
    return 0
}
# the games' directories, for APPEND: "\GAMES\DOOM;\GAMES\ROTT"
games_dirs () {
    for gf in $GAMEFILES/*; do if [ -s "$gf" ]; then printf '\\GAMES\\%s;' "$(basename $gf)"; fi; done | sed 's/;$//'
}
disk_indexes () {       # disk_indexes DISK: manifest.json and index.json, every file in every directory
    (cd $1 && find . -type f -not -name manifest.json | sed 's|^\./||') | \
        awk 'BEGIN{printf "["} NR>1{printf ","} {printf "\"%s\"", $0} END{print "]"}' > $1/manifest.json
    (cd $1 && find . -type f -not -name manifest.json -not -name index.json -printf '%s %P\n' | sort -k2) | \
        awk 'BEGIN{printf "["} NR>1{printf ","} {n=$0; sub(/^[0-9]+ /,"",n); printf "[\"%s\",%s]", n, $1} END{print "]"}' \
        > $1/index.json
}

LJDOS=libjs/images/dos
rm -rf $LJDOS
# Its directories: GRIN (grin and its applications), BIN (DOS programs),
# GAMES\NAME (each game and its data); at the root only C4DOS and its two
# files. AUTOEXEC.BAT puts GRIN and BIN on the PATH.
if ls .grin/*.c4o > /dev/null 2>&1 && ls .libc-bare/*.c4o > /dev/null 2>&1 && [ -f $OUT/c4dos32.c4r ]; then
    say "the page's C4DOS disk, booting into grin: $LJDOS"
    mkdir -p $LJDOS/GRIN $LJDOS/BIN
    # libc as library members, and --gc, as the games above
    ./c4rlink32 --gc .grin/*.c4o --start-lib .libc-bare/libc-*.c4o -o $LJDOS/GRIN/grin.c4r > /dev/null
    # its applications (docs/grin-plan.md 3.4): main, the kit's class, and
    # import.c in the kit's place; EXITKEY is test-grin's
    for app in clock calc notepad exitkey; do
        [ -f .grinapp/$app.c4o ] || continue
        kit=; [ -f .grinapp/wk-$app.c4o ] && kit=.grinapp/wk-$app.c4o
        ./c4rlink32 --gc .grinapp/$app.c4o $kit .grinapp/import.c4o --start-lib .libc-bare/libc-*.c4o -o $LJDOS/GRIN/$app.c4r > /dev/null
    done
    # DOS programs for trying redirection with, and test-grin's: ASKLINE
    # (bare.c: reads a line through DOS), DOSFILT (a filter; -r is a plain
    # c4cc program's READ and printf, the tap's) and HELLO (c4cc's printf)
    [ -f .grinapp/askline.c4o ] && ./c4rlink32 --gc .grinapp/askline.c4o --start-lib .libc-bare/libc-*.c4o -o $LJDOS/BIN/askline.c4r > /dev/null
    $CC -o $LJDOS/BIN/dosfilt.c4r include/c4dos.h src/tests/dosfilt.c > /dev/null
    cp $OUT/hello32.c4r $LJDOS/BIN/hello.c4r
    cp $OUT/c4dos32.c4r $LJDOS/
    if [ -d $GAMESDISK ]; then games_put $LJDOS; fi
    sed 's/SIZE=[0-9]*/SIZE=16777216/' src/c4dos/fs/CONFIG.SYS > $LJDOS/config.sys
    printf 'DEVICE=CONSOLE.SYS FLUSH\nDEVICE=DRIVES.SYS HOME=B:\nDEVICE=HEAP.SYS\nDEVICE=REDIR.SYS TAP\n' >> $LJDOS/config.sys
    list=$(echo $GAMES_ON_DISK | sed 's/ /, /g')
    {
        printf '@ECHO OFF\nECHO Welcome to C4DOS\nVER\nPATH \\GRIN;\\BIN\n'
        if [ -n "$list" ]; then
            printf 'ECHO The games on this disk, each in its directory in \\GAMES: %s.\n' "$list"
            printf 'ECHO CD \\GAMES\\DOOM, then DOOM, plays Doom; CD \\ comes back.\n'
        fi
        # (no > in an ECHO here: REDIR.SYS would take it for a redirection)
        printf 'ECHO Starting grin: its File menu has Exit grin, back to the prompt.\nGRIN\n'
    } > $LJDOS/autoexec.bat
    listings $LJDOS
    disk_indexes $LJDOS
fi

# c4sc, the compiled c4lc, so C4IX can rebuild itself: /usr's
# BUILD.B4K files run it (docs/c4sc-design.md). The c4ke-root disk below
# carries the same image.
[ -f c4sc32-fused.c4r ] && cp c4sc32-fused.c4r $DISK/c4sc.c4r
# It is copied, not built: `make c4bb-images` builds it first (about two
# minutes after a change to c4lc). Run on its own, this script says when
# the copy is older than what it is compiled from -- C4IX would rebuild
# itself with yesterday's compiler, which once overran the stack
# compiling c4ke.c in test-libjs.
if [ -f c4sc32-fused.c4r ]; then
    for src in src/c4sp/lisp/c4lc*.lisp src/c4sp/lisp/c4r.lisp src/c4sp/lisp/c4opt.lisp \
               src/c4sp/include/gc.h src/c4sc/c4sc.lisp src/c4sc/c4sc-main.c src/c4sc/*.h; do
        if [ "$src" -nt c4sc32-fused.c4r ]; then
            echo "c4bb: WARNING: c4sc32-fused.c4r is older than $src." >&2
            echo "c4bb:          It is COPIED here, not built -- run 'make c4sc32-fused.c4r'." >&2
            break
        fi
    done
fi

# ---- the C4IX root archive -----------------------------------------
# One gzipped tar of the repo itself, unpacked over / by C4IX's vfsload
# at boot (ARCHIVES in c4ix.vfs.txt, in the background).
# The point is the request count: the web page fetches each disk file
# separately, so a few hundred more files as manifest entries would be a
# few hundred more round trips, where this is one. /usr IS the repo as
# git has it (text files only): its src/ is /usr/src, its include/ is
# /usr/include -- beside C4IX's own headers, so a C4 program's
# `#include <u0.h>` and a C4IX program's <c4ix_user.h> both just work --
# and its docs, Makefile and BUILD.B4K are /usr's. The C4IX libraries
# are in /usr/lib. Reproducible: fixed order, owner and times, gzip -n.
#
# At its end, /build: what `b4ke -C /usr` makes, already made
# (PB above), so a build inside C4IX -- the IDE's, or b4ke's -- starts
# with everything up to date and remakes only what was edited. Order is
# what makes that true: a file's mtime is the RAM filesystem's clock
# when it was written (vfs.c), and the archive is written in order, so
# these come after every source, objects before the libraries made from
# them, and libraries before what links them. (In the same archive, not
# a second one: vfsload unpacks each archive in its own task, at once.)
say "the C4IX root archive, c4ix-root.tgz"
ROOT=.c4bb_ix_root
rm -rf $ROOT
mkdir -p $ROOT/usr/lib $ROOT/usr/include
if git rev-parse --git-dir > /dev/null 2>&1; then
    git ls-files -- src docs include libjs README.md LICENSE Makefile BUILD.B4K INSTALL.B4K c4.c c4m.c c4m_float.c c4l.c load-c4r.c |
        grep -v '^src/c4bb/images/\|^src/c4or1k/images/' |
        while read -r f; do
            [ -f "$f" ] && grep -Iq . "$f" 2>/dev/null || continue
            mkdir -p "$ROOT/usr/$(dirname "$f")"
            cp "$f" "$ROOT/usr/$f"
        done
else
    echo "c4bb: not a git checkout, the root archive carries no sources" >&2
fi
[ -f $OUT/libc4ix32.c4l ] && cp $OUT/libc4ix32.c4l $ROOT/usr/lib/libc4ix.c4l
[ -f $OUT/libarc32.c4l ] && cp $OUT/libarc32.c4l $ROOT/usr/lib/libarc.c4l
cp src/c4ix/include/*.h src/c4ix/include/*.c libjs/guest/gui.h libjs/guest/gui.c libjs/guest/snd.h libjs/guest/snd.c $ROOT/usr/include/
# libc's headers, for -fno-builtin -I /usr/include/libc
mkdir -p $ROOT/usr/include/libc/sys
cp src/c4ix/libc/include/*.h $ROOT/usr/include/libc/
cp src/c4ix/libc/include/sys/*.h $ROOT/usr/include/libc/sys/
mkdir -p $ROOT/build/bin
cp -r $PB/. $ROOT/build/
# src/c4cc/BUILD.B4K's: built by c4cc above, as they are inside C4IX
for t in c4cc c4rdump c4rlink cpp c4 c4m; do cp $DISK/$t.c4r $ROOT/build/bin/; done
cp $OUT/c4m.pp.c $ROOT/build/c4m.pp.c
BUILT=$(cd $ROOT && {
    find build -name '*.c4o' -o -name '*.pp.c' | sort
    echo build/lib/libc4ix.c4l build/lib/libc4ix.c4s build/lib/libarc.c4l build/lib/libarc.c4s build/lib/libgui.c4s build/lib/libc.c4s
    find build/bin -type f | sort
    echo build/c4ix.c4r
})
tar --format=ustar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
    -C $ROOT -cf - usr --no-recursion $BUILT | gzip -9n > $DISK/c4ix-root.tgz
rm -rf $ROOT

say "the disk's manifest, sizes and index"
# manifest for the web app's disk loader (recursive: some entries,
# like src/c4ke/c4ke.c, are real subdirectories on purpose - see the
# innerbench comment above)
(cd $DISK && find . -type f -not -name manifest.json | sed 's|^\./||') | \
    awk 'BEGIN{printf "["} NR>1{printf ","} {printf "\"%s\"", $0} END{print "]"}' > $DISK/manifest.json

# LAZY LOADING (docs/c4ix-desktop.md, part three). c4ix.sizes is a
# "SIZE NAME" line per file: C4IX's vfsload reads it and makes each
# entry a lazy RAM file that the kernel reads from the host only when
# something first uses it. index.json is the same table for the web
# page, whose worker then fetches a file on first read instead of all
# of them before boot. Written last, after every copy above.
(cd $DISK && find . -type f -not -name manifest.json -not -name c4ix.sizes -not -name index.json -not -name c4ix.sizes.tmp \
    -printf '%s %P\n' | sort -k2) > $DISK/c4ix.sizes.tmp
mv $DISK/c4ix.sizes.tmp $DISK/c4ix.sizes
# index.json lists c4ix.sizes too -- a lazy disk serves only what its
# index names, and without the table vfsload falls back to copying all.
(cat $DISK/c4ix.sizes; echo "$(stat -c %s $DISK/c4ix.sizes) c4ix.sizes") | \
    awk 'BEGIN{printf "["} NR>1{printf ","} {n=$0; sub(/^[0-9]+ /,"",n); printf "[\"%s\",%s]", n, $1} END{print "]"}' \
    > $DISK/index.json
# and the manifest again, so the eager loader sees c4ix.sizes too
(cd $DISK && find . -type f -not -name manifest.json | sed 's|^\./||') | \
    awk 'BEGIN{printf "["} NR>1{printf ","} {printf "\"%s\"", $0} END{print "]"}' > $DISK/manifest.json

# ---- per-system disks -----------------------------------------------
# APPEND-ONLY SECTION. Everything above builds the ONE shared disk that
# test-c4bb.sh boots, and must not move. This derives two curated disks
# from it by copying named subsets -- no new compilation, so nothing
# here can change what the shared disk contains.
#
# Why two more disks at all: the shared disk is a demonstration that
# three operating systems can live on one, which is a fine thing to
# show and a bad thing to BUILD on. A recovery floppy that boots DOS
# and rebuilds the kernel should not carry C4IX's userland, and a C4KE
# root filesystem carrying its own compiler should not have to share a
# manifest with C4DOS's config.sys.
say "the derived disks: dos-recovery, climb, c4ke-root"
DOSDISK=$OUT/dos-recovery
ROOTDISK=$OUT/c4ke-root
rm -rf $DOSDISK $ROOTDISK
mkdir -p $DOSDISK $ROOTDISK

# --- the emergency recovery disk: DOS, a compiler, and the sources ---
# Boot with: node src/c4bb/sim/cli.js -i -d $DOSDISK $OUT/c4dos32.c4r
# then type BUILD, then RUN dosload.c4r c4ke.c4r
if [ -f $DISK/config.sys ]; then
    cp $DISK/config.sys $DISK/autoexec.bat $DOSDISK/
    for f in dostar.c4r cpp.c4r c4cc.c4r dosload.c4r c4ke-src.tar build.bat; do
        [ -f $DISK/$f ] && cp $DISK/$f $DOSDISK/
    done
    # c4sh is NOT built by BUILD.BAT and the init that is built spawns
    # it, so it has to be here or the kernel comes up with no shell.
    # init.c4r is deliberately absent: the one that boots must be the
    # one the machine just compiled, which is the whole point.
    cp $DISK/c4sh.c4r $DOSDISK/
    # The whole climb, when the pieces exist: LADDER builds C4KE from
    # source with cpp and c4cc, IX builds C4IX with the COMPILED c4lc
    # (docs/c4sc-design.md), and dosload boots either. c4sc.c4r and the
    # C4IX sources are what make the second half possible; without them
    # this stays the C4KE recovery disk it has always been.
    for f in ladder.bat ix.bat c4ix.objs tools-src.tar c4ix-src.tar \
             c4sc.c4r c4rlink.c4r init.c4r c4ke.vfs.c4r \
             c4ix-sh.c4r c4ix-ls.c4r c4ix-cat.c4r c4ix-ps.c4r; do
        [ -f ../../../c4dos-c4ix32/$f ] && cp ../../../c4dos-c4ix32/$f $DOSDISK/
        [ -f $OUT/../../../c4dos-c4ix32/$f ] && cp $OUT/../../../c4dos-c4ix32/$f $DOSDISK/
    done
    [ -f c4dos-c4ix32/config.sys ] && cp c4dos-c4ix32/config.sys $DOSDISK/config.sys
    (cd $DOSDISK && ls -p | grep -v '/$' | grep -v '^manifest.json$' > c4dos.dir)
    (cd $DOSDISK && find . -type f -not -name manifest.json | sed 's|^\./||') | \
        awk 'BEGIN{printf "["} NR>1{printf ","} {printf "\"%s\"", $0} END{print "]"}' > $DOSDISK/manifest.json
fi

# --- the C4KE root filesystem: userland plus the toolchain -----------
# Boot with: node src/c4bb/sim/cli.js -i -m 64 -d $ROOTDISK $OUT/c4ke32.c4r
# The manifest is the base one plus src/c4bb/fs/c4ke-dev.vfs.txt, which
# names the toolchain and C4IX's sources -- both were physically on the
# shared disk and in no manifest, so from inside C4KE they did not
# exist. Concatenated, not duplicated, so it cannot drift.
# Derived by SUBTRACTION, not by listing what to keep. The manifest
# names ~106 entries and every one of them has to be on the disk or the
# boot reports failures, so a hand-curated include list is a standing
# invitation to drift. Copy the shared disk and remove the things that
# belong to the other two systems instead -- what is left is by
# construction everything C4KE's manifest can ask for.
cp -r $DISK/. $ROOTDISK/
# --- the climb disk, for the browser ---------------------------------
# The web front-end fetches media out of images/, and the whole ladder
# lives on c4dos-c4ix32 at the repo root, which it cannot reach. So a
# copy, with the manifest the browser reads. It is the SAME disk the
# CLI climb test boots, deliberately: two ladders that differ by which
# front-end you used would be one ladder and one demo.
CLIMBDISK=$OUT/climb
rm -rf $CLIMBDISK
CLIMBSRC=
for root in ../../../c4dos-c4ix32 c4dos-c4ix32; do
    if [ -d $root ]; then
        mkdir -p $CLIMBDISK
        cp -r $root/. $CLIMBDISK/
        CLIMBSRC=$root
        break
    fi
done
if [ -d $CLIMBDISK ]; then
    # SAY SO IF WHAT WE JUST COPIED IS OLDER THAN ITS SOURCE.
    #
    # This block COPIES; it does not build. c4dos-c4ix32 is made by its
    # own Makefile target, and nothing here ever checked whether that
    # target had been run since the sources changed. So a person who
    # rebuilt "the images" got a fresh corpus and a two-day-old climb
    # disk, booted the BIOS, and found a C4DOS that had never heard of
    # the drive letters that landed yesterday -- with nothing anywhere
    # saying which half was stale. That is a whole afternoon.
    #
    # A warning rather than an error: rebuilding the climb disk is
    # expensive (it builds c4sc), this script has other reasons to run,
    # and a person who knows the ladder is stale is allowed to carry on.
    # Against the SOURCE image, never against the copy: cp gives the
    # copy this moment's timestamp, so comparing with it says "fresh"
    # for a disk built any time at all. (Written that way first, which
    # is why the note is here.)
    for src in src/c4dos/c4dos.c src/c4ke/c4ke.c src/c4ix/sched.c; do
        [ -f "$src" ] || continue
        [ -n "$CLIMBSRC" ] && [ -f "$CLIMBSRC/c4dos32.c4r" ] || continue
        if [ "$src" -nt "$CLIMBSRC/c4dos32.c4r" ]; then
            echo "c4bb: WARNING: the climb disk is older than $src." >&2
            echo "c4bb:          It is COPIED here, not built -- run 'make c4dos-c4ix32'" >&2
            echo "c4bb:          and then this script again, or the browser's BIOS boot" >&2
            echo "c4bb:          will keep running the old ladder." >&2
            break
        fi
    done
    rm -f $CLIMBDISK/manifest.json
    (cd $CLIMBDISK && find . -type f -not -name manifest.json | sed 's|^\./||') | \
        awk 'BEGIN{printf "["} NR>1{printf ","} {printf "\"%s\"", $0} END{print "]"}' > $CLIMBDISK/manifest.json
    echo "c4bb: browser climb disk: $CLIMBDISK ($(ls $CLIMBDISK | wc -l) entries)"
fi

# ---- the page's ladder disk: libjs/images/ladder ----------------------------
# What ?system=c4dos-ladder boots: C4DOS with the games, and everything the
# climb needs, so one page session goes C4DOS -> C4KE -> C4IX with no
# reboot (docs/c4dos-ladder.md). Three layers, each over the one before:
#   - the shared disk's C4IX userland and its tree (c4ix.vfs.txt), so the
#     C4IX the machine builds boots into what ?system=c4ix has, desktop
#     and games included;
#   - the climb floppy (c4dos-c4ix32): the build kit, BUILD and LADDER's
#     tools, the sources as archives, b4ke, c4ix.b4k, kboot, and the C4KE
#     tree (c4ke.vfs.txt) that floppy is made for;
#   - the page's C4DOS disk: the games and their data, grin, and the
#     newest C4DOS.
# The prebuilt C4IX kernel is left out: the one that boots is the one b4ke
# links. Copied from the climb floppy, as $CLIMBDISK is, so it is skipped
# (and says so) when that floppy has not been made.
LJLAD=libjs/images/ladder
rm -rf $LJLAD
if [ -n "$CLIMBSRC" ] && [ -f $LJDOS/c4dos32.c4r ]; then
    say "the page's ladder disk, C4DOS to C4KE to C4IX: $LJLAD"
    mkdir -p $LJLAD
    # Its directories, a file's by its name: GAMES\NAME (a game's files,
    # as on the page's C4DOS disk), GRIN and BIN (the same), BUILD (the
    # build kit: the batch files, the archives, the compilers and the tools
    # BUILD, LADDER and IX run, c4lc's Lisp, b4ke and what it reads), C4IX
    # (C4IX's userland and its tree, c4ix.vfs.txt), and C4KE (C4KE's: the
    # rest of the shared disk and the climb floppy, its programs and their
    # sources). src/ stays where it was: the nested c4m innerbench starts
    # opens src/c4ke/c4ke.c by that path.
    lad_dir () {
        case "$1" in
            # (the shared disk's tyrian/ is C4IX's: its hostdir(tyrian/))
            tyrian/*) echo C4IX ;;
            */*) g=$(game_of "$1"); if [ -n "$g" ]; then echo "GAMES/$g"; fi ;;
            config.sys|autoexec.bat|c4dos32.c4r|ladder.txt) ;;
            grin.c4r|clock.c4r|calc.c4r|notepad.c4r|exitkey.c4r) echo GRIN ;;
            askline.c4r|dosfilt.c4r|hello.c4r) echo BIN ;;
            c4ix-src.tar|c4ix.b4k|c4ix.objs|c4ix.lst) echo BUILD ;;
            c4ix-*|c4ix.*|c4ix_user.h) echo C4IX ;;
            *.bat|*.tar|*.lisp|*.b4k|*.lst|u0.h) echo BUILD ;;
            cpp.c4r|c4cc.c4r|c4sp.c4r|c4sc.c4r|c4rlink.c4r|c4rdump.c4r|c4th.c4r) echo BUILD ;;
            dostar.c4r|dosload.c4r|b4ke.c4r|kboot.c4r|tar.c4r|install.c4r|kinstall.c4r) echo BUILD ;;
            bbsave.c4r|save.c4r|reboot.c4r) echo BUILD ;;
            *) g=$(game_of "$1"); if [ -n "$g" ]; then echo "GAMES/$g"; else echo C4KE; fi ;;
        esac
    }
    lad_put () {            # lad_put DIR: DIR's files onto the ladder disk, each in its directory, over what is there
        (cd $1 && find . -type f ! -name manifest.json ! -name index.json ! -name c4dos.dir \
            ! -name config.sys ! -name autoexec.bat | sed 's|^\./||') | while read -r f; do
            # a page disk's own directories are the ladder's too
            case "$f" in
                GRIN/*|BIN/*|GAMES/*) to=$f ;;
                *) d=$(lad_dir "$f"); to=${d:+$d/}$f ;;
            esac
            lj_put "$1/$f" $LJLAD "$to"
        done
    }
    lad_put $DISK
    lad_put $CLIMBSRC
    lad_put $LJDOS
    rm -f $LJLAD/C4IX/c4ix.c4r $LJLAD/C4KE/boot.cfg
    cp $DISK/c4ix.vfs.txt $LJLAD/C4IX/c4ix.vfs.txt
    cp src/c4dos/fs/BUILD.BAT $LJLAD/BUILD/build.bat
    cp src/c4dos/fs/LADDER.TXT $LJLAD/ladder.txt
    # the climb floppy's RAM disk (32 MB: LADDER and the kernel's sources),
    # and the page's devices: HEAP.SYS gives each rung's memory back when
    # it returns to the one below, REDIR.SYS TAP is for grin's DOS Prompts.
    # No HOME=B:, as the page's C4DOS disk has: with a home drive, cpp and
    # c4cc would write the kernel to B: rather than the RAM disk C4KE is
    # seeded from.
    sed 's/SIZE=[0-9]*/SIZE=33554432/' src/c4dos/fs/CONFIG.SYS > $LJLAD/config.sys
    printf 'DEVICE=CONSOLE.SYS FLUSH\nDEVICE=DRIVES.SYS\nDEVICE=HEAP.SYS\nDEVICE=REDIR.SYS TAP\n' >> $LJLAD/config.sys
    list=$(echo $GAMES_ON_DISK | sed 's/ /, /g')
    {
        printf '@ECHO OFF\nECHO Welcome to C4DOS, the Build Ladder\nVER\n'
        printf 'PATH \\BUILD;\\GRIN;\\BIN\n'
        # C4KE and C4IX look for a file where they were started, as DOS
        # did before it had directories; APPEND says where else to look,
        # for them (the machine's DISK_DIRS) and for DOS
        printf 'REM C4KE and C4IX find their files, and the games, through APPEND\n'
        printf 'APPEND \\BUILD;\\C4KE;\\C4IX;%s\n' "$(games_dirs)"
        if [ -n "$list" ]; then printf 'ECHO The games, in \\GAMES: %s.\n' "$list"; fi
        # (C4DOS splits a line into at most 15 words, so ECHO says 14; and
        # no > in an ECHO: REDIR.SYS would take it for a redirection)
        printf 'ECHO GRIN starts the graphical shell.\n'
        printf 'ECHO Climb: BUILD, then RUN dosload.c4r c4ke.c4r boots the kernel it built.\n'
        printf 'ECHO TYPE LADDER.TXT says every step, up to C4IX.\n'
    } > $LJLAD/autoexec.bat
    listings $LJLAD
    disk_indexes $LJLAD
    echo "c4bb: ladder disk: $LJLAD ($(find $LJLAD -type f | wc -l) files, $(du -sh $LJLAD | cut -f1))"
else
    echo "c4bb: no ladder disk: it needs the climb floppy ('make c4dos-c4ix32') and $LJDOS" >&2
fi

rm -f $ROOTDISK/manifest.json $ROOTDISK/c4dos.dir
rm -f $ROOTDISK/config.sys $ROOTDISK/autoexec.bat $ROOTDISK/build.bat
rm -f $ROOTDISK/c4ke-src.tar $ROOTDISK/dostar.c4r $ROOTDISK/dosload.c4r
# C4IX's binaries go; C4IX's SOURCES stay, because building them is
# what this disk is for.
rm -f $ROOTDISK/c4ix.c4r $ROOTDISK/c4ix-*.c4r $ROOTDISK/c4ix.vfs.txt
rm -f $ROOTDISK/c4ix-*.c $ROOTDISK/c4ix.h $ROOTDISK/c4ix_user.h
# The build tools, so C4KE can build C4IX the way it is meant to:
# b4ke drives it, c4sc is the compiled c4lc, c4rlink joins the objects.
# docs/c4sc-design.md and src/c4ke/bin/c4ix.b4k.
$CC -o $ROOTDISK/b4ke.c4r $U0 $BIN/b4ke.c > /dev/null
# tar unpacks an archive into the ramfs (dostar's job, one rung up), and
# save writes the ramfs onto a drive, so what C4KE builds can outlive it.
# docs/climbing-the-ladder.md, docs/c4bb-storage.md.
$CC -o $ROOTDISK/tar.c4r $U0 $BIN/tar.c > /dev/null
$CC -o $ROOTDISK/save.c4r $U0 include/c4bb.h $BIN/save.c > /dev/null
cp c4ix-src.tar $ROOTDISK/ 2>/dev/null || true
[ -f c4sc32-fused.c4r ] && cp c4sc32-fused.c4r $ROOTDISK/c4sc.c4r
cp $BIN/c4ix.b4k $ROOTDISK/
cat src/c4bb/fs/c4ke.vfs.txt src/c4bb/fs/c4ke-dev.vfs.txt > $ROOTDISK/c4ke.vfs.txt
(cd $ROOTDISK && find . -type f -not -name manifest.json | sed 's|^\./||') | \
    awk 'BEGIN{printf "["} NR>1{printf ","} {printf "\"%s\"", $0} END{print "]"}' > $ROOTDISK/manifest.json

echo "c4bb: images built in $OUT"
echo "c4bb: derived disks: $DOSDISK ($(ls $DOSDISK | wc -l) files), $ROOTDISK ($(find $ROOTDISK -type f | wc -l) files)"
