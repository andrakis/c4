#!/bin/bash
# blood-c4lc.sh [FRAMES]: M7's test (docs/c4cxx-plan.md 4.11). Blood built
# by c4 alone plays the shareware's demos as c4-Blood's host build (B0,
# g++) does, frame for frame:
#
#   1. B0: c4-Blood's c4/host.sh, into $W/ref (kept: built once), by
#      blood-b0.sh: globals in the source's order, Watcom's rand.
#   2. The C units host.sh builds -- chocolate-blood's engine, MACT and
#      audiolib, fframe and crc32 in C, and c4/'s platform with no display
#      and no sound (blood-host.c) -- through c4lc (lc32), against c4's libc
#      headers, as host.sh gives them to gcc: -funsigned-char everywhere,
#      and Watcom's layouts (-fshort-enums -mms-bitfields) for Blood's own.
#   3. Every C++ unit, and c4/netnone.cpp, through c4cxx against the same
#      headers, with Watcom's layouts, then c4lc.
#   4. c4rlink32: loadsave first (its constructor builds the list the
#      savers' join), the rest, rt/libc4cxx.c for c4/cxxrt.cpp, and the
#      bare machine's libc: $W/blood.c4r. And with C4IX's libc instead,
#      when there are C4IX images to run it in: $W/c4ix-blood.c4r.
#   5. Blood's own demo loop, every frame's checksum printed (-sum 1), from
#      one directory of the game's files: B0 natively, FRAMES frames
#      (default 4000); then on the bare machine in node (libjs/cli.js, the
#      directory as its drive), the same; under C4DOS on that machine,
#      blood.c4r again, and inside C4IX, c4ix-blood.c4r, OS_FRAMES frames
#      each (default 1000). Each list must be B0's. Blood finds its demos
#      with _dos_findfirst, which c4/c4blood.c makes of opendir, so each
#      system lists the directory in B0's order: c4dos.dir on the bare
#      machine and under C4DOS (bare.c), /usr/games/blood in C4IX.
#
# From the c4 tree's root, with c4cxx, c4sc-host32, c4rlink32, the bare
# libc (make libc-bare-objs) and c4dos32.c4r built, and node on the path.
# BLOOD_DIR is c4-Blood (default ~/git/games/c4-blood), BLOOD_VER the
# version (BS111A), W the work directory (default /tmp/blood-c4lc; kept),
# BLOOD_RUN a directory of the game's files with BLOOD.CFG
# (blood-oracle.sh's), C4_IMAGES the images C4IX boots from (default
# src/c4bb/images: make c4bb-images; without them C4IX is skipped).
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
if [ ! -x $W/ref/blood-host ] || [ ! -f $W/ref/rand.o ]; then
	BLOOD_DIR=$G src/c4cxx/blood-b0.sh $W/ref || exit 1
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
# 3. the C++ units: c4cxx, then c4lc. -fargs-rtl: a call's arguments in
# g++'s order, which B0 has (Blood's weapons draw their spread so:
# actFireVector (..., dx+Random3(600), dy+Random3(600), ...))
cxx=
for u in $CXX_UNITS; do
	case $u in blood/qtools/*) def="-D APPVER_EXEDEF=$QT" ;; blood/*) def="-D APPVER_EXEDEF=$VER" ;; *) def= ;; esac
	b=$(echo $u | tr / _)
	src=$B/$u.cpp
	[ -f $src ] || src=$G/$u.cpp
	printf '#include "c4blood.h"\n#include "%s"\n' $src > $O/w_$b.cpp
	if ! ./c4cxx -x c++ $WF -funsigned-char -fargs-rtl $DEF $def -I $C/c4inc -I $C/inc $GINC \
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
# ... and for C4IX, when there are images to run it in (C4_IMAGES, which
# may be another tree's): with C4IX's libc, this tree's, linked in as
# vfsload has it (build-images.sh compiles the same units -fpic into
# /lib/libc.c4s), so that the disk's libraries need not be this tree's
IMG=${C4_IMAGES:-$R/src/c4bb/images}
if [ -f $IMG/c4ix32.c4r ] && [ -f $IMG/disk/c4ix.vfs.txt ]; then
	X=$W/ixlibc
	mkdir -p $X
	for m in string stdlib stdio unix fp math; do
		./c4sc-host32 -c 400000 compile -O -fno-builtin -c -I src/c4ix/libc/include -I src/c4ix/libc src/c4ix/libc/$m.c $X/$m.c4o \
			> $X/$m.lc 2>&1 || { echo "blood-c4lc: C4IX's libc: $m.c"; exit 1; }
	done
	./c4sc-host32 -c 400000 compile -O -c src/c4ix/libc/opcodes.c $X/opcodes.c4o > /dev/null \
		&& ./c4sc-host32 -c 400000 compile -O -c -I src/c4ix/include src/c4ix/lib/libc4ix.c $X/libc4ix.c4o > /dev/null \
		|| { echo "blood-c4lc: C4IX's libc: opcodes.c or libc4ix.c"; exit 1; }
	./c4rlink32 $O/blood_loadsave.c4o $cxx $objs $O/libc4cxx.c4o $X/*.c4o -o $W/c4ix-blood.c4r > $W/link-c4ix.log 2>&1 \
		|| { echo "blood-c4lc: the link for C4IX failed"; grep -v '^;;' $W/link-c4ix.log | head; exit 1; }
	echo "blood-c4lc: $W/c4ix-blood.c4r, $(stat -c %s $W/c4ix-blood.c4r) bytes"
fi

# 5. the demos: one directory of $RUN's files, as links, and blood.c4r.
# Blood finds its demo loop with _dos_findfirst, which c4/c4blood.c makes
# of opendir, and takes the demos in the order the directory gives them;
# it opens tables.dat, which is TABLES.DAT, by finding the name in the
# directory without regard to case. So each system's listing must be the
# host's: on the bare machine and under C4DOS it is c4dos.dir (bare.c),
# written here in the order B0 reads the directory in (ls -f); in C4IX
# the order of the game's directory in c4ix.vfs.txt, the same.
rm -rf $W/run && mkdir -p $W/run
for f in $RUN/*; do ln -s $(readlink -f $f) $W/run/; done
cp $W/blood.c4r $W/run/
(cd $W/run && ls -f | grep -v '^\.\.\?$' | grep -vx c4dos.dir > c4dos.dir)
NAMES=$(grep -vx 'blood.c4r' $W/run/c4dos.dir)
# Every frame's checksum (-sum 1), the time dropped: B0's is the host's,
# and the run's in C4IX is the machine's
sums () { tr -d '\r' < $1 | grep "blood-host: frame" | sed 's/ t=.*//; s/^.*blood-host: /blood-host: /'; }
(cd $W/run && timeout 3000 $W/ref/blood-host -sum 1 -frames $FRAMES) > $W/blood-host.out 2>&1
sums $W/blood-host.out > $W/ref.sums
n=$(wc -l < $W/ref.sums)
# Blood's dassert: "ERROR (N) FILE", the file as its build named it
stopped () { tr -d '\r' < $1 | grep -m1 -o 'ERROR ([0-9]*) [^%]*' | sed 's|) .*/|) |'; }
STOP=
if [ "$n" -lt "$FRAMES" ]; then
	# B0 stopped short: on one of Blood's assertions, the builds must stop
	# with it, after the same frames. Built in the source's order Blood
	# stops so in its demo loop: it reads gUpperLink[-1] (warp.cpp's
	# CheckLink), velCeil's last, 0, which is no link (docs/c4cxx-plan.md 4.11)
	STOP=$(stopped $W/blood-host.out)
	[ -n "$STOP" ] || { echo "blood-c4lc: B0 ran $n frames of $FRAMES"; tail -3 $W/blood-host.out; exit 1; }
	echo "blood-c4lc: B0 stops after frame $n of $FRAMES: $STOP"
fi
# what a run printed against B0's first FRAMES (fewer for the systems);
# if B0 stopped within them, the run must stop as B0
check () {
	local what=$1 out=$2 frames=$3
	[ $frames -le $n ] || frames=$n
	sums $out > $out.sums
	head -$frames $W/ref.sums > $W/ref-$frames.sums
	if ! cmp -s $W/ref-$frames.sums $out.sums; then
		echo "blood-c4lc: $what: $(wc -l < $out.sums) frames; they differ from $(diff $W/ref-$frames.sums $out.sums | grep -m1 '^<' | sed 's/^< blood-host: //')"
		tail -3 $out; return 1
	fi
	local how=
	if [ -n "$STOP" ] && [ $frames = $n ]; then
		if [ "$(stopped $out)" != "$STOP" ]; then
			echo "blood-c4lc: $what: $frames frames, B0's, but not B0's stop ($STOP)"; tail -3 $out; return 1
		fi
		how=", and B0's stop"
	fi
	echo "blood-c4lc: $what: $frames frames, every checksum B0's$how ($(tail -1 $out.sums | sed 's/.*total/total/'))"
}
OSF=${OS_FRAMES:-1000}
[ $OSF -le $FRAMES ] || OSF=$FRAMES
# Each file Blood opens is read whole into memory on the bare machine and
# under C4DOS (bare.c): 256 MB
(cd $W/run && timeout ${C4_TIMEOUT:-20000} node $R/libjs/cli.js -m ${C4_MEM:-256} -d . blood.c4r -sum 1 -frames $FRAMES) \
	> $W/bare.out 2>&1
check "the bare machine" $W/bare.out $FRAMES || exit 1
# Under C4DOS: the same image, a transient of C4DOS's
(cd $W/run && printf 'blood -sum 1 -frames %s\nver\n' $OSF | timeout ${C4_TIMEOUT:-20000} node $R/libjs/cli.js -m ${C4_MEM:-256} \
	-d . $R/c4dos32.c4r) > $W/dos.out 2>&1
check "C4DOS" $W/dos.out $OSF || exit 1
[ -f $W/c4ix-blood.c4r ] || { echo "blood-c4lc: no C4IX images in $IMG (make c4bb-images, or C4_IMAGES): C4IX skipped"; exit 0; }
# In C4IX: its disk, as links, with Blood in /usr/games/blood, its files
# on the disk as blood-NAME (the disk is flat and shared), listed in the
# directory B0 reads and in c4ix.sizes (vfsload: lazy files)
rm -rf $W/disk && mkdir -p $W/disk
for f in $(ls -A $IMG/disk); do ln -s $IMG/disk/$f $W/disk/$f; done
rm $W/disk/c4ix.vfs.txt $W/disk/c4ix.sizes
cp $W/c4ix-blood.c4r $W/disk/
for f in $NAMES; do ln -s $(readlink -f $W/run/$f) $W/disk/blood-$f; done
{
	printf '\t\t\t# Blood (c4cxx, src/c4cxx/blood-c4lc.sh): the test'"'"'s\n\t\t\tblood/:\n\t\t\t\tblood.c4r: c4ix-blood.c4r\n'
	# C4IX lists a directory newest entry first: the last in c4ix.vfs.txt
	for f in $(echo $NAMES | tr ' ' '\n' | tac); do printf '\t\t\t\t%s: blood-%s\n' $f $f; done
} > $W/vfs-blood.txt
awk -v add=$W/vfs-blood.txt '/^\t\t\tduke3d\/:$/ && !done { while ((getline l < add) > 0) print l; done = 1 } { print } END { if (!done) exit 1 }' \
	$IMG/disk/c4ix.vfs.txt > $W/disk/c4ix.vfs.txt || { echo "blood-c4lc: no /usr/games/duke3d in $IMG/disk/c4ix.vfs.txt to put Blood beside"; exit 1; }
{
	echo "$(stat -c %s $W/disk/c4ix-blood.c4r) c4ix-blood.c4r"
	for f in $NAMES; do echo "$(stat -L -c %s $W/disk/blood-$f) blood-$f"; done
	cat $IMG/disk/c4ix.sizes
} > $W/disk/c4ix.sizes
printf 'cd /usr/games/blood\n./blood.c4r -sum 1 -frames %s\nexit\n' $OSF | timeout ${C4_TIMEOUT:-20000} \
	node $R/libjs/cli.js -m ${C4_MEM:-256} -d $W/disk $IMG/c4ix32.c4r > $W/c4ix.out 2>&1
check "C4IX" $W/c4ix.out $OSF || exit 1
