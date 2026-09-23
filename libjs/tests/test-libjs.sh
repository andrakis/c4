#!/bin/bash
# test-libjs.sh - the libjs c4m against native c4m, and both kernels.
# Run from the repo root (make test-libjs). Modelled on
# src/c4bb/tests/test-c4bb.sh, whose images and oracles it shares.
#
# The oracle for each image is native 32-bit c4m (./c4m32 load-c4r.c),
# except where c4m32 cannot be trusted:
#   - %lld reads two words on a 32-bit libc where the program pushed one,
#     so those images compare against 64-bit c4m;
#   - test_args and test_printf segfault c4m32 in its missed-trap path.
# Trap chatter from kernel-less images and %p lines are filtered.
#
# Out of contract, as on c4bb: test_customop test_signal test_timekeeping
# test_fread (need a kernel or a host fd), test_crash (STRC needs symbols).

LIBJS="node libjs/cli.js"
C4M32="./c4m32 load-c4r.c --"
C4M64="./c4m load-c4r.c --"
IMAGES=src/c4bb/images
FILTER='/^Trap type/d; /missed a trap/d'
NORM='s/rendered in [0-9]*ms/rendered in Xms/; s|f/s [ 0-9]\{5\}|f/s XXXXX|g; s/kc [ 0-9]\{5\}/kc XXXXX/g'
fail=0

ok ()   { echo "test-libjs: $1 OK"; }
bad ()  { echo "test-libjs: $1 FAILED"; fail=1; }

check () {
    name="$1"; shift
    a=$(timeout 300 $LIBJS "$@" 2>/dev/null | sed "$FILTER" | sed "$NORM")
    b=$(timeout 300 $C4M32 "$@" 2>&1 | sed "$FILTER" | sed "$NORM")
    if [ "$a" = "$b" ]; then ok "$name"; else
        bad "$name"; diff <(echo "$a") <(echo "$b") | head -10; fi
}

check64 () {
    name="$1"; img32="$2"; img64="$3"
    a=$(timeout 300 $LIBJS "$img32" 2>/dev/null | sed "$FILTER")
    b=$(timeout 300 $C4M64 "$img64" 2>&1 | sed "$FILTER")
    if [ "$a" = "$b" ]; then ok "$name (vs 64-bit baseline)"; else
        bad "$name"; diff <(echo "$a") <(echo "$b") | head -10; fi
}

# ---- pure units ---------------------------------------------------------
node libjs/tests/test-printf.mjs || fail=1
node libjs/tests/test-heap.mjs   || fail=1
node libjs/tests/test-gui.mjs    || fail=1

# ---- programs, byte for byte against native -----------------------------
for t in hello32 test_basic test_static test_vprintf tests \
         multifun test-order test-ptrs test_continue test_exit \
         test_printloop mandel test_float cycles; do
    check "$t" $IMAGES/$t.c4r
done
check raycast $IMAGES/disk/raycast.c4r 21x21 -s 7 -d -n 20 -g 80x22
# gui-demo: without a display fitted (the CLI fits none) it must say
# "not fitted" on both hosts and touch nothing.
for t in bb_customop bb_preempt bb_pm bb_mbox gui-demo fb-demo; do
    check "$t" $IMAGES/$t.c4r
done
check64 factorial   $IMAGES/factorial.c4r   src/tests/factorial.c4r
check64 test_malloc $IMAGES/test_malloc.c4r src/tests/test_malloc.c4r

argnorm () { sed 's|src/c4bb/images/|src/tests/|; /an int ptr/d'; }
for t in test_args test_printf; do
    extra=""; [ $t = test_args ] && extra="arg1 arg2"
    a=$(timeout 300 $LIBJS $IMAGES/$t.c4r $extra 2>/dev/null | sed "$FILTER" | argnorm)
    b=$(timeout 300 $C4M64 src/tests/$t.c4r $extra 2>&1 | sed "$FILTER" | argnorm)
    if [ "$a" = "$b" ]; then ok "$t (vs 64-bit baseline)"; else
        bad "$t"; diff <(echo "$a") <(echo "$b") | head -10; fi
done

# ---- the fused opcodes --------------------------------------------------
# All ten, each against its unfused twin, hand-assembled by c4th's
# assembler (the same differential c4bb runs).
fused_out=$(timeout 300 $LIBJS -s -m 96 -d $IMAGES/fused $IMAGES/fused/c4th32.c4r core.f asm.f fused.f 2>&1)
if echo "$fused_out" | grep -q "fused: 0 mismatches" &&
   [ "$(echo "$fused_out" | grep -c ' ok$')" = "10" ] &&
   echo "$fused_out" | grep -q "0 missed traps"; then
    ok "fused all ten"
else
    bad "fused all ten"; echo "$fused_out" | tail -5
fi

# ---- STRC, with names from the symbol section ------------------------------
strc_out=$($LIBJS $IMAGES/libjs-strc.c4r 2>&1 | sed 's/ \[0x[0-9A-F]*\]//')
strc_want=$(printf 'c4m: stacktrace:\n inner()\n  inner()\n   inner()\n    middle()\n     main()\nresult 7')
if [ "$strc_out" = "$strc_want" ]; then ok "stacktrace names every frame"
else bad "stacktrace names every frame"; echo "$strc_out"; fi

# ---- exit status ----------------------------------------------------------
$LIBJS $IMAGES/test_exit.c4r >/dev/null 2>&1; s1=$?
$C4M32 $IMAGES/test_exit.c4r >/dev/null 2>&1; s2=$?
if [ "$s1" = "$s2" ]; then ok "exit-status ($s1)"; else bad "exit-status ($s1 vs $s2)"; fi

# ---- the programmable interrupt timer -------------------------------------
pit_out=$(timeout 120 $LIBJS -m 8 $IMAGES/bb_pit.c4r 2>&1)
if [ "$(echo "$pit_out" | grep -c "real time")" = 2 ]; then ok "pit (free running and masked every pass)"
else bad "pit"; echo "$pit_out" | tail -4; fi

# ---- C4KE ----------------------------------------------------------------
# Grep assertions, as c4bb does: interleavings differ from native.
c4ke () { printf "$1" | timeout 300 $LIBJS -c "${2:-60000000}" -d $IMAGES/disk $IMAGES/c4ke32.c4r 2>/dev/null; }
out=$(c4ke 'hello.c4r\n\\q\n')
for want in "c4ke v" "entering task scheduling" "C4SH - The C4 SHell" "yello" "clean shutdown"; do
    if echo "$out" | grep -qF "$want"; then ok "c4ke-boot '$want'"; else bad "c4ke-boot '$want'"; fi
done
out=$(c4ke 'ls -a /usr/src/bin\n\\q\n')
counts=$(echo "$out" | grep -oE '[0-9]+/[0-9]+ entries loaded')
n1="${counts%%/*}"; n2="${counts#*/}"; n2="${n2%% entries*}"
if [ -n "$counts" ] && [ "$n1" = "$n2" ]; then ok "c4ke-vfs full manifest loaded ($counts)"
else bad "c4ke-vfs full manifest loaded (got '$counts')"; fi
if echo "$out" | grep -qF "/usr/src/bin/ls.c"; then ok "c4ke-vfs ls listing"; else bad "c4ke-vfs ls listing"; fi
out=$(c4ke 'ls\necho second-command\n\\q\n')
if echo "$out" | grep -qF "entries in /" && echo "$out" | grep -qF "second-command" &&
   echo "$out" | grep -qF "clean shutdown"; then ok "c4ke two commands in one session"
else bad "c4ke two commands in one session"; echo "$out" | tail -8; fi
out=$(c4ke 'badop.c4r\necho survived-the-kill\n\\q\n')
if echo "$out" | grep -qF "Custom opcode not found: 9001" && echo "$out" | grep -qF "badop.c4r:main()" &&
   echo "$out" | grep -qF "survived-the-kill" && echo "$out" | grep -qF "clean shutdown"; then
    ok "c4ke survives a task killed on a bad opcode"
else bad "c4ke survives a task killed on a bad opcode"; echo "$out" | tail -10; fi
out=$(c4ke 'cat /usr/src/bin/vfsload.c\n\\q\n')
if echo "$out" | grep -qF "C4KE boot-time filesystem loader"; then ok "c4ke-vfs cat real content"
else bad "c4ke-vfs cat real content"; fi

# ---- C4IX ----------------------------------------------------------------
out=$(printf 'c4ix-ps.c4r\nexit\n' | timeout 300 $LIBJS -c 120000000 -d $IMAGES/disk $IMAGES/c4ix32.c4r 2>/dev/null)
for want in "C4IX booting" "protected mode on for user tasks, preemption on" \
            "c4ix-sh" "c4ix-ps.c4r" "tasks,"; do
    if echo "$out" | grep -qF "$want"; then ok "c4ix-boot '$want'"; else bad "c4ix-boot '$want'"; fi
done
# The C4IX vfsload race (docs/c4bb-design.md) is a known issue carried
# over from c4bb, so "most entries" is the pin here too.
if echo "$out" | grep -qE '4[0-9]/4[0-9] entries loaded from c4ix\.vfs\.txt'; then
    ok "c4ix-vfs manifest mostly loaded ($(echo "$out" | grep -oE '[0-9]+/[0-9]+ entries loaded'))"
else bad "c4ix-vfs manifest mostly loaded"; echo "$out" | grep -E 'entries loaded|cannot open' | head -5; fi

if [ $fail = 0 ]; then echo "test-libjs: OK"; else echo "test-libjs: FAILURES"; exit 1; fi
