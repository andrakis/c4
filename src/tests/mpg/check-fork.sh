#!/bin/bash
# check-fork.sh - c4mpg.c must be c4m.c plus marked blocks, and nothing else.
#
# c4mpg is a fork because it has to be: c4m.c is read as SOURCE at run
# time by a plain c4 with no preprocessor, which compiles what is
# between the '#' lines it skips -- so an '#ifdef C4MPG' in c4m.c is
# unconditional code in every nested interpreter innerbench starts.
# See the header of c4mpg.c.
#
# A fork rots. This is the thing that stops it rotting silently: strip
# every block between '//>>> c4mpg' and '//<<< c4mpg' out of c4mpg.c and
# what remains must be c4m.c, byte for byte. Change c4m.c and this says
# exactly which lines have gone out of step; it never guesses, and it
# never merges for you.
set -u
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

# Drop the file header (everything before the first line c4m.c also has,
# which is its own opening comment marker) and the marked blocks.
awk '
    /^\/\/>>> c4mpg$/          { skip = 1; next }
    /^[ \t]*\/\/>>> c4mpg$/    { skip = 1; next }
    /^[ \t]*\/\/<<< c4mpg$/    { skip = 0; next }
    skip == 0                  { print }
' c4mpg.c > "$tmp".stripped

# c4mpg.c carries a header c4m.c does not. Everything from the first
# line the two share onwards is what must match.
first=$(head -1 c4m.c)
awk -v first="$first" 'found { print } $0 == first && !found { found = 1; print }' \
    "$tmp".stripped > "$tmp"

if diff -u c4m.c "$tmp" > "$tmp".diff; then
    echo "check-fork: c4mpg.c is c4m.c plus its marked blocks OK"
    rm -f "$tmp".stripped "$tmp".diff
    exit 0
fi
echo "check-fork: c4mpg.c HAS DRIFTED FROM c4m.c"
echo "check-fork: (left = c4m.c, right = c4mpg.c with its marked blocks removed)"
head -60 "$tmp".diff | sed 's/^/    /'
rm -f "$tmp".stripped "$tmp".diff
exit 1
