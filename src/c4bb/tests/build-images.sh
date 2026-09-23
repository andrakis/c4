#!/bin/bash
# build-images.sh - build the 32-bit .c4r corpus for c4bb testing.
# Run from the repo root; expects c4cc32 to exist (make c4bb-32bit).
set -e

CC=./c4cc32
OUT=src/c4bb/images
U0=include/u0.h
PREPROC="gcc -E -Iinclude -I. -DC4CC=1 -D__c4__=1 -D__C4CC__=1 -D__c4cc__=1 -C"
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
fwbuild fw-hello  -D FW_STAGE=0
fwbuild fw-ram    -D FW_STAGE=1 -D FW_RAM=1
fwbuild fw-drives -D FW_STAGE=2 -D FW_RAM=1 -D FW_DRIVES=1 -D FW_SEEONLY=1
fwbuild fw        -D FW_STAGE=3 -D FW_RAM=1 -D FW_DRIVES=1 -D FW_BOOT=1

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

# c4bb's own trap-machinery tests (src/tests/test_customop predates
# TLEV/DBG and uses colliding opcode numbers, so it cannot be used)
for t in bb_customop bb_preempt bb_pm bb_pit bb_whowrote; do
    $CC -o $OUT/$t.c4r src/c4bb/tests/src/$t.c > /dev/null
done
# the mailbox probe: board-only header passed as a source (c4cc has no #include)
$CC -o $OUT/bb_mbox.c4r include/c4bb_mbox.h src/c4bb/tests/src/bb_mbox.c > /dev/null
# libjs's display demo (docs/libjs-design.md): prints "not fitted" on any
# machine without the display, so it is safe in every corpus.
$CC -o $OUT/gui-demo.c4r libjs/guest/gui.h libjs/guest/gui-demo.c > /dev/null

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
    $PREPROC src/c4ke/c4ke.c > .c4bb_klc.c
    ./c4sp32 -c 64000000 src/c4sp/lisp/c4lc.lisp -O .c4bb_klc.c $OUT/c4ke32.c4r > /dev/null
    rm -f .c4bb_klc.c
fi

# userland (c4cc32, same recipes as the Makefile's bin rules)
BIN=src/c4ke/bin
$CC -o $DISK/init.c4r  $U0 $BIN/ps.c $BIN/eshell.c src/c4ke/services/init.c > /dev/null
$CC -o $DISK/c4sh.c4r  $U0 $BIN/ps.c src/c4sh/c4sh.c src/c4sh/c4sh_builtins.c src/c4sh/c4sh_scripting.c > /dev/null
$CC -o $DISK/eshell.c4r $U0 $BIN/ps.c $BIN/eshell.c > /dev/null
$CC -o $DISK/c4ke.vfs.c4r $U0 src/c4ke/include/service.h src/c4ke/services/c4ke.vfs.c > /dev/null
for t in ls ps cat echo kill spin c4le type xxd badop; do
    $CC -o $DISK/$t.c4r $U0 $BIN/$t.c > /dev/null
done
$CC -o $DISK/top.c4r $U0 $BIN/ps.c $BIN/top.c
# the mailbox from a task: bound to the host, and task to task
for t in mbecho mbpair; do
    $CC -o $DISK/$t.c4r $U0 include/c4ke_mbox.h $BIN/$t.c > /dev/null
done > /dev/null

# vfsload: built with c4lc (needs real block scoping, not just c4cc's
# top-of-function declarations). c4lc's own preprocessor hangs on
# u0.h (a pre-existing issue, unrelated to word size - reproduces at
# 64-bit too), so preprocess with gcc -E first, same as the kernel.
$PREPROC $BIN/vfsload.c > .c4bb_vfsload_pp.c
./c4sp32 -c 16000000 src/c4sp/lisp/c4lc.lisp -O .c4bb_vfsload_pp.c $DISK/vfsload.c4r > /dev/null
rm -f .c4bb_vfsload_pp.c

# raycast: also c4lc, also no u0, and additionally -conforming so it
# can write ANSI escapes as "\033[" instead of poking 27 in as an
# integer. No gcc -E here -- it includes nothing, so c4lc's own
# preprocessor handles the one -D and the #ifdef it guards.
./c4sp32 -c 16000000 src/c4sp/lisp/c4lc.lisp -O -conforming -D RC_KE=1 \
    src/tests/raycast.c $DISK/raycast.c4r > /dev/null

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
rm -f .c4bb_c4m_pp.c
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
C4IX_USER="hello uhello echo wc cat sh ps bench cycles ls mkdir top spin fmt gui"
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
if [ ! -f $OUT/c4ix32.c4r ] || [ src/c4ix/sched.c -nt $OUT/c4ix32.c4r ] || \
   [ src/c4ix/c4ix.h -nt $OUT/c4ix32.c4r ] || [ src/c4ix/init.c -nt $OUT/c4ix32.c4r ] || \
   [ src/c4ix/loader.c -nt $OUT/c4ix32.c4r ]; then
    objs=""
    for m in $C4IX_MODS; do
        ./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O -c -I src/c4ix src/c4ix/$m.c .c4bb_ix_$m.c4o > /dev/null
        objs="$objs .c4bb_ix_$m.c4o"
    done
    ./c4rlink32 $objs -o $OUT/c4ix32.c4r
    ./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O -c -I src/c4ix/include src/c4ix/lib/libc4ix.c .c4bb_ix_lib.c4o > /dev/null
    ./c4rlink32 -r .c4bb_ix_lib.c4o -o .c4bb_libc4ix32.c4l
    for u in $C4IX_USER; do
        ./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O -c -I src/c4ix/include src/c4ix/user/$u.c .c4bb_ix_u.c4o > /dev/null
        ./c4rlink32 .c4bb_ix_u.c4o .c4bb_libc4ix32.c4l -o $DISK/c4ix-$u.c4r > /dev/null
    done
    # the VFS loader: c4ix-vfsload.c4r, NOT plain vfsload.c4r - C4KE
    # has its own boot-time loader of the same name on this shared
    # disk (src/c4ke/bin/vfsload.c), and the two would otherwise
    # silently overwrite each other (they did, until this was caught:
    # C4IX's build ran second, so C4KE started running C4IX's loader
    # under itself - "c4ix-" keeps it consistent with every other
    # C4IX binary here, all of which face the same shared-disk risk).
    ./c4sp32 -c $C4IX_CELLS src/c4sp/lisp/c4lc.lisp -O -c -I src/c4ix/include src/c4ix/user/vfsload.c .c4bb_ix_u.c4o > /dev/null
    ./c4rlink32 .c4bb_ix_u.c4o .c4bb_libc4ix32.c4l -o $DISK/c4ix-vfsload.c4r > /dev/null
    rm -f .c4bb_ix_*.c4o .c4bb_ix_*.pp.c .c4bb_libc4ix32.c4l
fi

# the C4IX VFS manifest and the plain-text sources it references -
# c4ix.vfs.txt is curated SOURCE (see src/c4bb/fs/), copied in like
# c4ke.vfs.txt above, not regenerated with the rest of images/.
# Sources are named c4ix-NAME.c, the same prefix the binaries already
# use, because C4KE and C4IX share this one flat disk and both have a
# "cat.c"/"ps.c"/"top.c" etc of their own - without the prefix the
# two systems' vfsload runs would silently overwrite each other's
# source copies.
cp src/c4bb/fs/c4ix.vfs.txt $DISK/
for n in $C4IX_USER; do
    cp src/c4ix/user/$n.c $DISK/c4ix-$n.c
done
cp src/c4ix/c4ix.h $DISK/c4ix.h
cp src/c4ix/include/c4ix_user.h $DISK/c4ix_user.h

# ---- the ladder ------------------------------------------------------
# HOMEWARD's premise is that you climb this disk: C4DOS boots, C4DOS
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
# docs/homeward-ladder.md tracks the whole climb.
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

# manifest for the web app's disk loader (recursive: some entries,
# like src/c4ke/c4ke.c, are real subdirectories on purpose - see the
# innerbench comment above)
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
