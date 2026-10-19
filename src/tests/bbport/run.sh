#!/bin/bash
# The Busybox port's compiler probes (docs/busybox-port.md, "The compiler").
# Each .c here is one construct Busybox's source uses. gcc is the oracle:
# c4lc must compile the probe and print what gcc's build prints.
#
#   bash src/tests/bbport/run.sh [probe ...]     (from the repository root)
#
# It prints PASS, DIFF (compiles, wrong output) or CFAIL (does not compile)
# per probe, then a count. It is a tracker, not a gate: it exits 1 while any
# probe fails, and no make target runs it yet.
cd "$(dirname "$0")/../../.." || exit 2
D=src/tests/bbport
T=$(mktemp -d)
trap 'rm -rf $T' EXIT
if [ $# -gt 0 ]; then set -- $(for p; do echo $D/$p.c; done); else set -- $D/*.c; fi
pass=0; total=0
for f; do
  n=$(basename $f .c); total=$((total + 1))
  if gcc -w -std=gnu99 -include stdio.h -include stdlib.h -include string.h -include setjmp.h \
       $f -o $T/$n.gcc 2>/dev/null; then want=$($T/$n.gcc); else want="(gcc failed)"; fi
  if timeout 120 ./c4sp -c 8000000 src/c4sp/lisp/c4lc.lisp -O -conforming -P -I src/c4ix/libc/include \
       $f $T/$n.c4r > $T/$n.out 2>&1 && [ -s $T/$n.c4r ]; then
    got=$(timeout 60 ./c4m load-c4r.c -- $T/$n.c4r 2>&1 | head -5)
    if [ "$got" = "$want" ]; then echo "PASS  $n"; pass=$((pass + 1))
    else echo "DIFF  $n | want: $want | got: $(echo $got | cut -c1-100)"; fi
  else
    echo "CFAIL $n | $(grep -i error $T/$n.out | tail -1 | sed 's/^c4sp: error: //' | cut -c1-110)"
  fi
done
echo "bbport: $pass of $total probes pass"
[ $pass = $total ]
