#!/bin/bash
# test-install.sh -- the climb's second power-on.
#
# The pin for docs/c4bb-storage.md M11. One machine, two boots, and the
# second one comes up in a system the first one compiled:
#
#   BIOS boots drive 0 (C4DOS)
#   LADDER            builds a compiler, a preprocessor and a kernel
#   INSTALL 1:        writes the boot medium, from install.lst
#   reboot 0          takes the C4DOS floppy OUT, and restarts
#   BIOS boots drive 1 -- the kernel this machine made
#
# The eject matters as much as the install: without it the BIOS finds
# drive 0 still bootable and you come up in exactly the system you were
# trying to leave.
set -e
cd "$(dirname "$0")/../../.."

CLI="node src/c4bb/sim/cli.js"
T=.bbinstall
rm -rf $T
mkdir -p $T/blank

# Drive 0 is the climb disk, read-only, as a floppy is. Drive 1 is
# blank and writable -- the medium being installed onto.
#
# The commands go in up front: everything up to the reboot is read by
# C4DOS, which does not lose queued input. `\q` after it is for the
# C4SH that comes up on the other side, and the timeout is the backstop
# if it does not land -- C4KE gives the console to the focused task, so
# a keystroke typed while one is running is gone.
{ printf 'LADDER\nRUN install.c4r 1:\nRUN reboot.c4r 0\n'; sleep 60; printf '\\q\n'; sleep 10; } \
  | timeout 900 $CLI -i -m 192 -d c4dos-c4ix32 -w $T/blank > $T/run.log 2>&1 || true

fail=0
check () {                                  # check <what> <pattern>
    if grep -q "$2" $T/run.log; then
        echo "test-install: $1 OK"
    else
        echo "test-install: $1 FAILED (no '$2')"
        fail=1
    fi
}

# 1. the first boot, off the floppy
check "boots drive 0"       "bios: drive 0 has c4dos32.c4r"
# 2. LADDER got as far as a kernel -- otherwise install would have
#    nothing to install and would say so instead
check "LADDER built a kernel" "wrote .* bytes to ram:c4ke.c4r"
# 3. the install itself, and the thing that makes the medium bootable
check "install ran"         "^install: [0-9]* files, [0-9]* bytes onto drive 1, boot.cfg -> c4ke.c4r"
# 4. the eject and the restart
# No anchor: the DOS prompt sits on the same line (`A>ejecting...`).
check "ejected and reset"   "ejecting drive 0"
check "soft reset"          "c4bb: soft reset"
# 5. the SECOND boot: a different drive, and the image install named
check "boots drive 1"       "bios: drive 1 has c4ke.c4r"
check "boots what we built" "bios: booting c4ke.c4r"
# 6. and it is a working system with its own tree, not a bare kernel
check "kernel ready"        "entering task scheduling"
check "a source tree"       "^vfsload: [0-9]*/[0-9]* entries loaded"
check "a shell"             "C4SH - The C4 SHell"

# Nothing on the medium may be missing: vfsload names what it could not
# find, and on a medium this install wrote the answer is nothing. This
# is what keeps install.lst and c4ke-climb.vfs.txt honest about each
# other -- a name in one and not the other shows up here.
if grep -q "vfsload: cannot open" $T/run.log; then
    echo "test-install: installed medium is complete FAILED"
    grep "vfsload: cannot open" $T/run.log | head -5
    fail=1
else
    echo "test-install: installed medium is complete OK ($(grep -o 'vfsload: [0-9]*/[0-9]* entries' $T/run.log | head -1))"
fi

# And the medium really is on the host side of the drive, not just in
# the machine's memory: these are the files a later session would find.
for f in boot.cfg c4ke.c4r init.c4r c4sh.c4r c4ix-src.tar; do
    if [ ! -s $T/blank/$f ]; then
        echo "test-install: $f is not on the medium FAILED"
        fail=1
    fi
done
if [ "$(cat $T/blank/boot.cfg 2>/dev/null | tr -d '\n')" != "c4ke.c4r" ]; then
    echo "test-install: boot.cfg does not name c4ke.c4r FAILED"
    fail=1
fi

if [ $fail = 0 ]; then
    rm -rf $T
    echo "test-install: OK -- C4DOS built a kernel, installed it on a blank"
    echo "                   medium, and the machine restarted into it"
else
    echo "test-install: FAILURES -- log in $T/run.log"
    exit 1
fi
