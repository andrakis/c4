#!/bin/bash
# blood-b0.sh OUT: B0, the host build c4's Blood is held to (blood-oracle.sh,
# blood-c4lc.sh): c4-Blood's c4/host.sh into OUT, as it is, with two of DOS
# Blood's ways that host.sh leaves to the host (docs/c4cxx-plan.md 4.11):
#
#   - globals in the source's order (gcc and g++ -fno-toplevel-reorder, by
#     wrappers in OUT/bin ahead of PATH), as Watcom and c4rlink lay them
#     out. Blood reads outside its arrays: view.cpp takes xsector[-1] for a
#     sector without an xsector, and what it finds there follows the
#     layout, and the demos with it.
#   - Watcom's rand (b0/rand.c), as DOS Blood's was and C4IX's libc's is,
#     not glibc's: OUT/blood-host is linked again with it, as host.sh links
#     (loadsave first), and OUT/rand.o is there for blood-oracle.sh's link.
#
# BLOOD_DIR is c4-Blood (default ~/git/games/c4-blood); BLOOD_VER goes to
# host.sh. From the c4 tree's root.
OUT=${1:?"blood-b0.sh OUT"}
G=${BLOOD_DIR:-$HOME/git/games/c4-blood}
R=$(pwd)
mkdir -p $OUT/bin
for cc in gcc g++; do
	printf '#!/bin/sh\nexec %s -fno-toplevel-reorder "$@"\n' "$(command -v $cc)" > $OUT/bin/$cc
	chmod +x $OUT/bin/$cc
done
PATH=$OUT/bin:$PATH $G/c4/host.sh $OUT > $OUT/host.log 2>&1 || { echo "blood-b0: host.sh failed"; tail -5 $OUT/host.log; exit 1; }
gcc -m32 -O2 -g -c -o $OUT/rand.o $R/src/c4cxx/b0/rand.c || exit 1
OBJS="$OUT/obj/blood_loadsave.o $(ls $OUT/obj/*.o | grep -v /blood_loadsave.o)"
gcc -m32 -g -o $OUT/blood-host $OBJS $OUT/rand.o -lm > $OUT/link.log 2>&1 || { echo "blood-b0: the link failed"; tail -5 $OUT/link.log; exit 1; }
# the program's own rand, not glibc's
nm $OUT/blood-host | grep -q " T rand$" || { echo "blood-b0: blood-host has no rand of its own"; exit 1; }
