#!/bin/bash
# common-link.sh -- a global several units define is one object
# (c4rlink.c, common_plan; docs/c4lc-design.md, "Globals defined twice")
#
# common_a.c, common_b.c and common_c.c define counter, table and limit
# more than once, with and without initializers, and common_a.c has a
# static whose name common_b.c gives a global, which common_c.c names:
# linked in any order, and with B and C as a -r library, the image must
# print what gcc -fcommon makes of it, with no warning, and --warn-common
# must name the three it merged. common_d.c initializes limit a second
# time: that link must stop. In one unit, c4lc_tentative.c's twice-defined
# globals must each be one symbol, and c4lc_redef.c must not compile.
# At 64 and 32 bits, plain and -O.
#
#   bash src/c4sp/tests/common-link.sh
cd "$(dirname "$0")/../../.." || exit 1
[ -f src/c4sp/tests/common-link.sh ] || { echo "common-link: not in c4's tree"; exit 1; }
T=.common_link
WANT="common 111 4 42 5 7 7111"
rm -rf $T; mkdir -p $T
fail=0
bad () { echo "FAIL common-link: $*"; fail=$((fail + 1)); }
run () {
  local out
  out=$(./c4m$1 load-c4r.c -- $2 2>&1)
  [ "$out" = "$WANT" ] || bad "$3 prints '$out'"
}

for w in "" 32; do
  lc="./c4sp$w -c 400000 src/c4sp/lisp/c4lc.lisp"
  for o in "" -O; do
    k="($w$o)"
    for u in a b c d; do
      $lc $o -c src/tests/common_$u.c $T/$u.c4o > /dev/null || bad "common_$u.c -c $k"
    done
    ./c4rlink$w $T/a.c4o $T/b.c4o $T/c.c4o -o $T/abc.c4r > $T/log 2>&1 || bad "a b c $k: $(cat $T/log)"
    grep -qi "warn" $T/log && bad "a b c $k warns: $(cat $T/log)"
    run "$w" $T/abc.c4r "a b c $k"
    ./c4rlink$w $T/b.c4o $T/c.c4o $T/a.c4o -o $T/bca.c4r > /dev/null 2>&1; run "$w" $T/bca.c4r "b c a $k"
    ./c4rlink$w $T/c.c4o $T/a.c4o $T/b.c4o -o $T/cab.c4r > /dev/null 2>&1; run "$w" $T/cab.c4r "c a b $k"
    ./c4rlink$w -r $T/b.c4o $T/c.c4o -o $T/bc.c4l > /dev/null 2>&1
    ./c4rlink$w $T/a.c4o $T/bc.c4l -o $T/lib.c4r > /dev/null 2>&1; run "$w" $T/lib.c4r "a with a -r library of b c $k"
    n=$(./c4rlink$w --warn-common $T/a.c4o $T/b.c4o $T/c.c4o -o $T/abc.c4r 2>&1 | grep -c "^c4rlink: common ")
    [ "$n" = 3 ] || bad "--warn-common $k names $n globals, not 3"
    if ./c4rlink$w $T/a.c4o $T/b.c4o $T/c.c4o $T/d.c4o -o $T/abcd.c4r > $T/log 2>&1; then
      bad "a b c d $k links: limit is initialized twice"
    fi
    grep -q "multiple definition of 'limit'" $T/log || bad "a b c d $k: no multiple definition of limit: $(cat $T/log)"
    # one unit
    $lc $o -c src/tests/c4lc_tentative.c $T/t.c4o > /dev/null || bad "c4lc_tentative.c -c $k"
    if [ -z "$w" ]; then
      for v in x y z arr; do
        [ "$(./c4rdump -s $T/t.c4o | grep -c " '$v':")" = 1 ] || bad "c4lc_tentative.c -c $k: $v is not one symbol"
      done
    fi
    $lc $o src/tests/c4lc_tentative.c $T/t.c4r > /dev/null || bad "c4lc_tentative.c $k"
    [ "$(./c4m$w load-c4r.c -- $T/t.c4r 2>&1)" = "tentative 2 3 4 9 2" ] || bad "c4lc_tentative.c $k does not run"
    if $lc $o -c src/tests/c4lc_redef.c $T/r.c4o > $T/log 2>&1; then
      bad "c4lc_redef.c $k compiles"
    fi
    grep -q "redefinition of r" $T/log || bad "c4lc_redef.c $k: no redefinition of r: $(cat $T/log)"
  done
done

rm -rf $T
[ $fail = 0 ] && echo "common-link: OK"
[ $fail = 0 ]
