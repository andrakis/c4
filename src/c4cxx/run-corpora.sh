#!/bin/bash
# run-corpora.sh [-E|-C]: test-same.sh over the corpora c4cxx is held to:
# c4's src/tests, chocolate-blood's C units (helios ~/gate/c4cxx), and Duke
# Nukem 3D's units (~/git/c4/games/duke3d). From the c4 tree's root.
mode=${1:--C}
R=$(cd "$(dirname "$0")/../.." && pwd)
cd $R
T=src/c4cxx/test-same.sh
st=0
# c4lc_lex_sample.c: c4lc turns a backslash-newline inside a string into a
# newline character (a c4cc quirk); C removes it, as c4cxx does
$T $mode -I src/tests -I src/c4lm/include -I src/c4lm -I src/c4ke/include -I src/c4ix/libc/include -- \
	$(ls src/tests/*.c | grep -v c4lc_lex_sample) || st=1
if [ -d ~/gate/c4cxx/m0/c ]; then
	S=~/gate/c4cxx/shim B=~/gate/c4cxx/b0/source
	$T $mode -D __C4__ -D C4BLOOD -D C4BLOOD_C4 -I $S/c4inc -I $S/inc -I ~/git/c4/games/duke3d/c4/inc \
		-I src/c4ix/libc/include -I src/c4ix/include -I libjs/guest -I $B/build -I $B/mact -I $B/audiolib \
		-I $B/blood -I $B/blood/qtools/include -- $(ls ~/gate/c4cxx/m0/c/w_*.c | grep -v a_asm) || st=1
fi
D=${DUKE3D_DIR:-$R/games/duke3d}; [ -d $D/Engine ] || D=~/git/c4/games/duke3d
if [ -d $D/Engine ]; then
	U=""
	for e in cache draw dummy_multi engine filesystem fixedPoint_math network tiles; do U="$U $D/Engine/src/$e.c"; done
	for g in actors animlib config console control cvar_defs cvars game gamedef global keyboard menues player premap \
		rts scriplib sector sounds; do U="$U $D/Game/src/$g.c"; done
	for a in midi al_midi gmtimbre ll_man; do U="$U $D/Game/src/audiolib/$a.c"; done
	for c in duke-display duke-sum duke-gui duke-snd; do U="$U $D/c4/$c.c"; done
	$T $mode -D C4DUKE -D PLATFORM_UNIX -I $D/c4/inc -I src/c4ix/libc/include -I src/c4ix/include -I libjs/guest \
		-I $D/Engine/src -I $D/Game/src -I $D/c4 -- $U || st=1
fi
exit $st
