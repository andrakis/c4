#!/bin/bash
# blood-oracle.sh [FRAMES]: M6's oracle (docs/c4cxx-plan.md 4.10). Blood
# built from c4cxx's C by gcc -m32 must play the shareware's demos as
# c4-Blood's host build (B0, g++) does, frame for frame.
#
#   1. B0: c4-Blood's c4/host.sh, into $W/ref (blood-host), by
#      blood-b0.sh: globals in the source's order, Watcom's rand.
#   2. Every C++ unit of Blood, and c4/netnone.cpp, through c4cxx against
#      the host's headers (glibc's and gcc's, after c4-Blood's own), with
#      what g++ gets from host.sh: Watcom's layouts (-fshort-enums
#      -mms-bitfields -funsigned-char) and gcc's predefined __GNUC__ and
#      __i386__, so that the headers take the paths they take for g++. Then
#      gcc -m32 with host.sh's flags and B0's -fno-toplevel-reorder,
#      -fno-common as C++ has it.
#   3. Linked as B0 is: loadsave first, host.sh's C objects, B0's rand, and
#      rt/libc4cxx.c for c4/cxxrt.cpp: $W/blood-cxx.
#   4. Both run FRAMES frames (default 4000, the three demos) in $BLOOD_RUN,
#      a directory of the game's files with BLOOD.CFG (c4-Blood's README:
#      ScreenMode = 1, FXDevice = 13, MusicDevice = 13), every frame's
#      checksum printed (-sum 1); the two lists must be the same.
#   5. With gdb, the size of every struct and class c4cxx wrote, in both
#      programs' debug information: they must be the same.
#
# From the c4 tree's root, c4cxx built. BLOOD_DIR is c4-Blood (default
# ~/git/games/c4-blood), BLOOD_VER the version (BS111A), W the work
# directory (default /tmp/blood-oracle; kept), C4CXX_FLAGS more of
# c4cxx's options (-fargs-rtl, as blood-c4lc.sh has it).
cd "$(dirname "$0")/../.." || exit 1
FRAMES=${1:-4000}
G=${BLOOD_DIR:-$HOME/git/games/c4-blood}
B=$G/source C=$G/c4
VER=${BLOOD_VER:-BS111A}
case $VER in
	BS110|BSR110|BR110|BP110|BS111|BSR111|BR111|BP111) QT=QT110 ;;
	*) QT=QT111A ;;
esac
W=${W:-/tmp/blood-oracle}
RUN=${BLOOD_RUN:?"BLOOD_RUN: a directory of the game's files and BLOOD.CFG"}
H=$W/cxx
mkdir -p $H
GCC_INC=$(gcc -m32 -print-file-name=include)
SYS="-I /usr/include/$(gcc -print-multiarch) -I /usr/include -I $GCC_INC"
INC="-I $C/inc -I $B/blood -I $B/blood/qtools/include -I $B/build -I $B/mact -I $B/audiolib"
WF="-fshort-enums -mms-bitfields -funsigned-char"
GNU="-D __GNUC__=$(gcc -dumpversion | cut -d. -f1) -D __GNUC_MINOR__=0 -D __GNUC_PATCHLEVEL__=0 -D __i386__=1 -D __linux__=1"
GCC="gcc -m32 -O2 -g -std=gnu99 -fno-strict-aliasing -fwrapv -msse2 -mfpmath=sse -ffp-contract=off -fno-toplevel-reorder -fno-common -fpermissive -w $WF"
GAME="globals blood screen view db gameutil fire sectorfx map2d satimer usrhooks endgame levels
sound sfx controls actor player replace tile triggers eventq qav weapon gui seq mirrors dude warp
weather credits menu messages ai aibat aibeast aiboneel aiburn aicaleb aicerber aicult aigarg
aigilbst aighost aihand aihound aiinnoc aipod airat aispid aitchern aizomba aizombf fx callback
loadsave config network demo gib asound gamemenu cdrom smkplay choke"
QTU="qheap resource iob trig getopt error debug4g textio pqueue key inifile qmouse misc gfx helix"
UNITS="$(for g in $GAME; do echo blood/$g; done) $(for q in $QTU; do echo blood/qtools/src/$q; done) c4/netnone"

# 1. B0
BLOOD_DIR=$G src/c4cxx/blood-b0.sh $W/ref || exit 1
# 2. c4cxx, then gcc
bad=0 cxx=
for u in $UNITS; do
	case $u in blood/qtools/*) def=-DAPPVER_EXEDEF=$QT ;; blood/*) def=-DAPPVER_EXEDEF=$VER ;; *) def= ;; esac
	b=$(echo $u | tr / _)
	src=$B/$u.cpp
	[ -f $src ] || src=$G/$u.cpp
	printf '#include "c4blood.h"\n#include "%s"\n' $src > $H/w_$b.cpp
	cxx="$cxx $b.o"
	if ! ./c4cxx -x c++ $WF $C4CXX_FLAGS $GNU -D __C4__ -D C4BLOOD -D MAPEDIT=0 $def $INC $SYS -o $H/$b.c $H/w_$b.cpp > $H/$b.err 2>&1; then
		echo "blood-oracle: c4cxx $u: $(grep -m1 error $H/$b.err | cut -c1-200)"; bad=$((bad + 1)); continue
	fi
	if ! $GCC -c -o $H/$b.o $H/$b.c > $H/$b.gcc 2>&1; then
		echo "blood-oracle: gcc $u: $(grep -m1 error $H/$b.gcc | cut -c1-200)"; bad=$((bad + 1))
	fi
done
[ $bad = 0 ] || exit 1
# 3. the link
gcc -m32 -O2 -g -w -c -o $H/libc4cxx.o src/c4cxx/rt/libc4cxx.c || exit 1
c=
for o in $W/ref/obj/*.o; do
	case " $cxx c4_cxxrt.o " in *" $(basename $o) "*) ;; *) c="$c $o" ;; esac
done
objs="$H/blood_loadsave.o"
for o in $cxx; do [ $o = blood_loadsave.o ] || objs="$objs $H/$o"; done
gcc -m32 -g -o $W/blood-cxx $objs $c $W/ref/rand.o $H/libc4cxx.o -lm > $W/link.log 2>&1 || {
	echo "blood-oracle: the link failed"; grep -E "undefined reference|multiple definition" $W/link.log | sed 's/.*: //' | sort | uniq -c | head; exit 1; }
echo "blood-oracle: $(echo $UNITS | wc -w) C++ units through c4cxx and gcc, linked with $(echo $c | wc -w) of host.sh's C objects"
# 4. the demos
for p in ref/blood-host blood-cxx; do
	(cd $RUN && timeout 3000 $W/$p -sum 1 -frames $FRAMES) > $W/$(basename $p).out 2>&1
done
grep "blood-host: frame" $W/blood-host.out | sed 's/ t=.*//' > $W/ref.sums
grep "blood-host: frame" $W/blood-cxx.out | sed 's/ t=.*//' > $W/cxx.sums
n=$(wc -l < $W/ref.sums)
# B0 may stop short on one of Blood's assertions ("ERROR (N) FILE": built
# in the source's order, warp.cpp's 235 in the demo loop, blood-c4lc.sh);
# blood-cxx must stop with it
stopped () { grep -m1 -o 'ERROR ([0-9]*) [^%]*' $1 | sed 's|) .*/|) |'; }
STOP= how=
if [ "$n" -lt "$FRAMES" ]; then
	STOP=$(stopped $W/blood-host.out)
	[ -n "$STOP" ] || { echo "blood-oracle: B0 ran $n frames of $FRAMES"; tail -3 $W/blood-host.out; exit 1; }
	echo "blood-oracle: B0 stops after frame $n of $FRAMES: $STOP"
fi
if ! cmp -s $W/ref.sums $W/cxx.sums; then
	echo "blood-oracle: the frames differ from $(diff $W/ref.sums $W/cxx.sums | grep -m1 '^<' | sed 's/^< blood-host: //')"
	tail -3 $W/blood-cxx.out; exit 1
fi
if [ -n "$STOP" ]; then
	[ "$(stopped $W/blood-cxx.out)" = "$STOP" ] || { echo "blood-oracle: $n frames, B0's, but not B0's stop ($STOP)"; tail -3 $W/blood-cxx.out; exit 1; }
	how=", and B0's stop"
fi
echo "blood-oracle: $n frames, every checksum B0's$how ($(tail -1 $W/ref.sums | sed 's/.*total/total/'))"
# 5. the sizes
if command -v gdb > /dev/null; then
	# unit by unit, g++'s object against c4cxx's, so that a type has one
	# definition (a program's C units lay theirs out without -mms-bitfields):
	# the types c4cxx defined in the unit, but its vtables (X__vt), which
	# g++'s have no name for; each its own -ex, so that one a unit's debug
	# information leaves out (g++ keeps no unused one) stops nothing
	: > $W/sizes.both
	for o in $cxx; do
		b=${o%.o}
		grep -o "^struct [A-Za-z_][A-Za-z_0-9]* {\|[;{] struct [A-Za-z_][A-Za-z_0-9]* {" $H/$b.c |
			sed 's/.*struct \([A-Za-z_0-9]*\) {/\1/' | grep -v "__vt$" | sort -u > $W/types.txt
		args=()
		while read t; do args+=(-ex "printf \"$t %d\\n\", (int) sizeof (struct $t)"); done < $W/types.txt
		for f in ref/obj/$o cxx/$o; do
			gdb -q -batch "${args[@]}" $W/$f 2>/dev/null | grep "^[A-Za-z_][A-Za-z_0-9]* [0-9]*$" | sort > $W/$(dirname $f | tr / _).sizes
		done
		join $W/ref_obj.sizes $W/cxx.sizes | sed "s/^/$b /" >> $W/sizes.both
	done
	k=$(awk '{ print $2 }' $W/sizes.both | sort -u | wc -l)
	d=$(awk '$3 != $4' $W/sizes.both)
	if [ -n "$d" ]; then echo "blood-oracle: sizes differ (unit, type, g++'s, c4cxx's):"; echo "$d" | head; exit 1; fi
	echo "blood-oracle: $k structs and classes, in $(wc -l < $W/sizes.both) unit's uses, each of g++'s size"
fi
