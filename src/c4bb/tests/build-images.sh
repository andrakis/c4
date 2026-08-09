#!/bin/bash
# build-images.sh - build the 32-bit .c4r corpus for c4bb testing.
# Run from the repo root; expects c4cc32 to exist (make c4bb-32bit).
set -e

CC=./c4cc32
OUT=src/c4bb/images
U0=include/u0.h
PREPROC="gcc -E -Iinclude -I. -DC4CC=1 -D__c4__=1 -D__C4CC__=1 -D__c4cc__=1 -C"
mkdir -p $OUT

# firmware: built with c4lc (the nicer compiler; real preprocessor).
# c4lc emits for its host's word size, so the 32-bit c4sp gives 32-bit
# images. The corpus below builds with c4cc32 so the parity suite
# exercises both compilers.
./c4sp32 -c 64000000 src/c4sp/lisp/c4lc.lisp -O src/c4bb/fw/fw.c src/c4bb/fw/fw.c4r > /dev/null

# hello links without u0 (Makefile:860)
$CC -o $OUT/hello32.c4r src/tests/hello.c > /dev/null

# standard u0-linked tests
for t in factorial test_basic test_malloc test_static tests multifun \
         test-order test-ptrs test_continue mandel test_args test_exit \
         cycles test_printloop test_printf; do
    $CC -o $OUT/$t.c4r $U0 src/tests/$t.c > /dev/null
done

# c4bb's own trap-machinery tests (src/tests/test_customop predates
# TLEV/DBG and uses colliding opcode numbers, so it cannot be used)
for t in bb_customop bb_preempt bb_pm; do
    $CC -o $OUT/$t.c4r src/c4bb/tests/src/$t.c > /dev/null
done

# these include real headers, so they go through the preprocessor
$PREPROC $U0 src/tests/test_vprintf.c 2>/dev/null | $CC -o $OUT/test_vprintf.c4r - > /dev/null
$PREPROC -Isrc/tests $U0 src/tests/test_float.c 2>/dev/null | $CC -o $OUT/test_float.c4r - > /dev/null

# ---- C4KE and its userland ------------------------------------------
# kernel: preprocess + c4lc under the 32-bit c4sp (like the Makefile's
# c4ke-lc.c4r rule, at 32 bits). Skipped when already newer.
DISK=$OUT/disk
mkdir -p $DISK
if [ ! -f $OUT/c4ke32.c4r ] || [ src/c4ke/c4ke.c -nt $OUT/c4ke32.c4r ]; then
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
for t in ls ps cat echo kill spin c4le type xxd; do
    $CC -o $DISK/$t.c4r $U0 $BIN/$t.c > /dev/null
done
$CC -o $DISK/top.c4r $U0 $BIN/ps.c $BIN/top.c > /dev/null

# vfsload: built with c4lc (needs real block scoping, not just c4cc's
# top-of-function declarations). c4lc's own preprocessor hangs on
# u0.h (a pre-existing issue, unrelated to word size - reproduces at
# 64-bit too), so preprocess with gcc -E first, same as the kernel.
$PREPROC $BIN/vfsload.c > .c4bb_vfsload_pp.c
./c4sp32 -c 16000000 src/c4sp/lisp/c4lc.lisp -O .c4bb_vfsload_pp.c $DISK/vfsload.c4r > /dev/null
rm -f .c4bb_vfsload_pp.c

# the self-hosted core (c4, c4m, c4cc) and the C4R toolchain, same
# recipes as the Makefile's native (64-bit) rules, just via c4cc32
$CC -o $DISK/c4.c4r $U0 c4.c > /dev/null
$PREPROC c4m.c 2>/dev/null | $CC -o $DISK/c4m.c4r - > /dev/null
$CC -o $DISK/c4cc.c4r $U0 load-c4r.c src/c4cc/c4cc.c src/c4cc/asm-c4r.c > /dev/null
$CC -o $DISK/c4rdump.c4r $U0 load-c4r.c src/c4cc/c4cc.c src/c4cc/asm-c4r.c $BIN/c4rdump.c > /dev/null
$CC -o $DISK/c4rlink.c4r $U0 load-c4r.c src/c4cc/c4cc.c src/c4cc/asm-c4r.c $BIN/c4rlink.c > /dev/null

# benchmarks
for t in bench benchtop innerbench; do
    $CC -o $DISK/$t.c4r $U0 src/bench/$t.c > /dev/null
done

cp $OUT/hello32.c4r $DISK/hello.c4r
for t in tests factorial multifun test-order test-ptrs test_continue \
         test_args test_exit test_printloop test_basic test_malloc \
         test_float mandel; do
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
              test_basic test_malloc test_float mandel"
for n in $BIN_TEST_SRC; do
    cp src/tests/$n.c $DISK/$n.c 2>/dev/null || true
done
cp include/c4.h include/c4ke.h include/c4m.h $DISK/
cp src/c4ke/include/service.h $DISK/

# ---- C4IX and its userland ------------------------------------------
# modules compiled by 32-bit c4lc (its own preprocessor), linked by
# the 32-bit c4rlink; userland links against libc4ix
C4IX_MODS="boot console va host sl4b task sched vfs sys c4ke loader init"
C4IX_USER="hello uhello echo wc cat sh ps bench cycles ls mkdir top spin fmt"
if [ ! -f $OUT/c4ix32.c4r ] || [ src/c4ix/sched.c -nt $OUT/c4ix32.c4r ] || \
   [ src/c4ix/c4ix.h -nt $OUT/c4ix32.c4r ] || [ src/c4ix/init.c -nt $OUT/c4ix32.c4r ]; then
    objs=""
    for m in $C4IX_MODS; do
        ./c4sp32 -c 8000000 src/c4sp/lisp/c4lc.lisp -O -c -I src/c4ix src/c4ix/$m.c .c4bb_ix_$m.c4o > /dev/null
        objs="$objs .c4bb_ix_$m.c4o"
    done
    ./c4rlink32 $objs -o $OUT/c4ix32.c4r
    ./c4sp32 -c 8000000 src/c4sp/lisp/c4lc.lisp -O -c -I src/c4ix/include src/c4ix/lib/libc4ix.c .c4bb_ix_lib.c4o > /dev/null
    ./c4rlink32 -r .c4bb_ix_lib.c4o -o .c4bb_libc4ix32.c4l
    for u in $C4IX_USER; do
        ./c4sp32 -c 8000000 src/c4sp/lisp/c4lc.lisp -O -c -I src/c4ix/include src/c4ix/user/$u.c .c4bb_ix_u.c4o > /dev/null
        ./c4rlink32 .c4bb_ix_u.c4o .c4bb_libc4ix32.c4l -o $DISK/c4ix-$u.c4r > /dev/null
    done
    # the VFS loader: c4ix-vfsload.c4r, NOT plain vfsload.c4r - C4KE
    # has its own boot-time loader of the same name on this shared
    # disk (src/c4ke/bin/vfsload.c), and the two would otherwise
    # silently overwrite each other (they did, until this was caught:
    # C4IX's build ran second, so C4KE started running C4IX's loader
    # under itself - "c4ix-" keeps it consistent with every other
    # C4IX binary here, all of which face the same shared-disk risk).
    ./c4sp32 -c 8000000 src/c4sp/lisp/c4lc.lisp -O -c -I src/c4ix/include src/c4ix/user/vfsload.c .c4bb_ix_u.c4o > /dev/null
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

# manifest for the web app's disk loader (recursive: some entries,
# like src/c4ke/c4ke.c, are real subdirectories on purpose - see the
# innerbench comment above)
(cd $DISK && find . -type f -not -name manifest.json | sed 's|^\./||') | \
    awk 'BEGIN{printf "["} NR>1{printf ","} {printf "\"%s\"", $0} END{print "]"}' > $DISK/manifest.json

echo "c4bb: images built in $OUT"
