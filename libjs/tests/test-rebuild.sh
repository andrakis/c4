#!/bin/bash
# test-rebuild.sh - C4IX rebuilds what it ships, inside itself (`make
# test-c4ix-rebuild`). Boots the libjs disk, runs `b4ke -B -C /usr`
# (-B: the root archive's /build is already up to date)
# -- the kernel, both libraries, every userland program, the desktop and
# the c4cc-built toolchain -- then checks each result against the image
# the host built:
#
#   - everything c4lc builds (through c4sc) must be byte for byte the
#     disk's: the kernel, the libraries, every c4ix-*.c4r;
#   - c4cc's images write its own heap addresses into their symbol
#     records, so they only have to be the same SIZE as the host's --
#     and the rebuilt c4cc has to compile a program that runs.
#
# Then a second b4ke run must find everything up to date.
# About nine minutes under node; not in test-libjs.
set -u
cd "$(dirname "$0")/../.."
IMAGES=src/c4bb/images
LIBJS="node libjs/cli.js"
B=/build
fail=0
ok()  { echo "test-rebuild: $1 OK"; }
bad() { echo "test-rebuild: $1 FAILED"; fail=1; }

LC_PROGS="hello uhello echo wc cat sh ps bench cycles ls mkdir top spin fmt gui vfsload b4ke cc tar gunzip crc32 splash persist desktop libctest pictest unixtest"
CC_PROGS="c4cc c4rdump c4rlink cpp c4 c4m"
{
    echo "wait 4"
    echo "b4ke -B -C /usr"
    for u in $LC_PROGS; do echo "crc32 $B/bin/c4ix-$u.c4r"; done
    for t in $CC_PROGS; do echo "crc32 $B/bin/$t.c4r"; done
    echo "crc32 $B/c4ix.c4r"
    echo "crc32 $B/lib/libc4ix.c4l"
    echo "crc32 $B/lib/libc4ix.c4s"
    echo "crc32 $B/lib/libc.c4s"
    echo "crc32 $B/lib/libarc.c4l"
    echo "crc32 $B/lib/libarc.c4s"
    echo "crc32 $B/lib/libgui.c4s"
    echo "$B/bin/c4cc.c4r -o /tmp/h.c4r /usr/src/c4ix/examples/hello.c"
    echo "/tmp/h.c4r"
    echo "b4ke -C /usr"
    echo "b4ke -C /usr -f INSTALL.B4K"
    echo "echo installed-ls-runs | wc"
    echo "exit"
} > .rebuild.cmds
out=$(timeout 1500 $LIBJS -m 64 -d $IMAGES/disk $IMAGES/c4ix32.c4r < .rebuild.cmds 2>&1)
rm -f .rebuild.cmds

# all built, and nothing failed or overran on the way (the second run
# would say "all built" too, after making what the first did not)
if echo "$out" | grep -qF "b4ke: all built, in /build" &&
   ! echo "$out" | grep -qE " [1-9][0-9]* failed|overran|did not make"; then ok "b4ke -C /usr builds everything"
else bad "b4ke -C /usr builds everything"; echo "$out" | grep -E 'failed|did not|cannot|overran' | head -10; fi

# the host's CRC of the file a build output corresponds to
host() { python3 -c 'import zlib,sys; d=open(sys.argv[1],"rb").read(); print("%08x %d" % (zlib.crc32(d) & 0xffffffff, len(d)))' "$1"; }
same=0; differ=""
check() {   # check NAME-IN-MACHINE HOST-FILE MODE(crc|size)
    got=$(echo "$out" | grep -E "^[0-9a-f]{8} [0-9]+ $1\$" | head -1 | cut -d' ' -f1,2)
    want=$(host "$2")
    if [ "$3" = size ]; then got=${got#* }; want=${want#* }; fi
    if [ -n "$got" ] && [ "$got" = "$want" ]; then same=$((same + 1)); else differ="$differ $1"; fi
}
for u in $LC_PROGS; do check $B/bin/c4ix-$u.c4r $IMAGES/disk/c4ix-$u.c4r crc; done
check $B/c4ix.c4r $IMAGES/c4ix32.c4r crc
check $B/lib/libc4ix.c4l $IMAGES/libc4ix32.c4l crc
check $B/lib/libc4ix.c4s $IMAGES/disk/c4ix-libc4ix.c4s crc
check $B/lib/libc.c4s $IMAGES/disk/c4ix-libc.c4s crc
check $B/lib/libarc.c4l $IMAGES/libarc32.c4l crc
check $B/lib/libarc.c4s $IMAGES/disk/c4ix-libarc.c4s crc
check $B/lib/libgui.c4s $IMAGES/disk/c4ix-libgui.c4s crc
if [ -z "$differ" ]; then ok "c4lc's $same images are the disk's, byte for byte"
else bad "c4lc's images are the disk's (differ:$differ)"; fi
same=0; differ=""
for t in $CC_PROGS; do check $B/bin/$t.c4r $IMAGES/disk/$t.c4r size; done
if [ -z "$differ" ]; then ok "c4cc's $same images are the disk's size"
else bad "c4cc's images are the disk's size (differ:$differ)"; fi

if echo "$out" | grep -qE "^[0-9a-f]{8} [0-9]+ $B/bin/c4cc.c4r$" && echo "$out" | grep -qF "hello from a program compiled inside C4IX"; then ok "the rebuilt c4cc compiles a program that runs"
else bad "the rebuilt c4cc compiles a program that runs"; fi
if echo "$out" | grep -qE "b4ke: /usr/src/c4ix/user: 0 ran, [0-9]+ up to date, 0 failed"; then
    ok "a second run finds everything up to date"
else bad "a second run finds everything up to date"; echo "$out" | grep 'up to date' | tail -6; fi

if echo "$out" | grep -qF "b4ke: installed in /bin and /usr/lib" && echo "$out" | grep -qF "18 bytes 1 lines"; then
    ok "INSTALL.B4K puts them in /bin, and they run"
else bad "INSTALL.B4K puts them in /bin, and they run"; fi

if [ $fail = 0 ]; then echo "test-rebuild: OK"; else echo "test-rebuild: FAILURES"; exit 1; fi
