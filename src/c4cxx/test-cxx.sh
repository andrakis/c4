#!/bin/bash
# test-cxx.sh [file.cpp ...]: c4cxx's C++ programs (default
# src/c4cxx/tests/t_*.cpp), each run three ways, which must print the same:
#   1. g++ -m32 (no exceptions, RTTI or C++ library): the reference;
#   2. c4cxx to C, gcc -m32;
#   3. c4cxx to C, c4lc (C4CXX_LC, default ./c4sc-host32), on c4m32;
# and gcc -m32 confirms the types c4cxx gave the C it wrote (-check-types,
# as test-types.sh).
# From the c4 tree's root, with c4cxx, c4sc-host32 and c4m32 built.
LC=${C4CXX_LC:-./c4sc-host32}
T=$(mktemp -d)
trap 'rm -rf $T' EXIT
files=${*:-src/c4cxx/tests/t_*.cpp}
n=0 bad=0
for f in $files; do
	b=$(basename $f .cpp)
	n=$((n + 1))
	fail () { echo "FAIL $b: $1"; bad=$((bad + 1)); }
	# linked by gcc: there is no 32-bit libstdc++ here, and no need of one
	if ! { g++ -m32 -std=gnu++98 -fno-exceptions -fno-rtti -nostdinc++ -w -c -o $T/$b.o $f &&
	       gcc -m32 -o $T/$b.ref $T/$b.o; } 2> $T/$b.gxx; then
		fail "g++ rejects it: $(head -1 $T/$b.gxx)"; continue
	fi
	$T/$b.ref > $T/$b.ref.out 2>&1
	if ! ./c4cxx -o $T/$b.c $f 2> $T/$b.err; then
		fail "c4cxx: $(head -1 $T/$b.err)"; continue
	fi
	if ! gcc -m32 -std=gnu99 -w -o $T/$b.gcc $T/$b.c 2> $T/$b.gccerr; then
		fail "gcc rejects c4cxx's C: $(grep -m1 error $T/$b.gccerr)"; continue
	fi
	$T/$b.gcc > $T/$b.gcc.out 2>&1
	if ! cmp -s $T/$b.ref.out $T/$b.gcc.out; then
		fail "c4cxx+gcc prints otherwise than g++:"; diff $T/$b.ref.out $T/$b.gcc.out | head -10; continue
	fi
	if ! { ./c4cxx -check-types -o $T/$b.t.c $f &&
	       gcc -m32 -std=gnu11 -fsyntax-only -fpermissive -w -fno-builtin $T/$b.t.c; } > $T/$b.terr 2>&1; then
		fail "gcc does not confirm c4cxx's types: $(grep -m1 error $T/$b.terr)"; continue
	fi
	if ! $LC compile -P $T/$b.c $T/$b.c4r > $T/$b.lc 2>&1; then
		fail "c4lc rejects c4cxx's C: $(grep -v '^;;' $T/$b.lc | head -1)"; continue
	fi
	./c4m32 load-c4r.c -- $T/$b.c4r > $T/$b.c4.out 2>&1
	if ! cmp -s $T/$b.ref.out $T/$b.c4.out; then
		fail "c4cxx+c4lc prints otherwise than g++:"; diff $T/$b.ref.out $T/$b.c4.out | head -10; continue
	fi
done
echo "test-cxx.sh: $((n - bad)) of $n programs print as g++'s do (gcc -m32 and c4lc)"
# what c4cxx refuses (e_*.cpp), with the message its first line expects
if [ -z "$*" ]; then
	m=0 mbad=0
	for f in src/c4cxx/tests/e_*.cpp; do
		m=$((m + 1))
		want=$(head -1 $f | sed 's|^// expect: ||')
		if ./c4cxx -o $T/e.c $f 2> $T/e.err; then
			echo "FAIL $(basename $f): c4cxx took it"; mbad=$((mbad + 1))
		elif ! grep -qF "$want" $T/e.err; then
			echo "FAIL $(basename $f): not the message expected: $(head -1 $T/e.err)"; mbad=$((mbad + 1))
		fi
	done
	echo "test-cxx.sh: $((m - mbad)) of $m refused as they should be"
	bad=$((bad + mbad))
fi
[ $bad = 0 ]
