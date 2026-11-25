#!/usr/bin/env bash
# test-cpp.sh - the preprocessor's pin (see docs/c4dos-design.md).
#
# Two properties:
#   1. IMAGE PARITY: for every corpus entry the shipping build pipes
#      through gcc -E, `cpp | c4cc` and `gcc -E | c4cc` produce
#      byte-identical .c4r images. Both compiles run under `setarch -R`
#      because c4cc leaks an ASLR'd heap address into the image (found
#      while building this pin; deterministic with ASLR off).
#   2. THE TOWER: plain c4 interpreting cpp.c produces byte-identical
#      output to the native gcc build of the same file - the strict-c4
#      dialect claim, checked rather than asserted.
#
# Run from the repo root: bash src/c4dos/tests/test-cpp.sh

set -e
cd "$(dirname "$0")/../../.."

CPP=./cpp
[ -x "$CPP" ] || { echo "test-cpp: run 'make cpp' first"; exit 2; }
[ -x ./c4cc ] || { echo "test-cpp: run 'make c4cc' first"; exit 2; }
[ -x ./c4 ]   || { echo "test-cpp: run 'make c4' first"; exit 2; }

# gcc predefines __GNUC__; the tree's guards (__attribute__, unistd.h)
# are written for it and the shipping images bake that branch in, so
# the cpp side must say it too.
GD="-DC4CC=1 -D__c4__=1 -D__C4CC__=1 -D__c4cc__=1"
CD="$GD -D__GNUC__=1"
T=.cpp_test_tmp
mkdir -p $T
trap 'rm -rf $T' EXIT

pin () { # name extra-includes files...
  n=$1; inc=$2; shift 2
  gcc -E -Iinclude -I. $inc $GD -C "$@" 2>/dev/null > $T/g-$n.i
  setarch -R ./c4cc -o $T/g-$n.c4r $T/g-$n.i > /dev/null 2>&1
  $CPP -Iinclude -I. $inc $CD "$@" > $T/c-$n.i
  setarch -R ./c4cc -o $T/c-$n.c4r $T/c-$n.i > /dev/null 2>&1
  cmp -s $T/g-$n.c4r $T/c-$n.c4r || { echo "test-cpp: IMAGE MISMATCH: $n"; exit 1; }
  echo "test-cpp: image parity: $n"
}

# the corpus: everything build-images.sh feeds c4cc through $PREPROC
pin vprintf ""            include/u0.h src/tests/test_vprintf.c
pin float   "-Isrc/tests" include/u0.h src/tests/test_float.c
pin c4m     ""            c4m.c

# the tower: plain c4 interpreting cpp.c, output byte-identical
# (c4 appends its own "exit(N) cycle = M" line - strip it)
$CPP -Iinclude -I. $CD include/u0.h src/tests/test_vprintf.c > $T/native.i
# cpp now calls the C4DOS API for -o, and plain c4 takes exactly ONE
# source file -- so the tower gets the header concatenated in, the same
# way c4cc is handed it as a source. gcc reaches the stubs through
# c4dos_native.h instead; neither build can see the other's copy.
cat include/c4dos.h src/c4dos/cpp.c > $T/cpp_tower.c
./c4 $T/cpp_tower.c -Iinclude -I. $CD include/u0.h src/tests/test_vprintf.c \
  | grep -v '^exit([0-9-]*) cycle = ' > $T/interp.i
cmp -s $T/native.i $T/interp.i || { echo "test-cpp: TOWER MISMATCH (plain c4 vs native)"; exit 1; }
echo "test-cpp: the tower holds (plain c4 output identical)"

echo "test-cpp: OK"
