#!/bin/bash
# test-quick.sh - the quick tier (make test-quick): only the parts of
# test-libjs.sh that the paths a change touched can reach, and
# test-c4ix-rebuild only when the toolchain that rebuild runs changed.
# The full gate (make c4bb-images test-c4ix test-c4ix-rebuild test-libjs)
# stays the one for a merge: this is for the edits before it.
#
#   bash libjs/tests/test-quick.sh [BASE]    (BASE: c4ix_and_toppings)
#   QUICK_DRY=1 ...                          says what it would run
#   QUICK_PATHS="a b" ...                    for those paths instead
#
# The change is everything since BASE's merge-base, committed or not,
# untracked files too. A path no rule knows runs everything.
set -u
cd "$(dirname "$0")/../.."
BASE=${1:-${QUICK_BASE:-c4ix_and_toppings}}
mb=$(git merge-base HEAD "$BASE") || { echo "test-quick: no merge-base with $BASE"; exit 2; }
paths=${QUICK_PATHS:-$( { git diff --name-only "$mb"; git ls-files --others --exclude-standard; } | sort -u)}

ALL="grin desktop ladder swap c4m ixbuild ixsh c4ix c4dos c4ke ixlib native ixpersist smp sound units maps"
IX="desktop c4ix c4m swap ixbuild ixsh ixlib ixpersist smp ladder"
parts="" rebuild=0 ix=0
add () { parts="$parts $*"; }
for f in $paths; do
    case "$f" in
    # what no test here reads: the docs, the page in a browser (make
    # test-libjs-web), and its own browser checks
    docs/*|*.md|libjs/web/*|libjs/tests/web-*.mjs|libjs/tests/cdp.mjs|libjs/tests/bench-*|libjs/tests/prof-*|\
    libjs/serve.mjs|libjs/page.js|libjs/display.js|libjs/worker.js|libjs/coi.js|libjs/uploads.js|libjs/site.sh|libjs/pages.sh) ;;
    libjs/tests/test-libjs.sh) add $ALL ;;
    libjs/tests/test-quick.sh) ;;
    libjs/tests/test-rebuild.sh) rebuild=1 ;;
    libjs/tests/test-grin.mjs) add grin ;;
    libjs/tests/test-desktop.mjs|libjs/tests/test-boot-desktop.mjs|libjs/tests/test-desktop-doom.mjs) add desktop ;;
    libjs/tests/test-c4dos-ladder.mjs) add ladder ;;
    libjs/tests/test-c4dos-heap.mjs|libjs/tests/test-persist.mjs) add c4dos ;;
    libjs/tests/test-c4ix-swap.mjs|libjs/tests/test-c4ix-par.mjs) add swap ;;
    libjs/tests/test-c4ix-c4m.mjs) add c4m ;;
    libjs/tests/test-c4ix-*.mjs) add c4ix ;;
    libjs/tests/test-*.mjs|libjs/tests/src/*) add units sound ;;
    # b4ke, cc and the build files are what the rebuild runs
    src/c4ix/user/b4ke.c|src/c4ix/user/cc.c|*BUILD.B4K|*INSTALL.B4K) add $IX; ix=1; rebuild=1 ;;
    # the desktop and its programs: what test-desktop drives
    src/c4ix/user/desktop/*|src/c4ix/user/gui.c|src/c4ix/art/*) add desktop; ix=1 ;;
    # the rest of C4IX: everything booted on it
    src/c4ix/*) add $IX; ix=1 ;;
    src/c4ke/*|src/c4sh/*) add c4ke c4m ladder ;;
    src/c4dos/*|src/grin/*|src/winkit/*) add grin c4dos ladder ;;
    games/*|src/opentyrian/*) add grin c4dos ladder desktop ;;
    src/c4mp/*) add smp swap ;;
    # the compilers and the machine: every image, and the rebuild
    src/c4sp/*|src/c4sc/*|src/c4cc/*) add $ALL; ix=1; rebuild=1 ;;
    *) add $ALL; ix=1; rebuild=1 ;;      # libjs's engine, the Makefile, build-images.sh, anything else
    esac
done
parts=$(for p in $parts; do echo $p; done | sort -u | xargs)
targets=""
[ $ix = 1 ] && targets="$targets test-c4ix"
[ $rebuild = 1 ] && targets="$targets test-c4ix-rebuild"
[ -n "$parts" ] && targets="$targets test-libjs"

echo "test-quick: $(echo "$paths" | grep -c .) paths changed since $BASE ($(git rev-parse --short $mb))"
echo "test-quick: make${targets:- nothing}${parts:+ (the parts of test-libjs: $parts)}"
[ -n "${QUICK_DRY:-}" ] && exit 0
[ -z "$targets" ] && { echo "test-quick: OK (no test reaches these paths)"; exit 0; }
PARTS="$parts" make $targets
