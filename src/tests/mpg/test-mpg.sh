#!/bin/bash
# test-mpg.sh - the c4mpg guard, both halves.
#
# Run from the repo root; expects ./c4m and ./c4mpg (make c4mpg).
#
# The KNOWN-BAD half asserts that each sin is caught AND named: the
# exact region, and how far past it. A guard that says only "something
# is wrong" is a guard you go back to printf for, so the phrasing is
# pinned here on purpose and a change that makes a message vaguer fails
# the build.
#
# The KNOWN-GOOD half is not optional, and is the half that has already
# earned its place: the first corpus run tripped on c4m's own switch
# jumptables (plain malloc'd memory the compiled program reads through
# an LI) and on src/tests/c4_jailbreak.c writing into the code area,
# which in this family is a documented technique and not a bug. A guard
# that cries wolf gets turned off within a day.
#
# docs/c4mpg-design.md
set -u
fail=0
MPG=./c4mpg
C4M=./c4m
T=src/tests/mpg

if [ ! -x $MPG ]; then echo "test-mpg: no ./c4mpg (run 'make c4mpg')" >&2; exit 1; fi

# ---- known bad: each must be caught, and named -----------------------
# want <program> <exit> <phrase>...
want () {
    prog=$1; shift
    code=$1; shift
    out=$(timeout 120 $MPG $T/$prog.c 2>&1)
    rc=$?
    ok=1
    [ "$rc" = "$code" ] || { echo "test-mpg: $prog expected exit $code, got $rc"; ok=0; }
    # A "STILL RUNNING" line means the program got past its own sin.
    if echo "$out" | grep -qF "STILL RUNNING"; then
        echo "test-mpg: $prog ran ON past the violation"; ok=0
    fi
    for phrase in "$@"; do
        echo "$out" | grep -qF "$phrase" || { echo "test-mpg: $prog missing: $phrase"; ok=0; }
    done
    if [ $ok = 1 ]; then echo "test-mpg: $prog caught and named OK"
    else echo "$out" | sed 's/^/    /'; fail=1; fi
}

want mpg_overrun  11 "outside every region" "nearest below: 'malloc'" \
                     "8 bytes past the end of it" "int main()"
want mpg_underrun 11 "outside every region" "nearest above: 'malloc'" \
                     "8 bytes higher" "int main()"
want mpg_freed    11 "outside every region" "THIS WAS A REGION" "Use after free" \
                     "int main()"
# M2: the buffer, not its first byte. These two are the vfsload bug
# (docs/dos-rung-fixes.md F12) in both of its halves, and neither is
# visible to a per-instruction check -- one overruns inside the host's
# read(), the other inside the host's printf().
want mpg_syscall      11 "read() into" "runs 65472 bytes past the end" "int main()"
want mpg_unterminated 11 "printf() %s argument" "no terminator" "int main()"

# ---- known good: identical to c4m, to the byte ----------------------
# Programs that print an address of their own are excluded by name, not
# by a loose comparison: their output legitimately differs run to run
# (ASLR), so a byte diff would be a false alarm rather than a finding.
PRINTS_ADDRESSES="test_crash2 test_gcscan test-oisc test_printf"
# test_timekeeping does not terminate under a plain interpreter.
SKIP="test_timekeeping"
good=0
for f in src/tests/*.c; do
    n=$(basename "$f" .c)
    case " $SKIP " in *" $n "*) continue ;; esac
    a=$(timeout 60 $C4M "$f" 2>&1); ra=$?
    [ $ra -eq 124 ] && continue
    b=$(timeout 180 $MPG "$f" 2>&1); rb=$?
    if echo "$b" | grep -q "^c4mpg:"; then
        echo "test-mpg: FALSE POSITIVE on $n"
        echo "$b" | grep -A4 "^c4mpg:" | head -6 | sed 's/^/    /'
        fail=1
        continue
    fi
    case " $PRINTS_ADDRESSES " in *" $n "*) good=$((good+1)); continue ;; esac
    if [ "$a" = "$b" ] && [ "$ra" = "$rb" ]; then
        good=$((good+1))
    else
        echo "test-mpg: $n differs under the guard (c4m rc=$ra, c4mpg rc=$rb)"
        diff <(echo "$a") <(echo "$b") | head -6 | sed 's/^/    /'
        fail=1
    fi
done
echo "test-mpg: $good corpus programs run identically under the guard"

if [ $fail = 0 ]; then echo "test-mpg: OK"; else echo "test-mpg: FAILED"; fi
exit $fail
