#!/bin/bash
# test-oisc4.sh - compare OISC4 output against c4m's loader, bit for bit.
# Run from the repo root (make test-oisc4).
#
# c4m prints trap chatter when a bare (kernel-less) image probes for
# custom opcodes; that noise is filtered before comparing.
#
# Out of contract, so not tested here (see docs/oisc4-design.md):
#   test_customop test_signal test_timekeeping  - need real trap machinery
#   fun_with_ptrs                               - segfaults c4m/c4l themselves
#   c4m.c4r                                     - C4 jailbreak (self-modifying)

OISC4=./src/oisc4/oisc4
C4M="./c4m load-c4r.c --"
FILTER='/^Trap type/d; /missed a trap/d'
fail=0

# render times differ between VMs; compare the text, not the milliseconds
NORM='s/rendered in [0-9]*ms/rendered in Xms/'

check () {
    name="$1"; shift
    a=$(timeout 300 $OISC4 "$@" 2>&1 | sed "$NORM")
    b=$(timeout 300 $C4M "$@" 2>&1 | sed "$FILTER" | sed "$NORM")
    if [ "$a" = "$b" ]; then
        echo "test-oisc4: $name OK"
    else
        echo "test-oisc4: $name FAILED"
        diff <(echo "$a") <(echo "$b") | head -10
        fail=1
    fi
}

grepcheck () {
    name="$1"; want="$2"; shift 2
    if timeout 300 $OISC4 "$@" 2>&1 | grep -q "$want"; then
        echo "test-oisc4: $name OK"
    else
        echo "test-oisc4: $name FAILED (no '$want')"
        fail=1
    fi
}

for t in hello factorial multifun test-order test-ptrs test_basic \
         test_malloc test_static test_continue mandel test_args test_exit \
         cycles test_fread test_printloop test_crash test_vprintf test_float \
         tests; do
    check "$t" src/tests/$t.c4r
done

# %p prints arena offsets under OISC4 (documented), so compare filtered
a=$(timeout 300 $OISC4 src/tests/test_printf.c4r 2>&1 | sed '/an int ptr/d')
b=$(timeout 300 $C4M src/tests/test_printf.c4r 2>&1 | sed "$FILTER" | sed '/an int ptr/d')
if [ "$a" = "$b" ]; then echo "test-oisc4: test_printf OK"; else echo "test-oisc4: test_printf FAILED"; fail=1; fi

# The acid tests: self-hosted compiler and the Lisp interpreter inside OISC
grepcheck "c4-in-oisc4"   "yello"               c4.c4r src/tests/hello.c
grepcheck "c4sp-in-oisc4" "roundtrip identical" -m 256 c4sp.c4r src/c4sp/lisp/c4r-roundtrip.lisp src/tests/hello.c4r

if [ $fail = 0 ]; then echo "test-oisc4: OK"; else echo "test-oisc4: FAILURES"; exit 1; fi
