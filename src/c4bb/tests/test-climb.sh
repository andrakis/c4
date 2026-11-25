#!/bin/bash
# test-climb.sh -- three systems, each booted from a medium the one
# below it wrote, with C4IX's build shared out over four machines.
#
# The pin for docs/c4bb-storage.md M11 and M12. Nothing is carried
# between the systems but media:
#
#   machine "climb"   BIOS boots drive 0 (C4DOS, the shipped floppy)
#     LADDER                    builds a compiler, a preprocessor, a kernel
#     install B:                writes the C4KE medium, from install.lst
#     reboot 0                  floppy OUT, restart
#                     BIOS boots drive 1 (C4KE -- the kernel C4DOS compiled)
#     tar x c4ix-src.tar        the sources the medium was carrying
#     b4ke -f c4ix.b4k <share>  its share of the thirteen modules
#     tar c objs.tar <share>    the objects it compiled...
#     save 2: objs.tar          ...onto its own blank medium
#
#   machines "s1" "s2" "s3", powered on the moment install B: is done,
#   each with that same C4KE medium in drive 0 (read-only) and a blank
#   in drive 1: BIOS boots drive 0, then the same four commands, with
#   their own share, and `save 1:`.
#
#   machine "link"    the C4KE medium in drive 0, a blank in drive 1,
#                     and the four object media in drives 2 to 5
#                     BIOS boots drive 0 (C4KE)
#     tar x 2:objs.tar ...      every object, off the medium it came on
#     b4ke -f c4ix.b4k          thirteen already here, skipped; the link
#     kinstall B:               writes the C4IX medium, from c4ix.lst
#     reboot 0                  C4KE's medium OUT, restart
#                     BIOS boots drive 1 (C4IX -- the kernel C4KE compiled)
#
# WHY FOUR MACHINES. On one, b4ke was 2,392 s of a 2,487 s test, and
# the test was the suite's critical path. The proof is unchanged: every
# module is compiled by C4KE, in a machine booted from the medium C4DOS
# wrote, and the kernel C4IX boots is the one C4KE linked from those
# objects. What changed is only how many machines compile at once. The
# objects travel the way anything travels between machines that share
# nothing: on a medium, written by the system that made them (`tar c`,
# `save`) and read by the one that links them (`tar x 2:objs.tar`). The
# host packs nothing; it only puts disks in drives. And the link
# machine's b4ke runs the WHOLE c4ix.b4k, so "13 skipped" is the check
# that every module arrived -- one left out of every share would be
# compiled there instead, and fail that count.
#
# The shares are balanced by measured time (SHARES below). mmu.c is
# the longest single compile, so it is a share on its own and the
# floor for the whole test.
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

# Who builds what. The first share is the climb machine's, which starts
# compiling a couple of seconds after the others (it reboots into C4KE
# rather than powering on into it). Seconds are one module at a time on
# helios, timed in a whole-suite run, 2026-11-12: keep the four sums
# close, and re-time them when a module grows.
#
#   mmu 518  vfs 373  sched 347  sys 229  c4ke 172  loader 159
#   console 157  task 145  sl4b 109  boot 94  init 81  va 54  host 45
#
# Four at once, each share takes about 1.3 times its sum (mmu and va
# together took 776 s), and mmu alone sets the floor.
SHARES=(
    "mmu.c4o"                                                # 518
    "vfs.c4o sys.c4o host.c4o"                               # 647
    "sched.c4o c4ke.c4o sl4b.c4o"                            # 628
    "loader.c4o console.c4o task.c4o boot.c4o init.c4o va.c4o"  # 690
)

T=.bbclimb
rm -rf $T
mkdir -p $T/d1 $T/d2
T0=$(date +%s)
SIM_PIDS=

# Power a machine on: a simulator reading $T/<name>.in and writing
# $T/<name>.log, its drives as cli.js takes them. The caller then opens
# the fifo on fd 3, which blocks until the simulator has it open.
# The whole machine's budget is an hour; the waits below are what
# notice a stuck step, and this only stops one that never ends.
power_on () {                       # power_on <name> <drive args...>
    local name=$1
    shift
    mkfifo $T/$name.in
    # Through cat, not `< fifo`: on Windows node is no MSYS2 program, and
    # an MSYS2 fifo handed to it reads as already ended, so C4DOS halted
    # at its first prompt (docs/windows.md). cat is one, and passes what
    # is written on through a pipe, as it does anywhere. $! is timeout's.
    cat $T/$name.in | timeout 3600 node src/c4bb/sim/cli.js -i -s -m 256 "$@" \
        > $T/$name.log 2>&1 &
    echo $! > $T/$name.pid
}

# Waited for, not just killed: the suite runner learns how many CPUs a
# test uses from its children's CPU time, and a simulator nobody reaps
# is a simulator whose time is never counted.
power_off () {                      # power_off <name>
    exec 3>&- 2>/dev/null || true
    kill "$(cat $T/$1.pid)" 2>/dev/null || true
    wait "$(cat $T/$1.pid)" 2>/dev/null || true
}

fail=0
cleanup () {
    exec 3>&- 2>/dev/null || true
    for p in $T/*.pid; do [ -f "$p" ] && kill "$(cat "$p")" 2>/dev/null; done
    for p in $SHARD_PIDS; do kill $p 2>/dev/null; done
    true
}
trap cleanup EXIT

# Each machine's driver sets M (its name) and LOG, and talks on fd 3.
say () { echo "test-climb: $M: $* (t=$(( $(date +%s) - T0 ))s)"; }

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
    say "no prompt '$1' after ${2}s"
    tail -20 $LOG
    return 1
}

# Wait for a line to appear. Every marker below is unique to its step,
# so an earlier stage's output can never satisfy a later stage's wait --
# or, for a step done several times over, it is the Nth time it appears.
await () {                          # await <what> <marker> <seconds> [N]
    local i=0
    while [ $i -lt $3 ]; do
        if [ "$(grep -c -- "$2" $LOG)" -ge "${4:-1}" ]; then say "$1 OK"; return 0; fi
        sleep 2
        i=$((i + 2))
    done
    say "$1 TIMED OUT after $3s (no '$2')"
    tail -20 $LOG
    fail=1
    return 1
}

# The same for a step that runs for a long time but SAYS so as it goes:
# c4sc prints its passes as each module is built. The budget is for
# silence, not for the step, so a build that grows -- or a slower host
# -- does not fail it, while one that stops making output still does. A
# fixed 900 s for all of b4ke was outgrown as C4IX grew (sched.c alone
# went from 29 KB to 65 KB in a week). <failed> ends the wait at once:
# b4ke stops at the first failed step and prints its summary.
await_busy () {                     # await_busy <what> <marker> <quiet secs> <failed>
    local quiet=0 size=-1 now start=$(date +%s)
    while [ $quiet -lt $3 ]; do
        if grep -q -- "$2" $LOG; then
            say "$1 OK, $(( $(date +%s) - start ))s"
            return 0
        fi
        if grep -q -- "$4" $LOG; then
            say "$1 FAILED"
            tail -20 $LOG
            fail=1
            return 1
        fi
        now=$(wc -c < $LOG)
        if [ "$now" != "$size" ]; then size=$now; quiet=0; else quiet=$((quiet + 2)); fi
        sleep 2
    done
    say "$1 TIMED OUT, ${3}s without output (no '$2')"
    tail -20 $LOG
    fail=1
    return 1
}

# Wait for the prompt, type, wait for the result.
run () {                            # run <what> <prompt> <cmd> <marker> <secs> [N]
    idle "$2" 300 || { fail=1; return 1; }
    printf '%s\n' "$3" >&3
    await "$1" "$4" "$5" "$6"
}

want () {                           # want <what> <pattern>
    grep -q -- "$2" $LOG || { say "$1 FAILED"; fail=1; }
}

# A C4KE machine that has just come up: the shell, and the tree.
c4ke_up () {
    await "C4KE came up"       "C4SH - The C4 SHell"            300 || return 1
    await "C4KE has its tree"  "^vfsload: 51/51 entries loaded"  60 || return 1
}

# One share, on a C4KE machine at its prompt: unpack, build the share,
# pack what it built, and save that onto the blank medium in <drive>.
build_share () {                    # build_share <drive> <share>
    local n
    n=$(echo $2 | wc -w)
    run "unpacked C4IX's source" 'c4sh>' 'tar x c4ix-src.tar' \
        "tar: extracted"             120 || return 1
    idle 'c4sh>' 300 || return 1
    printf 'b4ke -f c4ix.b4k %s\n' "$2" >&3
    await_busy "built $2" "^b4ke: [0-9]* ran, [0-9]* skipped, 0 failed" 900 \
        "^b4ke: [0-9]* ran, [0-9]* skipped, [1-9]" || return 1
    want "b4ke built the whole share" "^b4ke: $n ran, 0 skipped, 0 failed"
    run "packed the objects" 'c4sh>' "tar c objs.tar $2" \
        "^tar: packed $n files, [0-9]* bytes into objs.tar" 120 || return 1
    run "saved them" 'c4sh>' "save $1: objs.tar" \
        "^save: 1 files, [0-9]* bytes onto drive $1" 120 || return 1
}

# A shard machine, start to finish. Runs in the background, so its own
# fd 3, LOG and fail; the exit status is the verdict.
shard () {                          # shard <name> <share>
    M=$1
    LOG=$T/$1.log
    mkdir -p $T/o-$1
    power_on $1 -d $T/d1 -w $T/o-$1
    exec 3> $T/$1.in
    if await "BIOS boots drive 0" "bios: drive 0 has c4ke.c4r" 120 &&
       c4ke_up && build_share 1 "$2"; then :; else fail=1; fi
    power_off $1
    exit $fail
}

# ---- power-on 1: the shipped floppy ---------------------------------
M=climb
LOG=$T/climb.log
mkdir -p $T/o-climb
power_on climb -d c4dos-c4ix32 -w $T/d1 -w $T/o-climb
exec 3> $T/climb.in                 # blocks until the simulator opens it

await "BIOS boots drive 0" "bios: drive 0 has c4dos32.c4r" 60 || exit 1
run "C4DOS built a kernel"       'A:\\>' 'LADDER' \
    "Type RUN dosload.c4r c4ke.c4r"  600 || exit 1
run "installed the C4KE medium"  'A:\\>' 'RUN install.c4r B:' \
    "install: eject drive 0"         300 || exit 1
want "install summary" "^install: [0-9]* files, [0-9]* bytes onto drive 1, boot.cfg -> c4ke.c4r"
idle 'A:\\>' 60 || exit 1

# The C4KE medium is whole: the other three can power on from it now.
SHARD_PIDS=
i=1
while [ $i -lt ${#SHARES[@]} ]; do
    ( shard s$i "${SHARES[$i]}" ) &
    SHARD_PIDS="$SHARD_PIDS $!"
    i=$((i + 1))
done

printf 'RUN reboot.c4r 0\n' >&3

# ---- power-on 2: the kernel C4DOS compiled --------------------------
await "BIOS boots drive 1" "bios: drive 1 has c4ke.c4r"     120 || exit 1
c4ke_up || exit 1
build_share 2 "${SHARES[0]}" || exit 1
power_off climb

for p in $SHARD_PIDS; do wait $p || fail=1; done
SHARD_PIDS=
[ $fail = 0 ] || { M=climb; say "a share FAILED -- logs in $T"; exit 1; }

# ---- the link: every object, off the media that carried them --------
M=link
LOG=$T/link.log
drives=
for m in climb $(i=1; while [ $i -lt ${#SHARES[@]} ]; do echo s$i; i=$((i + 1)); done); do
    drives="$drives -d $T/o-$m"
done
power_on link -d $T/d1 -w $T/d2 $drives
exec 3> $T/link.in

await "BIOS boots drive 0" "bios: drive 0 has c4ke.c4r"     120 || exit 1
c4ke_up || exit 1
i=0
while [ $i -lt ${#SHARES[@]} ]; do
    run "unpacked drive $((i + 2))'s objects" 'c4sh>' "tar x $((i + 2)):objs.tar" \
        "^tar: extracted [0-9]* files" 120 $((i + 1)) || exit 1
    i=$((i + 1))
done
# and each archive held exactly the share its machine was given
i=0
grep "^tar: extracted" $LOG | while read -r _ _ got _; do
    want=$(echo ${SHARES[$i]} | wc -w)
    [ "$got" = "$want" ] || { say "drive $((i + 2)) held $got objects, not $want FAILED"; exit 1; }
    i=$((i + 1))
done || fail=1
idle 'c4sh>' 300 || exit 1
printf 'b4ke -f c4ix.b4k\n' >&3
await_busy "C4KE linked C4IX" "b4ke: C4IX built" 900 "^b4ke: [0-9]* ran," || exit 1
want "every module arrived, and only the link ran" "b4ke: 1 ran, 13 skipped, 0 failed"
run "installed the C4IX medium" 'c4sh>' 'kinstall B:' \
    "kinstall: eject drive 0"    300 || exit 1
want "kinstall summary" "^kinstall: [0-9]* files, [0-9]* bytes onto drive 1, boot.cfg -> c4ix.c4r"
idle 'c4sh>' 60 || exit 1
printf 'reboot 0\n' >&3

# ---- power-on 3: the kernel C4KE compiled ---------------------------
await "BIOS boots drive 1" "bios: drive 1 has c4ix.c4r"      120 || exit 1
await "C4IX came up"       "c4ix-sh -- 'help' for builtins"  300 || exit 1
await "C4IX has its tree"  "vfsload: 15/15 entries loaded"    60 || exit 1
run "and a userland"       'c4ix:/\$' 'ls /bin' "^uhello"    120 || exit 1
run "clean shutdown"       'c4ix:/\$' 'exit' \
    "c4ix: shutdown complete"    120 || exit 1

exec 3>&-
wait "$(cat $T/link.pid)" 2>/dev/null || true
M=climb

# Nothing either install wrote may be missing: vfsload names what it
# could not find, and on media these two wrote the answer is nothing.
# This is what keeps the four lists honest about each other --
# install.lst with c4ke-climb.vfs.txt, c4ix.lst with c4ix-climb.vfs.txt.
if grep -q "vfsload: cannot open" $T/*.log; then
    say "an installed medium is incomplete FAILED"
    grep "vfsload: cannot open" $T/*.log | head -5
    fail=1
else
    say "both installed media are complete OK"
fi

# And the media are real on the host side -- what a later session finds.
for f in boot.cfg c4ke.c4r init.c4r c4sh.c4r c4ix-src.tar kinstall.c4r; do
    [ -s $T/d1/$f ] || { say "d1/$f missing FAILED"; fail=1; }
done
for f in boot.cfg c4ix.c4r c4ix-sh.c4r c4ix-vfsload.c4r; do
    [ -s $T/d2/$f ] || { say "d2/$f missing FAILED"; fail=1; }
done
[ "$(tr -d '\n' < $T/d1/boot.cfg 2>/dev/null)" = "c4ke.c4r" ] \
    || { say "d1 boot.cfg does not name c4ke.c4r FAILED"; fail=1; }
[ "$(tr -d '\n' < $T/d2/boot.cfg 2>/dev/null)" = "c4ix.c4r" ] \
    || { say "d2 boot.cfg does not name c4ix.c4r FAILED"; fail=1; }

if [ $fail = 0 ]; then
    echo "test-climb: OK -- C4DOS built C4KE and installed it, C4KE built"
    echo "                 C4IX on four machines and installed it, and each"
    echo "                 system booted off the medium the last one wrote"
    echo "                 ($(( $(date +%s) - T0 ))s)"
    rm -rf $T
else
    echo "test-climb: FAILURES -- logs in $T"
    exit 1
fi
