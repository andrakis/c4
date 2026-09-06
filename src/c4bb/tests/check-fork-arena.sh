#!/bin/bash
# check-fork-arena.sh - arena-whowrote.js must be arena.js plus marked blocks.
#
# The same discipline as src/tests/mpg/check-fork.sh, for the same
# reason: a fork rots, so make the drift fail the build instead of
# waiting to be noticed. Strip every block between '//>>> whowrote' and
# '//<<< whowrote' out of arena-whowrote.js and what remains must be
# arena.js, byte for byte.
#
# arena.js itself is not modified at all -- the whole point of forking
# rather than adding a flag is that c4bb's ordinary runs do not pay for
# a tool they are not using.
set -u
A=src/c4bb/sim/arena.js
B=src/c4bb/sim/arena-whowrote.js
tmp=$(mktemp); trap 'rm -f "$tmp" "$tmp".s "$tmp".d' EXIT

awk '
    /^[ \t]*\/\/>>> whowrote$/ { skip = 1; next }
    /^[ \t]*\/\/<<< whowrote$/ { skip = 0; next }
    skip == 0                  { print }
' "$B" > "$tmp".s

# The fork carries a header arena.js does not; everything from the
# first line the two share onwards is what must match.
first=$(head -1 "$A")
awk -v first="$first" 'found { print } $0 == first && !found { found = 1; print }' \
    "$tmp".s > "$tmp"

if diff -u "$A" "$tmp" > "$tmp".d; then
    echo "check-fork-arena: arena-whowrote.js is arena.js plus its marked blocks OK"
    exit 0
fi
echo "check-fork-arena: arena-whowrote.js HAS DRIFTED FROM arena.js"
echo "check-fork-arena: (left = arena.js, right = the fork with its marked blocks removed)"
head -50 "$tmp".d | sed 's/^/    /'
exit 1
