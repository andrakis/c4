#!/bin/bash
# blood-units.sh [c4cxx options]: c4cxx (default -parse) over Blood's C++
# units (Blood-RE's game and qtools, as c4-Blood's host.sh builds them),
# each through a wrapper that includes c4blood.h first, as host.sh's
# -include does. Prints the units that fail with their first error, and a
# count. From the c4 tree's root (c4cxx built there, or C4CXX).
R=$(pwd)
G=${BLOOD_DIR:-$HOME/git/games/c4-blood}
B=$G/source C=$G/c4 S=$HOME/gate/c4cxx/shim
# W: where the wrappers and outputs go (default a scratch directory, removed)
if [ -z "$W" ]; then
	W=$(mktemp -d)
	trap 'rm -rf $W' EXIT
fi
VER=${BLOOD_VER:-BS111A}
mkdir -p $W
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
	w=$W/w_$(basename $u).cpp
	printf '#include "c4blood.h"\n#include "%s"\n' $B/blood/$u.cpp > $w
	n=$((n + 1))
	if ! ${C4CXX:-./c4cxx} $opts -D __C4__ -D C4BLOOD -D C4BLOOD_C4 -D MAPEDIT=0 $def $INC -o $W/$(basename $u).c $w \
		> $W/$(basename $u).err 2>&1; then
		bad=$((bad + 1))
		echo "$u: $(grep -m1 -i 'error' $W/$(basename $u).err | cut -c1-200)" | tee -a $W/fails.txt
	fi
done
echo "blood-units.sh $opts: $((n - bad)) of $n units pass"
[ $bad = 0 ]
