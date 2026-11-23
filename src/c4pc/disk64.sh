#!/bin/bash
# disk64.sh -- the 64-bit C4IX's disk (make .c4pc/disk64.tar; docs/c4pc-64.md).
#
# The counterpart of the C4IX part of src/c4bb/tests/build-images.sh, which
# makes the 32-bit disk with lc32: the same kernel, libraries, programs and
# desktop, compiled by lc64 (c4sc-host, c4lc compiled for the host, whose
# images are 64-bit) and linked by the 64-bit c4rlink, for the 64-bit
# machine (c4pc-host64w, c4pcw64.elf). Stage one: no games (they stay on
# the 32-bit system), no MMU, no accelerators; and no /build in the root
# archive, so b4ke inside makes everything the first time.
#
#   OUT=dir   where the disk is made (default .c4pc/disk64), as a folder;
#             the Makefile tars it into .c4pc/disk64.tar
#   JOBS=n    compiles at once (default 3)
set -eu
OUT=${OUT:-.c4pc/disk64}
T=.c4pc/d64                       # objects
JOBS=${JOBS:-3}
G=${IMG_G--g}
# lc64 as the Makefile has it (LC64_BIN = c4sc-host): IN OUT, flags before
LC="./c4sc-host compile -O $G"
LINK=./c4rlink
say () { echo "disk64: $*"; }
rm -rf "$OUT" "$T"
mkdir -p "$OUT" "$T"

# compile each NAME of a list: par "FLAGS" "OBJPREFIX" "SRCDIR" names...
# (SRCDIR/NAME.c -> $T/OBJPREFIXNAME.c4o, JOBS at once)
par () {
    local flags=$1 pre=$2 dir=$3; shift 3
    printf '%s\n' "$@" | xargs -P "$JOBS" -I{} sh -c \
        "$LC $flags $dir/{}.c $T/$pre{}.c4o > $T/$pre{}.log 2>&1 || { echo 'disk64: {}.c did not compile:'; tail -5 $T/$pre{}.log; exit 1; }"
}

# ---- the kernel: make's c4ix.c4r (-D C4IX_LFS, which on this word is free)
say "the kernel (make's c4ix.c4r)"
cp c4ix.c4r "$OUT/c4ix.c4r"

# ---- the libraries -------------------------------------------------
# static, for what runs before /lib is there (splash, vfsload)
say "libraries: libc4ix and libarc, static and shared; libgui; libc"
$LC -c -I src/c4ix/include src/c4ix/lib/libc4ix.c $T/libc4ix.c4o > /dev/null
$LINK -r $T/libc4ix.c4o -o $T/libc4ix.c4l > /dev/null
# libarc -mfuse, as src/c4ix/lib/BUILD.B4K and build-images.sh have it:
# unpacking the root archive is a boot's one long job, and c4pc runs fused code
$LC -mfuse -c -I src/c4ix/include src/c4ix/lib/archive.c $T/libarc.c4o > /dev/null
$LINK -r $T/libarc.c4o -o $T/libarc.c4l > /dev/null
# shared: -fpic, loaded once for every task (c4rlink -l), as on the 32-bit disk
PIC="-fpic -c -D C4IX_SHARED -I src/c4ix/include"
$LC $PIC src/c4ix/lib/libc4ix.c $T/s_libc4ix.c4o > /dev/null
$LC $PIC src/c4ix/include/window.c $T/s_window.c4o > /dev/null
$LC $PIC src/c4ix/include/cpustat.c $T/s_cpustat.c4o > /dev/null
mkdir -p $T/lib
$LINK -r $T/s_libc4ix.c4o $T/s_window.c4o $T/s_cpustat.c4o -o $T/lib/libc4ix.c4s > /dev/null
$LC $PIC -mfuse src/c4ix/lib/archive.c $T/s_arc.c4o > /dev/null
$LINK -r $T/s_arc.c4o -o $T/lib/libarc.c4s > /dev/null
$LC $PIC -I libjs/guest libjs/guest/gui.c $T/s_gui.c4o > /dev/null
$LC $PIC -I libjs/guest libjs/guest/snd.c $T/s_snd.c4o > /dev/null
$LINK -r $T/s_gui.c4o $T/s_snd.c4o -o $T/lib/libgui.c4s > /dev/null
par "-fno-builtin -fpic -c -I src/c4ix/libc/include -I src/c4ix/libc" libc_ src/c4ix/libc string stdlib stdio unix fp math
$LC -fpic -c src/c4ix/libc/opcodes.c $T/libc_opcodes.c4o > /dev/null
$LC -fpic -c -I src/c4ix/include src/c4ix/lib/libc4ix.c $T/libc_u.c4o > /dev/null
$LINK -r $T/libc_string.c4o $T/libc_stdlib.c4o $T/libc_stdio.c4o $T/libc_unix.c4o \
    $T/libc_fp.c4o $T/libc_math.c4o $T/libc_opcodes.c4o $T/libc_u.c4o -o $T/lib/libc.c4s > /dev/null
for m in libc4ix libarc libgui libc; do cp $T/lib/$m.c4s "$OUT/c4ix-$m.c4s"; done

# ---- the programs (build-images.sh's C4IX_USER and C4IX_ARC_USER) ----
C4IX_USER="hello uhello echo wc cat sh ps bench cycles ls mkdir top spin pin nice fmt gui b4ke cc dmesg free mount umount df cp mkfs chmod shutdown reboot install"
C4IX_ARC_USER="tar gunzip crc32 splash persist"
say "programs: $C4IX_USER $C4IX_ARC_USER"
par "-c -D C4IX_SHARED -I src/c4ix/include" u_ src/c4ix/user $C4IX_USER $(echo $C4IX_ARC_USER | sed 's/splash//')
$LC -c -I src/c4ix/include src/c4ix/user/splash.c $T/u_splash.c4o > /dev/null
for u in $C4IX_USER; do
    if [ $u = gui ]; then libs="-l $T/lib/libgui.c4s -l $T/lib/libc4ix.c4s"; else libs="-l $T/lib/libc4ix.c4s"; fi
    $LINK $T/u_$u.c4o $libs -o "$OUT/c4ix-$u.c4r" > /dev/null
done
for u in $C4IX_ARC_USER; do
    # splash runs while the boot unpacks the root archive, before anything
    # else may have been: it carries its libraries itself
    if [ $u = splash ]; then $LINK $T/u_$u.c4o $T/libarc.c4l $T/libc4ix.c4l -o "$OUT/c4ix-$u.c4r" > /dev/null
    else $LINK $T/u_$u.c4o -l $T/lib/libarc.c4s -l $T/lib/libc4ix.c4s -o "$OUT/c4ix-$u.c4r" > /dev/null; fi
done
# the VFS loader, static (it lays out /lib)
$LC -c -I src/c4ix/include src/c4ix/user/vfsload.c $T/u_vfsload.c4o > /dev/null
$LINK $T/u_vfsload.c4o $T/libc4ix.c4l -o "$OUT/c4ix-vfsload.c4r" > /dev/null

# ---- the desktop: its modules and the window kit's, linked together ----
DESKTOP="shell tools host term files taskmgr apps edit hl explore wall"
WINKIT="draw ui wmcore dialog look95 look31 textgrid savers calc notepad clock"
say "the desktop"
DFLAGS="-c -D C4IX_SHARED -I src/c4ix/include -I src/c4ix/user/desktop -I src/winkit"
par "$DFLAGS" d_ src/c4ix/user/desktop $DESKTOP
par "$DFLAGS" d_ src/winkit $WINKIT
$LC -c src/c4ix/libc/fp.c $T/d_fp.c4o > /dev/null
$LINK $(for m in $DESKTOP $WINKIT fp; do echo $T/d_$m.c4o; done) \
    -l $T/lib/libgui.c4s -l $T/lib/libarc.c4s -l $T/lib/libc4ix.c4s -o "$OUT/c4ix-desktop.c4r" > /dev/null

# ---- the toolchain: cc and b4ke (above) run c4sc and c4rlink ----
# c4sc and cpp as make builds them (cpp: src/c4cc/BUILD.B4K runs it, so
# b4ke -C /usr needs it on PATH); c4cc, c4rdump and c4rlink by the native
# c4cc as build-images.sh and src/c4cc/BUILD.B4K build them, with c4dos.h
say "the toolchain: c4sc, cpp, c4cc, c4rdump, c4rlink (64-bit images)"
for t in c4sc cpp; do
    [ "$(od -An -tu1 -j4 -N1 $t.c4r | tr -d ' ')" = 64 ] || { echo "disk64: $t.c4r is not a 64-bit image" >&2; exit 1; }
    cp $t.c4r "$OUT/$t.c4r"
done
CCSRC="include/u0.h include/c4dos.h load-c4r.c src/c4cc/c4cc.c src/c4cc/asm-c4r.c"
./c4cc -o "$OUT/c4cc.c4r" $CCSRC > /dev/null
./c4cc -o "$OUT/c4rdump.c4r" $CCSRC src/c4ke/bin/c4rdump.c > /dev/null
./c4cc -o "$OUT/c4rlink.c4r" $CCSRC src/c4ke/bin/c4rlink.c > /dev/null

# c4m as a C4IX program, as the 32-bit disk has it, and C4KE for it to boot
$LC -P -I src/c4ix/include -I include -I . -D C4CC=1 -D __c4cc__=1 c4m.c "$OUT/c4ix-c4m.c4r" > /dev/null
cp c4ke.c4r "$OUT/c4ke.c4r"

# ---- the manifest: the 32-bit disk's, less what this disk has not ----
say "the manifest (c4ix.vfs.txt)"
sed -e 's/^BIN_ALL = .*/BIN_ALL = '"$(echo $C4IX_USER $C4IX_ARC_USER desktop c4m | tr -s ' ')"'/' \
    -e 's/^TOOLS = .*/TOOLS = c4cc c4rlink c4rdump cpp c4sc/' \
    -e '/c4ix-libctest\.c4r\|c4ix-pictest\.c4r\|c4ix-unixtest\.c4r\|exercises libc/d' \
    src/c4bb/fs/c4ix.vfs.txt > $T/c4ix.vfs.in
python3 -I src/c4bb/tests/vfs-games.py $T/c4ix.vfs.in "$OUT/c4ix.vfs.txt"
cp src/c4ix/art/c4ix.c4p.gz "$OUT/c4ix-splash.c4p.gz"
cp src/c4ix/include/window.h "$OUT/c4ix-window.h"
cp src/c4ix/include/c4ix_user.h "$OUT/c4ix_user.h"
cp src/c4ix/c4ix.h "$OUT/c4ix.h"
cp src/c4ix/include/c4ix_accel.h "$OUT/c4ix_accel.h"
for n in $C4IX_USER $C4IX_ARC_USER; do cp src/c4ix/user/$n.c "$OUT/c4ix-$n.c"; done
cat src/winkit/winkit.h src/winkit/wkwin.h src/winkit/textgrid.h src/c4ix/user/desktop/desktop.h \
    src/c4ix/user/desktop/*.c src/winkit/*.c > "$OUT/c4ix-desktop.c"
for f in README.txt hello.c bounce.c mouselock.c; do cp src/c4ix/examples/$f "$OUT/c4ix-examples-$f"; done

# ---- the root archive: /usr, the repo as git has it, and the 64-bit
# libraries in /usr/lib (build-images.sh's, without /build) ----
say "the root archive, c4ix-root.tgz"
ROOT=$T/root
mkdir -p $ROOT/usr/lib $ROOT/usr/include
if git rev-parse --git-dir > /dev/null 2>&1; then
    git ls-files -- src docs include libjs README.md LICENSE Makefile BUILD.B4K INSTALL.B4K c4.c c4m.c c4m_float.c c4l.c load-c4r.c |
        grep -v '^src/c4bb/images/\|^src/c4or1k/images/\|^libjs/vendor/' |
        while read -r f; do
            [ -f "$f" ] && grep -Iq . "$f" 2>/dev/null || continue
            mkdir -p "$ROOT/usr/$(dirname "$f")"
            cp "$f" "$ROOT/usr/$f"
        done
fi
cp $T/libc4ix.c4l $ROOT/usr/lib/libc4ix.c4l
cp $T/libarc.c4l $ROOT/usr/lib/libarc.c4l
cp src/c4ix/include/*.h src/c4ix/include/*.c libjs/guest/gui.h libjs/guest/gui.c libjs/guest/snd.h libjs/guest/snd.c $ROOT/usr/include/
mkdir -p $ROOT/usr/include/libc/sys
cp src/c4ix/libc/include/*.h $ROOT/usr/include/libc/
cp src/c4ix/libc/include/sys/*.h $ROOT/usr/include/libc/sys/
tar --format=ustar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner -C $ROOT -cf - usr | gzip -9n > "$OUT/c4ix-root.tgz"
sha256sum "$OUT/c4ix-root.tgz" | cut -c1-16 > "$OUT/c4ix-root.tgz.id"
rm -rf $ROOT

# ---- the host directories' listings, and the sizes (build-images.sh's):
# c4ix.sizes makes each file a lazy RAM file vfsload need not copy, and
# says the root archive is here
HOSTDIRS=$(grep -o 'hostdir([^)]*)' "$OUT/c4ix.vfs.txt" | sed 's/^hostdir(//; s/)$//' | sort -u)
python3 -I src/c4bb/tests/host-dirs.py "$OUT" $HOSTDIRS > /dev/null
(cd "$OUT" && find . -type f -not -name c4ix.sizes -printf '%s %P\n' | sort -k2) > $T/sizes
awk -v pfx="$HOSTDIRS" 'BEGIN { n = split(pfx, p, " ") }
    { name = substr($0, index($0, " ") + 1)
      for (i = 1; i <= n; i++) if (index(name, p[i]) == 1 && name != p[i] ".c4ix-dir") next; print }' $T/sizes > "$OUT/c4ix.sizes"
say "done: $(ls "$OUT" | wc -l) files, $(du -sk "$OUT" | cut -f1) KB in $OUT"
