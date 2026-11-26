#!/bin/bash
# test-cxx.sh [program ...]: c4cxx's C++ programs, each run three ways,
# which must print the same:
#   1. g++ -m32 (no exceptions, RTTI or C++ library): the reference;
#   2. c4cxx to C, gcc -m32;
#   3. c4cxx to C, c4lc (C4CXX_LC, default ./c4sc-host32), linked by
#      ./c4rlink32, on c4m32;
# and gcc -m32 confirms the types c4cxx gave the C it wrote (-check-types,
# as test-types.sh). A program is src/c4cxx/tests/t_*.cpp, or a directory
# src/c4cxx/tests/m_*/ of units linked in their names' order (which the
# order their globals are built in follows). The runtime is linked in each
# way: tests/rt-gxx.cpp for g++ (new, delete), rt/libc4cxx.c for c4cxx's C.
# A src/c4cxx/tests/w_*.cpp has Watcom's layouts, as Blood's host build:
# -fshort-enums -mms-bitfields -funsigned-char for g++, c4cxx and gcc, and
# no c4lc run (c4lc has -mms-bitfields only, so far). A g_*.cpp uses gcc's
# builtins, as glibc's headers do, and has no c4lc run either.
# From the c4 tree's root, with c4cxx, c4sc-host32, c4m32 and c4rlink32 built.
LC=${C4CXX_LC:-./c4sc-host32}
R=src/c4cxx
T=$(mktemp -d)
trap 'rm -rf $T' EXIT
progs=${*:-$(ls -d $R/tests/t_*.cpp $R/tests/w_*.cpp $R/tests/g_*.cpp $R/tests/m_*/)}
n=0 bad=0
mkdir $T/rt
if ! { g++ -m32 -std=gnu++98 -fno-exceptions -fno-rtti -nostdinc++ -w -c -o $T/rt/rt-gxx.o $R/tests/rt-gxx.cpp &&
       gcc -m32 -std=gnu99 -w -c -o $T/rt/libc4cxx.o $R/rt/libc4cxx.c &&
       $LC compile -P -c $R/rt/libc4cxx.c $T/rt/libc4cxx.c4o > $T/rt/lc 2>&1; }; then
	echo "test-cxx.sh: the runtime does not build"; cat $T/rt/lc; exit 1
fi
for p in $progs; do
	b=$(basename $p .cpp)
	if [ -d $p ]; then units=$(ls $p/*.cpp); else units=$p; fi
	n=$((n + 1))
	fail () { echo "FAIL $b: $1"; bad=$((bad + 1)); }
	mkdir $T/$b
	ok=1 gxx= cs= c4s=
	case $b in w_*) xf="-fshort-enums -mms-bitfields -funsigned-char" lc= ;; g_*) xf= lc= ;; *) xf= lc=1 ;; esac
	for u in $units; do
		v=$(basename $u .cpp)
		# linked by gcc: there is no 32-bit libstdc++ here, and no need of one
		if ! g++ -m32 -std=gnu++98 -fno-exceptions -fno-rtti -nostdinc++ -w $xf -c -o $T/$b/$v.o $u 2> $T/$b/$v.gxx; then
			fail "g++ rejects $v: $(head -1 $T/$b/$v.gxx)"; ok=0; break
		fi
		if ! ./c4cxx $xf -o $T/$b/$v.c $u 2> $T/$b/$v.err; then
			fail "c4cxx: $(head -1 $T/$b/$v.err)"; ok=0; break
		fi
		if ! { ./c4cxx $xf -check-types -o $T/$b/$v.t.c $u &&
		       gcc -m32 -std=gnu11 -fsyntax-only -fpermissive -w -fno-builtin $xf $T/$b/$v.t.c; } > $T/$b/$v.terr 2>&1; then
			fail "gcc does not confirm c4cxx's types in $v: $(grep -m1 error $T/$b/$v.terr)"; ok=0; break
		fi
		if [ -n "$lc" ] && ! $LC compile -P -c $T/$b/$v.c $T/$b/$v.c4o > $T/$b/$v.lc 2>&1; then
			fail "c4lc rejects c4cxx's C of $v: $(grep -v '^;;' $T/$b/$v.lc | head -1)"; ok=0; break
		fi
		gxx="$gxx $T/$b/$v.o" cs="$cs $T/$b/$v.c" c4s="$c4s $T/$b/$v.c4o"
	done
	[ $ok = 1 ] || continue
	if ! gcc -m32 -o $T/$b/ref $gxx $T/rt/rt-gxx.o 2> $T/$b/ld; then
		fail "g++'s objects do not link: $(head -1 $T/$b/ld)"; continue
	fi
	$T/$b/ref > $T/$b/ref.out 2>&1
	if ! gcc -m32 -std=gnu99 -w $xf -o $T/$b/gcc $cs $T/rt/libc4cxx.o 2> $T/$b/gccerr; then
		fail "gcc rejects c4cxx's C: $(grep -m1 error $T/$b/gccerr)"; continue
	fi
	$T/$b/gcc > $T/$b/gcc.out 2>&1
	if ! cmp -s $T/$b/ref.out $T/$b/gcc.out; then
		fail "c4cxx+gcc prints otherwise than g++:"; diff $T/$b/ref.out $T/$b/gcc.out | head -10; continue
	fi
	[ -n "$lc" ] || continue
	if ! ./c4rlink32 $c4s $T/rt/libc4cxx.c4o -o $T/$b/prog.c4r > $T/$b/link 2>&1; then
		fail "c4rlink: $(head -1 $T/$b/link)"; continue
	fi
	./c4m32 load-c4r.c -- $T/$b/prog.c4r > $T/$b/c4.out 2>&1
	if ! cmp -s $T/$b/ref.out $T/$b/c4.out; then
		fail "c4cxx+c4lc prints otherwise than g++:"; diff $T/$b/ref.out $T/$b/c4.out | head -10; continue
	fi
done
echo "test-cxx.sh: $((n - bad)) of $n programs print as g++'s do (gcc -m32 and c4lc)"
# what c4cxx refuses (e_*.cpp), with the message its first line expects
if [ -z "$*" ]; then
	m=0 mbad=0
	for f in $R/tests/e_*.cpp; do
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
