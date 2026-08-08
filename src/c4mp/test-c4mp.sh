#!/bin/bash
#
# test-c4mp: stage 0 proves c4mp is a c4m you can trust.
#
# Three claims, in order of how much they would hurt to get wrong:
#
#   1. Equivalence. Every test image produces the same output and the
#      same exit status under `c4m load-c4r.c` and under c4mp. c4mp
#      has no compiler, so the comparison is loader-to-loader and the
#      only variable is the instruction set.
#   2. Backwards compatibility. The same c4mp, compiled by c4lc into
#      c4mp.c4r, runs hosted by c4m and produces those same results.
#   3. Nesting. c4mp runs itself. This is the property that proves the
#      modules smuggled in no host dependency -- anything c4mp needs
#      that it does not also provide shows up here and nowhere else.
#
# Masked, and why each is a measurement rather than behaviour:
#   * hex addresses -- both VMs malloc their own segments
#   * NNNms         -- elapsed time belongs to the host clock
#   * the VM's own name in its diagnostics
#
# stdbuf -oL is not cosmetic. Two of these images deliberately run off
# the end of a missed trap and segfault; without line buffering the
# two VMs truncate their stdio buffers at different points and the
# diff reports a difference that only exists after the program died.
#
set -u
cd "$(dirname "$0")/../.."

C4M=./c4m
C4MP=./c4mp
TESTS=src/tests
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# The 300KB cap is not tidiness. Several of these images probe custom
# opcodes in a loop with no handler installed, so both VMs print two
# diagnostic lines per probe forever until `timeout` kills them --
# test_timekeeping alone reaches 21 million lines and filled this
# machine's /tmp the first time this ran uncapped. Comparing a 300KB
# prefix plus the exit status is the whole check for those; every test
# that terminates on its own is far below the cap and compared in full.
mask() { sed -E 's/0x[0-9A-Fa-f]+/0xA/g; s/[0-9]+ms/Nms/g; s/^c4mp:/c4m:/' | head -c 300000; }

fail=0

# ---- 1. native equivalence, every image ----
for f in $TESTS/*.c4r; do
    b=$(basename "$f" .c4r)
    # runs forever by design; there is nothing to compare
    [ "$b" = "test_infiniteloop" ] && continue

    timeout 60 stdbuf -oL $C4M load-c4r.c -- "$f" </dev/null 2>&1 | mask > "$TMP/a"
    ra=${PIPESTATUS[0]}
    timeout 60 stdbuf -oL $C4MP            "$f" </dev/null 2>&1 | mask > "$TMP/b"
    rb=${PIPESTATUS[0]}

    if ! cmp -s "$TMP/a" "$TMP/b"; then
        echo "test-c4mp: FAIL $b: output differs"
        diff "$TMP/a" "$TMP/b" | head -6
        fail=1
    elif [ "$ra" != "$rb" ]; then
        echo "test-c4mp: FAIL $b: exit status c4m=$ra c4mp=$rb"
        fail=1
    fi
done
[ $fail = 0 ] && echo "test-c4mp: native equivalence OK"

# ---- 2. the c4lc build, hosted by c4m ----
# A subset: each of these runs the whole image through two interpreters,
# so the full set would take longer than it is worth here.
for b in hello factorial test_basic test_printf test-ptrs test_static test-order; do
    timeout 120 stdbuf -oL $C4M load-c4r.c -- "$TESTS/$b.c4r" </dev/null 2>&1 | mask > "$TMP/a"
    timeout 300 stdbuf -oL $C4M load-c4r.c -- c4mp.c4r "$TESTS/$b.c4r" </dev/null 2>&1 | mask > "$TMP/b"
    if ! cmp -s "$TMP/a" "$TMP/b"; then
        echo "test-c4mp: FAIL $b hosted by c4m: output differs"
        diff "$TMP/a" "$TMP/b" | head -6
        fail=1
    fi
done
[ $fail = 0 ] && echo "test-c4mp: c4mp.c4r under c4m OK"

# ---- 3. nesting ----
timeout 60  $C4MP c4mp.c4r "$TESTS/hello.c4r" </dev/null | grep -q yello \
    || { echo "test-c4mp: FAIL c4mp -> c4mp -> hello"; fail=1; }
timeout 300 $C4MP c4mp.c4r c4mp.c4r "$TESTS/hello.c4r" </dev/null | grep -q yello \
    || { echo "test-c4mp: FAIL c4mp -> c4mp -> c4mp -> hello"; fail=1; }
[ $fail = 0 ] && echo "test-c4mp: nesting OK"

if [ $fail = 0 ]; then echo "test-c4mp: OK"; else echo "test-c4mp: FAILED"; fi
exit $fail
