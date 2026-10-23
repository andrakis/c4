#!/bin/bash
# srcmap-invariant.sh -- track S's invariant (docs/c4-explorer.md 2.3):
# c4lc -g changes no byte of the image, only adds the trailer after it.
#
# Every src/tests/*.c that c4lc compiles, plainly, with -O and with
# -O -mfuse -minline; then C4IX's kernel modules, as build-images.sh
# compiles them, preprocessed by c4lc itself. Each pair, -g0 and -g, must
# agree on every byte -g0 wrote, and -g must have written more.
#
#   bash src/c4sp/tests/srcmap-invariant.sh [-q]
cd "$(dirname "$0")/../../.."
LC="./c4sp32 -c 400000 src/c4sp/lisp/c4lc.lisp"
T=.srcmap_inv
rm -rf $T; mkdir -p $T
n=0; skipped=0; fail=0

pair () {   # name flags... file
  local name=$1; shift
  local src=${@: -1}
  local flags=("${@:1:$#-1}")
  if ! $LC "${flags[@]}" "$src" $T/a.c4r > $T/a.log 2>&1; then skipped=$((skipped + 1)); return; fi
  if ! $LC -g "${flags[@]}" "$src" $T/b.c4r > $T/b.log 2>&1; then
    echo "FAIL $name ${flags[*]}: -g did not compile"; tail -2 $T/b.log; fail=$((fail + 1)); return
  fi
  local sa=$(stat -c %s $T/a.c4r) sb=$(stat -c %s $T/b.c4r)
  if ! head -c $sa $T/b.c4r | cmp -s - $T/a.c4r || [ $sb -le $sa ]; then
    echo "FAIL $name ${flags[*]}: -g changed the image ($sa -> $sb bytes)"; fail=$((fail + 1)); return
  fi
  n=$((n + 1))
}

for f in src/tests/*.c; do
  b=$(basename $f .c)
  pair $b $f
  pair $b -O $f
  pair $b -O -mfuse -minline $f
done
# preprocessed by c4lc, with includes: locations that carry their file
for m in $(sed -n 's/^C4IX_MODS="\(.*\)"/\1/p' src/c4bb/tests/build-images.sh); do
  pair c4ix/$m -O -c -I src/c4ix src/c4ix/$m.c
done
for u in sh ls cat; do
  [ -f src/c4ix/user/$u.c ] && pair c4ix/user/$u -O -c -I src/c4ix/include src/c4ix/user/$u.c
done

rm -rf $T
echo "srcmap-invariant: $n pairs equal, $skipped not compiled by c4lc, $fail failed"
[ $fail = 0 ]
