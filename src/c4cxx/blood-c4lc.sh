#!/bin/bash
# blood-c4lc.sh [FRAMES]: M7's test (docs/c4cxx-plan.md 4.11). Blood built
# by c4 alone plays the shareware's demos as c4-Blood's host build (B0,
# g++) does, frame for frame:
#
#   1. B0: c4-Blood's c4/host.sh, into $W/ref (kept: built once).
#   2. The C units host.sh builds -- chocolate-blood's engine, MACT and
#      audiolib, fframe and crc32 in C, and c4/'s platform with no display
#      and no sound (blood-host.c) -- through c4lc (lc32), against c4's libc
#      headers, as host.sh gives them to gcc: -funsigned-char everywhere,
#      and Watcom's layouts (-fshort-enums -mms-bitfields) for Blood's own.
#   3. Every C++ unit, and c4/netnone.cpp, through c4cxx against the same
#      headers, with Watcom's layouts, then c4lc.
#   4. c4rlink32: loadsave first (its constructor builds the list the
#      savers' join), the rest, rt/libc4cxx.c for c4/cxxrt.cpp, and the
#      bare machine's libc: $W/blood.c4r.
#   5. Both play FRAMES frames (default 4000) of the first demo in
#      $BLOOD_RUN, every frame's checksum printed (-sum 1): B0 natively,
#      blood.c4r on the bare machine in node (libjs/cli.js, the directory
#      as its drive). The two lists must be the same. The demo is named
#      (-nodemo -playback BLOOD000.DEM), because Blood finds its demo loop
#      with _dos_findfirst, which c4/c4blood.c makes of opendir, and the
#      bare machine's drive has no listing: there the loop is empty.
#
# From the c4 tree's root, with c4cxx, c4sc-host32, c4rlink32 and the bare
# libc (make libc-bare-objs) built, and node on the path. BLOOD_DIR is
# c4-Blood (default ~/git/games/c4-blood), BLOOD_VER the version (BS111A),
# W the work directory (default /tmp/blood-c4lc; kept), BLOOD_RUN a
# directory of the game's files with BLOOD.CFG (blood-oracle.sh's).
cd "$(dirname "$0")/../.." || exit 1
R=$(pwd)
FRAMES=${1:-4000}
G=${BLOOD_DIR:-$HOME/git/games/c4-blood}
B=$G/source C=$G/c4
VER=${BLOOD_VER:-BS111A}
case $VER in
	BS110|BSR110|BR110|BP110|BS111|BSR111|BR111|BP111) QT=QT110 ;;
	*) QT=QT111A ;;
esac
W=${W:-/tmp/blood-c4lc}
RUN=${BLOOD_RUN:?"BLOOD_RUN: a directory of the game's files and BLOOD.CFG"}
O=$W/obj
mkdir -p $O
LC="./c4sc-host32 -c 400000 compile -O -fno-builtin -c -conforming -funsigned-char"
WF="-fshort-enums -mms-bitfields"
DEF="-D __C4__ -D C4BLOOD -D C4BLOOD_C4 -D MAPEDIT=0"
GINC="-I $B/blood -I $B/blood/qtools/include -I $B/build -I $B/mact -I $B/audiolib"
LIBC="-I $R/src/c4ix/libc/include -I $R/src/c4ix/include"
C_UNITS="build/a_asm.c build/CACHE1D.C build/ENGINE.C build/serialize.c build/video_font.c build/MMULTI.C
mact/ANIMLIB.C mact/control.c mact/file_lib.c mact/keyboard.c mact/mathutil.c mact/scriplib.c
mact/tokenlib.c mact/util_lib.c mact/vidmsdos.c
audiolib/AL_MIDI.C audiolib/BLASTER.C audiolib/DEBUGIO.C audiolib/FX_MAN.C audiolib/GMTIMBRE.C
audiolib/LL_MAN.C audiolib/MIDI.C audiolib/MPU401.C audiolib/MULTIVOC.C audiolib/MUSIC.C
audiolib/mvreverb_asm.c audiolib/mv_mix16_asm.c audiolib/mv_mix_asm.c audiolib/PITCH.C
audiolib/TASK_MAN.C audiolib/USER.C blood/fframe_asm.c blood/qtools/src/crc32_asm.c"
GAME="globals blood screen view db gameutil fire sectorfx map2d satimer usrhooks endgame levels
sound sfx controls actor player replace tile triggers eventq qav weapon gui seq mirrors dude warp
weather credits menu messages ai aibat aibeast aiboneel aiburn aicaleb aicerber aicult aigarg
aigilbst aighost aihand aihound aiinnoc aipod airat aispid aitchern aizomba aizombf fx callback
loadsave config network demo gib asound gamemenu cdrom smkplay choke"
QTU="qheap resource iob trig getopt error debug4g textio pqueue key inifile qmouse misc gfx helix"
CXX_UNITS="$(for g in $GAME; do echo blood/$g; done) $(for q in $QTU; do echo blood/qtools/src/$q; done) c4/netnone"

# 1. B0
if [ ! -x $W/ref/blood-host ]; then
	$C/host.sh $W/ref > $W/ref.log 2>&1 || { echo "blood-c4lc: host.sh failed"; tail -5 $W/ref.log; exit 1; }
fi
bad=0 objs=
# one unit through c4lc: NAME, the source to wrap after c4blood.h, flags
lc () {
	local n=$1 src=$2; shift 2
	printf '#include "c4blood.h"\n#include "%s"\n' $src > $O/w_$n.c
	if ! $LC "$@" $O/w_$n.c $O/$n.c4o > $O/$n.lc 2>&1 || [ ! -s $O/$n.c4o ]; then
		echo "blood-c4lc: c4lc $src: $(grep -v '^;;' $O/$n.lc | grep -v '^warning' | head -1 | cut -c1-200)"
		bad=$((bad + 1)); return 1
	fi
	objs="$objs $O/$n.c4o"
}
# 2. the C units
for u in $C_UNITS; do
	n=$(echo $u | tr / _ | sed 's/\.[A-Za-z]*$//')
	case $u in
	blood/qtools/*) lc $n $B/$u $WF -D APPVER_EXEDEF=$QT $DEF -I $C/c4inc -I $C/inc $GINC $LIBC ;;
	blood/*) lc $n $B/$u $WF -D APPVER_EXEDEF=$VER $DEF -I $C/c4inc -I $C/inc $GINC $LIBC ;;
	# the engine, MACT and audiolib: their own headers first (Blood has a
	# sound.h too)
	*) lc $n $B/$u $DEF -I $B/build -I $B/mact -I $B/audiolib -I $C/c4inc -I $C/inc $GINC $LIBC ;;
	esac
done
for s in c4blood blood-host smacker; do
	lc c4_$s $C/$s.c $DEF -I $B/build -I $C/c4inc -I $C/inc $GINC $LIBC
done
# 3. the C++ units: c4cxx, then c4lc
cxx=
for u in $CXX_UNITS; do
	case $u in blood/qtools/*) def="-D APPVER_EXEDEF=$QT" ;; blood/*) def="-D APPVER_EXEDEF=$VER" ;; *) def= ;; esac
	b=$(echo $u | tr / _)
	src=$B/$u.cpp
	[ -f $src ] || src=$G/$u.cpp
	printf '#include "c4blood.h"\n#include "%s"\n' $src > $O/w_$b.cpp
	if ! ./c4cxx -x c++ $WF -funsigned-char $DEF $def -I $C/c4inc -I $C/inc $GINC \
			-isystem src/c4ix/libc/include -isystem src/c4ix/include -o $O/$b.c $O/w_$b.cpp > $O/$b.err 2>&1; then
		echo "blood-c4lc: c4cxx $u: $(grep -m1 error $O/$b.err | cut -c1-200)"; bad=$((bad + 1)); continue
	fi
	if ! $LC -P $WF $O/$b.c $O/$b.c4o > $O/$b.lc 2>&1 || [ ! -s $O/$b.c4o ]; then
		echo "blood-c4lc: c4lc $u: $(grep -v '^;;' $O/$b.lc | grep -v '^warning' | head -1 | cut -c1-200)"
		bad=$((bad + 1)); continue
	fi
	[ $b = blood_loadsave ] || cxx="$cxx $O/$b.c4o"
done
# c4cxx's runtime, after c4blood.h too: under -fno-builtin its printf is
# libc's, whose variadic calls want libc's declarations
lc libc4cxx $R/src/c4cxx/rt/libc4cxx.c $DEF -I $C/c4inc -I $C/inc $LIBC
objs=${objs% $O/libc4cxx.c4o}
[ $bad = 0 ] || exit 1
echo "blood-c4lc: $(echo $C_UNITS | wc -w) C units, 3 of c4's and $(echo $CXX_UNITS | wc -w) C++ units through c4lc"
# 4. the link
./c4rlink32 $O/blood_loadsave.c4o $cxx $objs $O/libc4cxx.c4o .libc-bare/libc-*.c4o -o $W/blood.c4r > $W/link.log 2>&1 \
	|| { echo "blood-c4lc: the link failed"; grep -v '^;;' $W/link.log | head; exit 1; }
echo "blood-c4lc: $W/blood.c4r, $(stat -c %s $W/blood.c4r) bytes"
# 5. the demos
DEMO="-nodemo -playback BLOOD000.DEM"
(cd $RUN && timeout 3000 $W/ref/blood-host $DEMO -sum 1 -frames $FRAMES) > $W/blood-host.out 2>&1
# The bare machine's drive has no listing, so c4/c4blood.c's search for a
# name without regard to case (Blood opens tables.dat; the file is
# TABLES.DAT) finds nothing there: the drive is $RUN's files under their
# own names and in lower case, as links. And each file it opens is read
# whole into memory (bare.c): 256 MB.
rm -rf $W/run && mkdir -p $W/run
for f in $RUN/*; do
	b=$(basename $f) l=$(basename $f | tr A-Z a-z)
	ln -s $(readlink -f $f) $W/run/$b
	[ $l = $b ] || ln -s $(readlink -f $f) $W/run/$l
done
(cd $W/run && timeout ${C4_TIMEOUT:-20000} node $R/libjs/cli.js -m ${C4_MEM:-256} -d . $W/blood.c4r $DEMO -sum 1 -frames $FRAMES) \
	> $W/blood-c4lc.out 2>&1
grep "blood-host: frame" $W/blood-host.out | sed 's/ t=.*//' > $W/ref.sums
grep "blood-host: frame" $W/blood-c4lc.out | sed 's/ t=.*//' > $W/c4lc.sums
n=$(wc -l < $W/ref.sums)
if [ "$n" -lt "$FRAMES" ]; then echo "blood-c4lc: B0 ran $n frames of $FRAMES"; tail -3 $W/blood-host.out; exit 1; fi
if ! cmp -s $W/ref.sums $W/c4lc.sums; then
	echo "blood-c4lc: $(wc -l < $W/c4lc.sums) frames; they differ from $(diff $W/ref.sums $W/c4lc.sums | grep -m1 '^<' | sed 's/^< blood-host: //')"
	tail -3 $W/blood-c4lc.out; exit 1
fi
echo "blood-c4lc: $n frames, every checksum B0's ($(tail -1 $W/ref.sums | sed 's/.*total/total/'))"
