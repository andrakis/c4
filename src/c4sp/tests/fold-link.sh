#!/bin/bash
# fold-link.sh -- c4rlink keeps one copy of c4lc's helpers (docs/link-fold.md)
#
# fold_a.c and fold_b.c each use unsigned division, 16-bit loads and
# stores, a logical shift, long long and double arithmetic, so c4lc
# splices its helpers for those into both objects, with no symbol.
# Linked, the image must run as with --no-fold, with less code, and with
# no function in its map twice where --no-fold has some twice -- but
# __c4fp_hw, which keeps the FPU's answer in statics of its object, and
# the NAME beside each NAME_s that asks it; each function and row of the
# map in the code. The objects compiled -g0 must link to the same image
# cut at the map (the fold does not look at the map). c4cc's build of
# c4rlink, run by B4KE under C4KE, must write an image of the same size,
# which runs. The same for a -r library and for -fpic objects, and for
# 32 bits, where c4rdump (64-bit only) does not look: smaller is all.
#
#   bash src/c4sp/tests/fold-link.sh
cd "$(dirname "$0")/../../.."
LC="./c4sp -c 400000 src/c4sp/lisp/c4lc.lisp"
LC32="./c4sp32 -c 400000 src/c4sp/lisp/c4lc.lisp"
T=.fold_link
WANT="fold 2863 3 -1234 5000 268435200 1024 -72 63"
rm -rf $T; mkdir -p $T
fail=0
bad () { echo "FAIL fold-link: $*"; fail=$((fail + 1)); }
codelen () { ./c4rdump -i "$1" | sed -n 's/.*Code = \([0-9]*\).*/\1/p'; }
# map functions named twice or more; with -x, not __c4fp_hw or what asks it
twice () {
  local x=0; [ "$1" = -x ] && { x=1; shift; }
  ./c4rdump -l "$1" | awk -v x=$x '
    /^  [0-9]+ (files|functions|rows)$/ { sec = $2; next }
    sec == "functions" { f = $3; sub(/,$/, "", f); n[f]++ }
    END { for (f in n) if (n[f] > 1 && !(x && (f == "__c4fp_hw" || (f "_s") in n))) print f }'
}
# every function and row in the code, the rows in order
inside () {
  ./c4rdump -l "$1" | awk -v len=$(codelen "$1") '
    /^  [0-9]+ (files|functions|rows)$/ { sec = $2; next }
    sec == "functions" && $1 + $2 > len { print "function " $3 " past the code"; bad = 1 }
    sec == "rows" { if ($1 >= len || $1 < prev) { print "row at " $1 " out of order or past the code"; bad = 1 } prev = $1 }
    END { exit bad }'
}
# one link: SUFFIX (32 or none), the image's name, the flags and objects
check () {
  local w=$1 img=$2; shift 2
  ./c4rlink$w "$@" -o $T/$img.c4r > /dev/null || { bad "$img: c4rlink$w"; return; }
  [ "$(./c4m$w load-c4r.c -- $T/$img.c4r 2>&1)" = "$WANT" ] || bad "$img does not run as fold_a.c does"
}

for w in "" 32; do
  lc=$LC; [ -n "$w" ] && lc=$LC32
  for u in a b; do
    $lc -g -c src/tests/fold_$u.c $T/$u$w.c4o > /dev/null || bad "fold_$u.c -g -c ($w)"
    $lc -g0 -c src/tests/fold_$u.c $T/$u$w.g0.c4o > /dev/null || bad "fold_$u.c -g0 -c ($w)"
  done
  check "$w" f$w $T/a$w.c4o $T/b$w.c4o
  check "$w" n$w --no-fold $T/a$w.c4o $T/b$w.c4o
  # the same code with -g0
  ./c4rlink$w $T/a$w.g0.c4o $T/b$w.g0.c4o -o $T/g0$w.c4r > /dev/null
  head -c $(stat -c %s $T/g0$w.c4r) $T/f$w.c4r | cmp -s - $T/g0$w.c4r || bad "f$w: -g0 objects fold otherwise"
  # a -r library, folded, linked into a program
  ./c4rlink$w -r $T/a$w.c4o $T/b$w.c4o -o $T/ab$w.c4l > /dev/null || bad "c4rlink$w -r"
  check "$w" l$w $T/ab$w.c4l
  # -fpic: a shared library's objects
  $lc -fpic -c src/tests/fold_a.c $T/pa$w.c4o > /dev/null && $lc -fpic -c src/tests/fold_b.c $T/pb$w.c4o > /dev/null || bad "-fpic -c ($w)"
  ./c4rlink$w -r $T/pa$w.c4o $T/pb$w.c4o -o $T/p$w.c4s > /dev/null || bad "c4rlink$w -r (-fpic)"
  ./c4rlink$w -r --no-fold $T/pa$w.c4o $T/pb$w.c4o -o $T/pn$w.c4s > /dev/null
  if [ -n "$w" ]; then
    [ $(stat -c %s $T/f$w.c4r) -lt $(stat -c %s $T/n$w.c4r) ] || bad "f$w: no smaller than --no-fold"
    [ $(stat -c %s $T/p$w.c4s) -lt $(stat -c %s $T/pn$w.c4s) ] || bad "p$w.c4s: -fpic objects not folded"
    continue
  fi
  [ "$(codelen $T/f.c4r)" -lt "$(codelen $T/n.c4r)" ] || bad "f: no less code than --no-fold"
  [ -n "$(twice -x $T/n.c4r)" ] || bad "n: no helper twice: the test does not test the fold"
  [ -z "$(twice -x $T/f.c4r)" ] || bad "f: still twice: $(twice -x $T/f.c4r | tr '\n' ' ')"
  inside $T/f.c4r || bad "f: the map is not in the code"
  [ -z "$(twice -x $T/ab.c4l)" ] || bad "ab.c4l: still twice"
  [ "$(codelen $T/p.c4s)" -lt "$(codelen $T/pn.c4s)" ] || bad "p.c4s: -fpic objects not folded"
done

# c4cc's build of c4rlink, in the VM, as test-b4ke runs it
printf 'B4KE1\nRUN c4rlink.c4r %s -o fold_vm.c4r\nRUN fold_vm.c4r\n' "$T/a.c4o $T/b.c4o" > $T/vm.b4k
cp src/c4ke/bin/b4ke.c4r .
./c4m load-c4r.c -- c4ke.c4r b4ke -f $T/vm.b4k > $T/vm.log 2>&1
grep -q "c4rlink: wrote $(stat -c %s $T/f.c4r) bytes to ramfs:fold_vm.c4r" $T/vm.log \
  || bad "c4rlink.c4r in the VM links another image than c4rlink"
grep -qx "$WANT" $T/vm.log || bad "the image c4rlink.c4r linked in the VM does not run"

rm -rf $T
[ $fail = 0 ] && echo "fold-link: OK"
[ $fail = 0 ]
