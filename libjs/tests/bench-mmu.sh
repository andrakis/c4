#!/bin/sh
# bench-mmu.sh [--mmu] [-n RUNS] [WHAT...] -- C4IX under node, timed, for
# docs/c4ix-mmu.md section 12: with the MMU (--mmu) and without.
#
#   boot     boot, unpack the root archive (`wait 4`), shut down
#   bench    C4IX's bench: pi digits and factorials, then the OS calls
#   cc       cc builds mandel.c and runs it, after the unpack
#   memory   the unpack again, by hand: tar of the root archive into /tmp
#   doom     Doom's demo3, -timedemo -nosound
#   wolf3d   Wolf3D's first demo, --timedemo 1
#   rott     ROTT's demo 4, --timedemo 4 NOSOUND
#   desktop  boot to the desktop, as the web page boots (bench-desktop.mjs)
#
# Each line is: WHAT SECONDS CYCLES MIPS, the best of RUNS (default 1), where
# SECONDS is the wall time of the whole session and CYCLES and MIPS are
# cli.js -s's. The game sessions boot and unpack first, as `boot` does, so
# their own time is theirs less boot's.
#
# C4IX_IMAGE: another kernel than the disk's, built on its own.
#
# Run from the repo root after make c4bb-images.

MMU=--no-mmu
RUNS=1
while [ $# -gt 0 ]; do
    case "$1" in
        --mmu) MMU=--mmu; shift ;;
        -n) RUNS=$2; shift 2 ;;
        *) break ;;
    esac
done
[ $# -gt 0 ] || set -- boot cc memory doom wolf3d rott desktop
IMAGES=src/c4bb/images
OUT=${TMPDIR:-/tmp}/bench-mmu.$$
mkdir -p "$OUT"

script () {
    case "$1" in
        boot)   printf 'wait 4\nexit\n' ;;
        bench)  printf 'wait 4\nbench\nexit\n' ;;
        cc)     printf 'wait 4\ncd /usr/src/tests\ncc -O mandel.c\n./mandel.c4r\nexit\n' ;;
        memory) printf 'wait 4\nmkdir /tmp/x\ntar xf /usr/lib/c4ix-root.tgz -C /tmp/x\nexit\n' ;;
        doom)   printf 'wait 4\ndoom -timedemo demo3 -nosound\nexit\n' ;;
        wolf3d) printf 'wait 4\nwolf3d --timedemo 1\nexit\n' ;;
        rott)   printf 'wait 4\nrott --timedemo 4 NOSOUND\nexit\n' ;;
    esac
}

for w in "$@"; do
    if [ $w = desktop ]; then
        node libjs/tests/bench-desktop.mjs $MMU $RUNS | sed 's/^desktop [a-z]* /desktop /'
        continue
    fi
    best=
    k=0
    while [ $k -lt $RUNS ]; do
        t0=$(date +%s.%N)
        script $w | timeout 3000 node libjs/cli.js -s $MMU -m 64 -d $IMAGES/disk ${C4IX_IMAGE:-$IMAGES/c4ix32.c4r} \
            > "$OUT/$w.out" 2> "$OUT/$w.err"
        t1=$(date +%s.%N)
        s=$(echo "$t1 - $t0" | bc)
        if [ -z "$best" ] || [ "$(echo "$s < $best" | bc)" = 1 ]; then
            best=$s
            stats=$(sed -n 's/^libjs: \([0-9]*\) cycles in [0-9.]*s (\([0-9]*\)k inst\/s).*/\1 \2/p' "$OUT/$w.err")
            cp "$OUT/$w.out" "$OUT/$w.best"
        fi
        k=$((k + 1))
    done
    set -- $stats
    printf '%-7s %8.2f %12s %6s%s\n' $w $best "$1" "$(echo "scale=1; $2 / 1000" | bc)" \
        "$(grep -q 'page fault\|killed\|panic' "$OUT/$w.best" && echo '  (see '"$OUT/$w.best"')')"
done
