#!/bin/bash
# blood-units.sh [-compile] [c4cxx options]: c4cxx (default -parse) over
# Blood's C++ units (Blood-RE's game and qtools, as c4-Blood's host.sh
# builds them), each through a wrapper that includes c4blood.h first, as
# host.sh's -include does. Prints the units that fail with their first
# error, and a count. From the c4 tree's root (c4cxx built there, or
# C4CXX).
#
# -compile translates each unit with the layouts Blood's host build gives
# g++ (-fshort-enums -mms-bitfields -funsigned-char) and compiles what
# c4cxx wrote with gcc -m32 (the same flags, -fpermissive: c4's libc
# declares memset and the like on char *) and with c4lc (C4CXX_LC,
# default ./c4sc-host32, -c -mms-bitfields). A unit passes when all three do.
R=$(pwd)
G=${BLOOD_DIR:-$HOME/git/games/c4-blood}
B=$G/source C=$G/c4 S=$HOME/gate/c4cxx/shim
LC=${C4CXX_LC:-./c4sc-host32}
# W: where the wrappers and outputs go (default a scratch directory, removed)
if [ -z "$W" ]; then
	W=$(mktemp -d)
	trap 'rm -rf $W' EXIT
fi
VER=${BLOOD_VER:-BS111A}
mkdir -p $W
compile=
if [ "$1" = -compile ]; then
	compile=1
	shift
	set -- -x c++ -fshort-enums -mms-bitfields -funsigned-char "$@"
fi
opts=${*:--parse}
# host.sh's order, then c4's libc where host.sh has the system's, as headers of C
INC="-I $S/c4inc -I $C/inc -I $B/blood -I $B/blood/qtools/include -I $B/build -I $B/mact -I $B/audiolib
 -isystem $R/src/c4ix/libc/include -isystem $R/src/c4ix/include -isystem $R/libjs/guest"
GAME="globals blood screen view db gameutil fire sectorfx map2d satimer usrhooks endgame levels
 menu gamemenu config controls sound sfx asound seq qav tile mirrors actor ai aibat aibeast aiboneel
 aiburn aicaleb aicerber aicult aigarg aighost aigilbst aihand aihound aiinnoc aipod airat aispid
 aitchern aizomba aizombf callback choke credits demo dude eventq fx gib gui loadsave messages
 player replace smkplay triggers warp weapon weather network cdrom"
QT="qheap resource iob trig getopt error debug4g textio pqueue key inifile qmouse misc gfx helix"
n=0 bad=0
: > $W/fails.txt
for u in $GAME $(for q in $QT; do echo qtools/src/$q; done); do
	case $u in qtools/*) def=-DAPPVER_EXEDEF=QT110 ;; *) def=-DAPPVER_EXEDEF=$VER ;; esac
	b=$(basename $u)
	w=$W/w_$b.cpp
	printf '#include "c4blood.h"\n#include "%s"\n' $B/blood/$u.cpp > $w
	n=$((n + 1))
	if ! ${C4CXX:-./c4cxx} $opts -D __C4__ -D C4BLOOD -D C4BLOOD_C4 -D MAPEDIT=0 $def $INC -o $W/$b.c $w \
		> $W/$b.err 2>&1; then
		bad=$((bad + 1))
		echo "$u: $(grep -m1 -i 'error' $W/$b.err | cut -c1-200)" | tee -a $W/fails.txt
	elif [ -n "$compile" ] && ! gcc -m32 -std=gnu99 -fpermissive -w -fshort-enums -mms-bitfields -funsigned-char \
			-c -o $W/$b.o $W/$b.c > $W/$b.gcc 2>&1; then
		bad=$((bad + 1))
		echo "$u: gcc: $(grep -m1 error $W/$b.gcc | cut -c1-200)" | tee -a $W/fails.txt
	elif [ -n "$compile" ] && ! $LC compile -P -c -mms-bitfields $W/$b.c $W/$b.c4o > $W/$b.lc 2>&1; then
		bad=$((bad + 1))
		echo "$u: c4lc: $(grep -v '^;;' $W/$b.lc | grep -v '^warning' | head -1 | cut -c1-200)" | tee -a $W/fails.txt
	fi
done
echo "blood-units.sh ${compile:+-compile }$opts: $((n - bad)) of $n units pass"
[ $bad = 0 ]
