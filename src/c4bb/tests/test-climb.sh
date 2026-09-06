#!/bin/bash
# test-climb.sh -- three power-ons, three systems, each booted from a
# disk the previous one wrote.
#
# The pin for docs/c4bb-storage.md M11 and M12. One machine, three
# drives, and nothing carried between the boots except the media:
#
#   BIOS boots drive 0 (C4DOS, the shipped floppy)
#     LADDER                  builds a compiler, a preprocessor, a kernel
#     install B:              writes the C4KE medium, from install.lst
#     reboot 0                floppy OUT, restart
#   BIOS boots drive 1 (C4KE -- the kernel C4DOS compiled)
#     tar x c4ix-src.tar      the sources the medium was carrying
#     b4ke -f c4ix.b4k        twelve modules and a link
#     kinstall C:             writes the C4IX medium, from c4ix.lst
#     reboot 1                that medium OUT, restart
#   BIOS boots drive 2 (C4IX -- the kernel C4KE compiled)
#
# Driven by what comes OUT rather than by a clock: a sleep long enough
# for b4ke on a slow host is minutes of waiting on a fast one, and one
# short enough for a fast host fails on a slow one for no reason at all.
#
# Two waits per step, and the first one is not optional. C4KE gives the
# console to the FOCUSED task, so a line typed while a program is still
# running is simply gone -- the first version of this test fed `b4ke`
# the instant `tar` printed its summary, watched the shell echo it and
# then do nothing, and sat there for the full fifteen minutes. So: wait
# until the log ENDS in a prompt, which is the system saying it is
# idle and listening, and only then type.
set -e
cd "$(dirname "$0")/../../.."

T=.bbclimb
rm -rf $T
mkdir -p $T/d1 $T/d2
LOG=$T/run.log

mkfifo $T/in
timeout 2400 node src/c4bb/sim/cli.js -i -s -m 256 \
    -d c4dos-c4ix32 -w $T/d1 -w $T/d2 < $T/in > $LOG 2>&1 &
SIM=$!
exec 3> $T/in                       # blocks until the simulator opens it

fail=0
cleanup () { exec 3>&- 2>/dev/null || true; kill $SIM 2>/dev/null || true; }
trap cleanup EXIT

# Is the machine sitting at a prompt with nothing after it? The prompt
# is the last thing in the file and carries no newline, so "the tail
# ends with it" is exactly the question. Asked twice, two seconds
# apart, because a prompt that is about to be followed by more output
# is not an idle one.
idle () {                           # idle <prompt> <seconds>
    local i=0 seen=0
    while [ $i -lt $2 ]; do
        if tail -c 400 "$LOG" | tr -d '\r' | grep -qe "$1\$"; then
            seen=$((seen + 1))
            [ $seen -ge 2 ] && return 0
        else
            seen=0
        fi
        sleep 2
        i=$((i + 2))
    done
    echo "test-climb: no prompt '$1' after ${2}s"
    tail -20 $LOG
    return 1
}

# Wait for a line to appear. Every marker below is unique to its step,
# so an earlier stage's output can never satisfy a later stage's wait.
await () {                          # await <what> <marker> <seconds>
    local i=0
    while [ $i -lt $3 ]; do
        if grep -q -- "$2" $LOG; then echo "test-climb: $1 OK"; return 0; fi
        sleep 2
        i=$((i + 2))
    done
    echo "test-climb: $1 TIMED OUT after $3s (no '$2')"
    tail -20 $LOG
    fail=1
    return 1
}

# Wait for the prompt, type, wait for the result.
run () {                            # run <what> <prompt> <cmd> <marker> <secs>
    idle "$2" 300 || { fail=1; return 1; }
    printf '%s\n' "$3" >&3
    await "$1" "$4" "$5"
}

want () {                           # want <what> <pattern>
    grep -q -- "$2" $LOG || { echo "test-climb: $1 FAILED"; fail=1; }
}

# ---- power-on 1: the shipped floppy ---------------------------------
await "BIOS boots drive 0" "bios: drive 0 has c4dos32.c4r" 60 || exit 1
run "C4DOS built a kernel"       'A>' 'LADDER' \
    "Type RUN dosload.c4r c4ke.c4r"  600 || exit 1
run "installed the C4KE medium"  'A>' 'RUN install.c4r B:' \
    "install: eject drive 0"         300 || exit 1
want "install summary" "^install: [0-9]* files, [0-9]* bytes onto drive 1, boot.cfg -> c4ke.c4r"
idle 'A>' 60 || exit 1
printf 'RUN reboot.c4r 0\n' >&3

# ---- power-on 2: the kernel C4DOS compiled --------------------------
await "BIOS boots drive 1" "bios: drive 1 has c4ke.c4r"     120 || exit 1
await "C4KE came up"       "C4SH - The C4 SHell"            300 || exit 1
await "C4KE has its tree"  "^vfsload: 51/51 entries loaded"  60 || exit 1
run "unpacked C4IX's source" 'c4sh>' 'tar x c4ix-src.tar' \
    "tar: extracted"             120 || exit 1
run "C4KE built C4IX"        'c4sh>' 'b4ke -f c4ix.b4k' \
    "b4ke: C4IX built"           900 || exit 1
want "b4ke had no failures" "b4ke: 13 ran, 0 skipped, 0 failed"
run "installed the C4IX medium" 'c4sh>' 'kinstall C:' \
    "kinstall: eject drive 1"    300 || exit 1
want "kinstall summary" "^kinstall: [0-9]* files, [0-9]* bytes onto drive 2, boot.cfg -> c4ix.c4r"
idle 'c4sh>' 60 || exit 1
printf 'reboot 1\n' >&3

# ---- power-on 3: the kernel C4KE compiled ---------------------------
await "BIOS boots drive 2" "bios: drive 2 has c4ix.c4r"      120 || exit 1
await "C4IX came up"       "c4ix-sh -- 'help' for builtins"  300 || exit 1
await "C4IX has its tree"  "vfsload: 28/28 entries loaded"    60 || exit 1
run "and a userland"       'c4ix:/\$' 'ls' "^uhello"         120 || exit 1
run "clean shutdown"       'c4ix:/\$' 'exit' \
    "c4ix: shutdown complete"    120 || exit 1

exec 3>&-
wait $SIM 2>/dev/null || true

# Nothing either install wrote may be missing: vfsload names what it
# could not find, and on media these two wrote the answer is nothing.
# This is what keeps the four lists honest about each other --
# install.lst with c4ke-climb.vfs.txt, c4ix.lst with c4ix-climb.vfs.txt.
if grep -q "vfsload: cannot open" $LOG; then
    echo "test-climb: an installed medium is incomplete FAILED"
    grep "vfsload: cannot open" $LOG | head -5
    fail=1
else
    echo "test-climb: both installed media are complete OK"
fi

# And the media are real on the host side -- what a later session finds.
for f in boot.cfg c4ke.c4r init.c4r c4sh.c4r c4ix-src.tar kinstall.c4r; do
    [ -s $T/d1/$f ] || { echo "test-climb: d1/$f missing FAILED"; fail=1; }
done
for f in boot.cfg c4ix.c4r c4ix-sh.c4r c4ix-vfsload.c4r; do
    [ -s $T/d2/$f ] || { echo "test-climb: d2/$f missing FAILED"; fail=1; }
done
[ "$(tr -d '\n' < $T/d1/boot.cfg 2>/dev/null)" = "c4ke.c4r" ] \
    || { echo "test-climb: d1 boot.cfg does not name c4ke.c4r FAILED"; fail=1; }
[ "$(tr -d '\n' < $T/d2/boot.cfg 2>/dev/null)" = "c4ix.c4r" ] \
    || { echo "test-climb: d2 boot.cfg does not name c4ix.c4r FAILED"; fail=1; }

if [ $fail = 0 ]; then
    echo "test-climb: OK -- C4DOS built C4KE and installed it, C4KE built"
    echo "                 C4IX and installed it, and the machine booted"
    echo "                 each one off the medium the last one wrote"
    rm -rf $T
else
    echo "test-climb: FAILURES -- log in $LOG"
    exit 1
fi
