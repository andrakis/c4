#!/bin/bash
# test-types.sh [-I dir|-D def]... -- FILE...: c4cxx's types against gcc's.
# c4cxx -check-types writes each unit with _Static_asserts of every
# expression's type (and constant's value), every struct's layout and every
# global's size, as c4cxx works them out; gcc -m32, which lays out and types
# C as c4lc's 32-bit target does, must find them all true. A unit gcc itself
# cannot compile is counted and skipped.
inc=()
while [ "$1" != "--" ]; do
	case "$1" in -I|-D) inc+=("$1" "$2"); shift 2 ;; *) inc+=("$1"); shift ;; esac
done
shift
C4CXX=${C4CXX:-./c4cxx}
T=${T:-$(mktemp -d)}
good=0 bad=0 fail=0 skip=0
GCC="gcc -m32 -std=gnu11 -fsyntax-only -fpermissive -w -fno-builtin"
LC=${C4LC32:-$HOME/git/c4/c4sc-host32}
for f in "$@"; do
	n=$(basename "$f" | sed 's/\.[A-Za-z]*$//')
	# what c4lc does not compile, c4cxx is not asked to (as test-same.sh)
	if ! "$LC" -c 400000 compile -c -O -P "${inc[@]}" "$f" $T/$n.c4o > /dev/null 2>&1; then
		skip=$((skip + 1)); continue
	fi
	if ! $C4CXX "${inc[@]}" -o $T/$n.c "$f" 2> $T/$n.err; then
		fail=$((fail + 1)); echo "C4CXX FAILED $f: $(head -1 $T/$n.err)"; continue
	fi
	if ! $GCC $T/$n.c 2> /dev/null; then
		skip=$((skip + 1)); continue
	fi
	$C4CXX -check-types "${inc[@]}" -o $T/$n.t.c "$f" 2> $T/$n.err
	if $GCC $T/$n.t.c 2> $T/$n.gcc; then
		good=$((good + 1))
	else
		bad=$((bad + 1))
		echo "TYPES DIFFER $f: $(grep -c 'static assertion failed' $T/$n.gcc) assertions"
		grep -m 5 "error" $T/$n.gcc | sed 's/^/    /' | cut -c1-200
	fi
done
echo "test-types: $good agree with gcc, $bad differ, $fail failed, $skip skipped (c4lc or gcc rejects the unit)"
[ -z "$KEEP" ] && rm -rf $T
[ $bad = 0 ] && [ $fail = 0 ]
