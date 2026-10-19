#!/bin/sh
# m4-volatile.sh C4RDUMP IMAGE -- the DATA references to vcount and ncount
# in IMAGE, as c4rdump's patch listing shows them (src/tests/c4lc_m4v.c).
# Prints "vcount N ncount M".
#
# A global's references are its DATA patches: every IMM of its address
# carries one, whether a load, a store or an &. The symbol listing gives
# the global's data offset and the patch listing gives each reference's.
d=$("$1" -sp "$2") || exit 1
count() {
	off=$(printf '%s\n' "$d" | sed -n "s/.*symbol [0-9]* '$1': .* value [0-9]* (\(0x[0-9a-f]*\)).*/\1/p")
	[ -n "$off" ] || { echo "m4-volatile: no symbol $1" >&2; exit 1; }
	printf '%s\n' "$d" | grep -c "patch type DATA .* value $off\$"
}
echo "vcount $(count vcount) ncount $(count ncount)"
