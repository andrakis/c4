#!/bin/bash
# test-same.sh MODE [-I dir|-D def]... -- FILE...: c4cxx against c4lc. For
# each FILE, c4lc (lc32, -c -O -P) compiles it as it is and compiles what
# c4cxx makes of it; the two objects must be the same bytes. MODE is -E
# (M1a: the preprocessed tokens) or -C (M1b on: parsed and written out as C).
#
# When they differ, gcc's preprocessor breaks the tie: c4lc compiles gcc -E's
# output too (c4's headers only, gcc's own names undefined). If that object
# is c4cxx's, c4lc's preprocessor is the odd one out (reported, not failed).
# A FILE that c4lc itself rejects is counted and skipped.
#   C4LC32  the compiler (default: ~/git/c4/c4sc-host32)
#   T       work directory (default: a new one, removed unless KEEP is set)
mode=$1; shift
inc=()
while [ "$1" != "--" ]; do
	case "$1" in -I|-D) inc+=("$1" "$2"); shift 2 ;; *) inc+=("$1"); shift ;; esac
done
shift
C4CXX=${C4CXX:-./c4cxx}
LC=${C4LC32:-$HOME/git/c4/c4sc-host32}
T=${T:-$(mktemp -d)}
same=0 diff=0 skip=0 fail=0 lcdiff=0
for f in "$@"; do
	n=$(basename "$f" | sed 's/\.[A-Za-z]*$//')
	if ! "$LC" -c 400000 compile -c -O -P "${inc[@]}" "$f" $T/$n.a.c4o > $T/$n.a.log 2>&1; then
		skip=$((skip + 1)); continue
	fi
	cx=-E; [ "$mode" = -C ] && cx=
	if ! $C4CXX $cx "${inc[@]}" -o $T/$n.x.c "$f" 2> $T/$n.x.err; then
		fail=$((fail + 1)); echo "C4CXX FAILED $f: $(head -1 $T/$n.x.err)"; continue
	fi
	if ! "$LC" -c 400000 compile -c -O -P $T/$n.x.c $T/$n.b.c4o > $T/$n.b.log 2>&1; then
		fail=$((fail + 1)); echo "C4LC REJECTS OUTPUT $f: $(grep -v '^;;' $T/$n.b.log | tail -1)"; continue
	fi
	if cmp -s $T/$n.a.c4o $T/$n.b.c4o; then
		same=$((same + 1)); continue
	fi
	if gcc -E -P -undef -nostdinc -D__c4_inttypes__=1 "${inc[@]}" -x c "$f" -o $T/$n.g.c 2>/dev/null &&
	   "$LC" -c 400000 compile -c -O -P $T/$n.g.c $T/$n.g.c4o > /dev/null 2>&1 &&
	   cmp -s $T/$n.g.c4o $T/$n.b.c4o; then
		lcdiff=$((lcdiff + 1)); echo "C4LC'S OWN -P DIFFERS (gcc -E agrees with c4cxx) $f"
	else
		diff=$((diff + 1)); echo "DIFFERENT $f"
	fi
done
echo "test-same $mode: $same same, $diff different, $fail failed, $lcdiff where c4lc's -P is the odd one out, $skip skipped (c4lc rejects the source)"
[ -z "$KEEP" ] && rm -rf $T
[ $diff = 0 ] && [ $fail = 0 ]
