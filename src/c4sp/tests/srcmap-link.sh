#!/bin/bash
# srcmap-link.sh -- track S2 (docs/c4-explorer.md 2.4, 6): source maps
# through c4rlink and c4opt-run.
#
# 1. c4rlink: srcmap_probe.c and srcmap_link.c compiled -g -c, with
#    test_link_b.c compiled -g0 between them. The linked map must be the
#    objects' maps rebased: the files unioned by path (srcmap_probe.h,
#    which both mapped objects include, once), each function and row
#    moved by its object's code offset, and one row of line 0 where the
#    object with no map starts. It is worked out here from c4rdump -l of
#    each object. c4cc's build of c4rlink, run by B4KE under C4KE (the
#    VM has no write but the kernel's RAM filesystem's), must write an
#    image of the same size that runs, and a 32-bit link the same map (a
#    map counts words, so it does not depend on the word size).
#    -s and -g0 leave no map, and -g0 is the image of objects with none.
# 2. c4opt-run: on an image compiled -g, and on the linked one, it writes
#    the code it writes for the image without the map, and the map it
#    writes has every row at an instruction, at a line the image's own
#    map had.
#
#   bash src/c4sp/tests/srcmap-link.sh
cd "$(dirname "$0")/../../.."
LC="./c4sp -c 400000 src/c4sp/lisp/c4lc.lisp"
LC32="./c4sp32 -c 400000 src/c4sp/lisp/c4lc.lisp"
T=.srcmap_link
rm -rf $T; mkdir -p $T
fail=0
bad () { echo "FAIL srcmap-link: $*"; fail=$((fail + 1)); }
codelen () { ./c4rdump -i "$1" | sed -n 's/.*Code = \([0-9]*\).*/\1/p'; }
maplen () { ./c4rdump -l "$1" | sed -n 's/^Source map: version 1, \([0-9]*\) bytes/\1/p'; }
# the map's payload out of an image's trailer: 'X', tag, length, the
# payload and its padding to a word (W bytes), a 0 tag
payload () { local f=$1 w=$2 n=$3; tail -c $((w + (w - n % w) % w + n)) "$f" | head -c $n; }

# ---- 1. c4rlink ----
# Every link here is --no-gc: the maps checked are the objects' maps
# whole, and c4rlink drops the functions nothing reaches by default.
$LC -g -c -I src/tests src/tests/srcmap_probe.c $T/p.c4o > /dev/null || bad "srcmap_probe.c -g -c"
$LC -g0 -c src/tests/test_link_b.c $T/b.c4o > /dev/null || bad "test_link_b.c -g0 -c"
$LC -g -c -I src/tests src/tests/srcmap_link.c $T/l.c4o > /dev/null || bad "srcmap_link.c -g -c"
./c4rlink --no-gc $T/p.c4o $T/b.c4o $T/l.c4o -o $T/pbl.c4r > /dev/null || bad "c4rlink"
./c4m load-c4r.c -- $T/pbl.c4r 2>&1 | grep -qx "66 28 27" || bad "the linked image does not run as srcmap_probe does"

# what the linked map must be
for o in p b l; do
  echo "OBJ $(codelen $T/$o.c4o)"
  ./c4rdump -l $T/$o.c4o
done | awk '
  BEGIN { nf = 0; nu = 0; nr = 0; next_base = 0; rf = 0 }
  function row(off, f, l) { rows[nr++] = sprintf("    %6d %d:%d", off, f, l); rf = f }
  function done() { if (mapped && first && clen) row(base, rf, 0) }
  /^OBJ / { done(); base = next_base; clen = $2; next_base += clen; mapped = 0; next }
  /^No source map/ { if (clen) row(base, rf, 0); next }
  /^Source map:/ { mapped = 1; first = 1; next }
  /^  [0-9]+ (files|functions|rows)$/ { sec = $2; next }
  sec == "files" {
    p = $2; sub(/,$/, "", p)
    if (!(p in fidx)) { fidx[p] = nf; files[nf] = sprintf("    %d %s, %s bytes, crc %s", nf, p, $3, $6); nf++ }
    fm[$1] = fidx[p]; next }
  sec == "functions" { funcs[nu++] = sprintf("    %6d %5d %s file %d line %d", base + $1, $2, $3, fm[$5], $7); next }
  sec == "rows" {
    split($2, a, ":")
    if (first && $1 > 0 && clen) row(base, rf, 0)
    first = 0
    if ($1 < clen) row(base + $1, fm[a[1]], a[2])
    next }
  END {
    done()
    printf "  %d files\n", nf;     for (i = 0; i < nf; i++) print files[i]
    printf "  %d functions\n", nu; for (i = 0; i < nu; i++) print funcs[i]
    printf "  %d rows\n", nr;      for (i = 0; i < nr; i++) print rows[i]
  }' > $T/want.txt
./c4rdump -l $T/pbl.c4r | tail -n +2 > $T/got.txt
if ! cmp -s $T/want.txt $T/got.txt; then bad "the linked map is not its objects' maps, rebased"; diff $T/want.txt $T/got.txt | head -20; fi
[ "$(grep -c 'srcmap_probe.h' $T/got.txt)" = 1 ] || bad "the header is not in the linked file table once"
grep -q ' 0:0$' $T/got.txt || bad "no line-0 row where the object without a map starts"

# c4cc's build of c4rlink, in the VM, as test-b4ke runs it
printf 'B4KE1\nRUN c4rlink.c4r --no-gc %s -o srcmap_vm.c4r\nRUN srcmap_vm.c4r\n' "$T/p.c4o $T/b.c4o $T/l.c4o" > $T/vm.b4k
cp src/c4ke/bin/b4ke.c4r .
./c4m load-c4r.c -- c4ke.c4r b4ke -f $T/vm.b4k > $T/vm.log 2>&1
grep -q "c4rlink: wrote $(stat -c %s $T/pbl.c4r) bytes to ramfs:srcmap_vm.c4r" $T/vm.log \
  || bad "c4rlink.c4r in the VM links another image than c4rlink"
grep -qx "66 28 27" $T/vm.log || bad "the image c4rlink.c4r linked in the VM does not run"

# 32 bits: the same map
$LC32 -g -c -I src/tests src/tests/srcmap_probe.c $T/p32.c4o > /dev/null || bad "32-bit srcmap_probe.c"
$LC32 -g0 -c src/tests/test_link_b.c $T/b32.c4o > /dev/null || bad "32-bit test_link_b.c"
$LC32 -g -c -I src/tests src/tests/srcmap_link.c $T/l32.c4o > /dev/null || bad "32-bit srcmap_link.c"
./c4rlink32 --no-gc $T/p32.c4o $T/b32.c4o $T/l32.c4o -o $T/pbl32.c4r > /dev/null || bad "c4rlink32"
./c4m32 load-c4r.c -- $T/pbl32.c4r 2>&1 | grep -qx "66 28 27" || bad "the 32-bit linked image does not run"
n=$(maplen $T/pbl.c4r)
payload $T/pbl.c4r 8 $n > $T/map64
payload $T/pbl32.c4r 4 $n > $T/map32
cmp -s $T/map64 $T/map32 || bad "the 32-bit link wrote another map"

# -s and -g0
./c4rlink --no-gc -s $T/p.c4o $T/b.c4o $T/l.c4o -o $T/s.c4r > /dev/null
./c4rdump -l $T/s.c4r | grep -q "No source map" || bad "-s kept the map"
./c4rlink --no-gc -g0 $T/p.c4o $T/b.c4o $T/l.c4o -o $T/g0.c4r > /dev/null
./c4rdump -l $T/g0.c4r | grep -q "No source map" || bad "-g0 kept the map"
./c4rdump -s $T/g0.c4r | grep -q "'link_sum'" || bad "-g0 dropped the symbols"
$LC -g0 -c -I src/tests src/tests/srcmap_probe.c $T/p0.c4o > /dev/null
$LC -g0 -c -I src/tests src/tests/srcmap_link.c $T/l0.c4o > /dev/null
./c4rlink --no-gc $T/p0.c4o $T/b.c4o $T/l0.c4o -o $T/pbl0.c4r > /dev/null
cmp -s $T/g0.c4r $T/pbl0.c4r || bad "-g0 is not the image of the objects without maps"
head -c $(stat -c %s $T/pbl0.c4r) $T/pbl.c4r | cmp -s - $T/pbl0.c4r || bad "the map changed the linked image"

# ---- 2. c4opt-run ----
# Every row at an instruction (an offset c4rdump -c lists) and at a line
# (file path and line) that the map had; the functions keep their names,
# and as many rows of line 0 are outside every function as were (the
# code of a linked object that had no map stays out of the function
# before it).
rows_ok () {   # optimized-image original-map-dump
  local img=$1 orig=$2
  ./c4rdump -c $img | sed -n 's/^ *\([0-9]*\): .*/\1/p' > $T/insns
  ./c4rdump -l $img > $T/optmap
  awk '
    FILENAME == ARGV[1] { insn[$1] = 1; next }
    /^Source map/ { sec = ""; next }
    /^  [0-9]+ (files|functions|rows)$/ { sec = $2; next }
    sec == "files" { p = $2; sub(/,$/, "", p); path[FILENAME, $1] = p; next }
    sec == "functions" { fn[FILENAME, $3]++; k = nfn[FILENAME]++; fs[FILENAME, k] = $1; fe[FILENAME, k] = $1 + $2; next }
    sec == "rows" {
      split($2, a, ":")
      if (a[2] == 0) {
        inside = 0
        for (i = 0; i < nfn[FILENAME]; i++) if ($1 >= fs[FILENAME, i] && $1 < fe[FILENAME, i]) inside = 1
        if (!inside) out0[FILENAME]++
      }
      key = path[FILENAME, a[1]] ":" a[2]
      if (FILENAME == ARGV[2]) had[key] = 1
      else {
        if (!($1 in insn)) { print "row at " $1 " is inside an instruction"; bad = 1 }
        if (!(key in had)) { print "row at " $1 " names " key ", which the map did not have"; bad = 1 }
        nrows++
      }
      next }
    END {
      if (!nrows) { print "no rows"; bad = 1 }
      for (k in fn) { split(k, b, SUBSEP); if (b[1] == ARGV[3] && fn[ARGV[2], b[2]] != fn[k]) { print "function " b[2] " is lost or doubled"; bad = 1 } }
      if (nfn[ARGV[2]] != nfn[ARGV[3]]) { print "functions: " nfn[ARGV[2]] " -> " nfn[ARGV[3]]; bad = 1 }
      if (out0[ARGV[2]] + 0 != out0[ARGV[3]] + 0) { print "rows of line 0 outside the functions: " out0[ARGV[2]] + 0 " -> " out0[ARGV[3]] + 0; bad = 1 }
      exit bad }' $T/insns $orig $T/optmap
}
opt_pair () {   # name image-g image-g0
  local name=$1 g=$2 g0=$3
  ./c4sp src/c4sp/lisp/c4opt-run.lisp $g $T/og.c4r > /dev/null || { bad "$name: c4opt-run"; return; }
  ./c4sp src/c4sp/lisp/c4opt-run.lisp $g0 $T/o0.c4r > /dev/null
  local so=$(stat -c %s $T/o0.c4r)
  if ! head -c $so $T/og.c4r | cmp -s - $T/o0.c4r || [ $(stat -c %s $T/og.c4r) -le $so ]; then
    bad "$name: c4opt-run optimised the mapped image differently, or dropped its map"; return
  fi
  ./c4rdump -l $g > $T/origmap
  rows_ok $T/og.c4r $T/origmap > $T/rows.log || { bad "$name: c4opt-run's map"; head -5 $T/rows.log; }
  ./c4m load-c4r.c -- $g > $T/run_a 2>&1
  ./c4m load-c4r.c -- $T/og.c4r > $T/run_b 2>&1
  cmp -s $T/run_a $T/run_b || bad "$name: the optimised image runs differently"
}
for t in srcmap_probe c4lc_l2; do
  $LC -g -I src/tests src/tests/$t.c $T/$t.g.c4r > /dev/null || bad "$t.c -g"
  $LC -g0 -I src/tests src/tests/$t.c $T/$t.c4r > /dev/null
  opt_pair $t $T/$t.g.c4r $T/$t.c4r
done
opt_pair linked $T/pbl.c4r $T/pbl0.c4r

rm -rf $T
[ $fail = 0 ] && echo "srcmap-link: OK"
[ $fail = 0 ]
