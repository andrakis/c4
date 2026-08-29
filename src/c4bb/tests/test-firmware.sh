#!/bin/bash
# test-firmware.sh -- the firmware in its four stages.
#
# The pin for docs/c4bb-storage.md M6. A player builds their machine up
# to a BIOS, and a rung has to be DENIED the hardware it has not built
# yet -- otherwise the milestone where they build it is a milestone they
# were given for free. So each stage must do its own job and refuse the
# next one's, and this checks both halves for all four.
set -e
cd "$(dirname "$0")/../../.."

CLI="node src/c4bb/sim/cli.js"
FW=src/c4bb/fw
DISK=src/c4bb/images/climb
T=.bbfw
rm -rf $T; mkdir -p $T

fail=0
run () { timeout 120 $CLI -m 16 -fw "$1" -d $DISK > $T/$1.log 2>&1 || true; }
has () { grep -q -- "$2" $T/$1.log || { echo "test-firmware: $1 should say '$2' FAILED"; fail=1; }; }
hasnt () { ! grep -q -- "$2" $T/$1.log || { echo "test-firmware: $1 must NOT say '$2' FAILED"; fail=1; }; }

# Every stage is alive: the UART works, which is the first thing the
# player ever builds.
for s in hello ram drives bios; do
    run $s
    has $s "c4bb -- the breadboard computer"
done

# ...and each one stops exactly where its hardware does.
hasnt hello  "RAM ok"                  # no memory probe
has   hello  "cannot read a drive"
hasnt hello  "drive 0 has"

has   ram    "RAM ok"                  # a memory probe, and nothing past it
has   ram    "cannot read a drive"
hasnt ram    "drive 0 has"

has   drives "RAM ok"                  # it can SEE the medium...
has   drives "drive 0 has c4dos32.c4r"
has   drives "cannot load an image"    # ...and cannot start it
hasnt drives "booting"

has   bios   "drive 0 has c4dos32.c4r" # and the finished machine boots
has   bios   "booting c4dos32.c4r"
has   bios   "C4DOS version"

# The missing parts are MISSING, not skipped: a stage that still carried
# the loader and declined to call it would be a firmware pretending, and
# the player could find it with a hex editor. Sizes, smallest first.
sizes=$(for f in fw-hello fw-ram fw-drives fw; do stat -c%s $FW/$f.c4r; done)
sorted=$(printf '%s\n' $sizes | sort -n)
if [ "$(printf '%s\n' $sizes)" = "$sorted" ]; then
    echo "test-firmware: each stage is bigger than the last ($(echo $sizes | tr ' ' '<')) OK"
else
    echo "test-firmware: stage sizes are not in order ($sizes) FAILED"
    fail=1
fi

# And all four stay at rung base: this is all before the CPU is extended.
node src/c4bb/tools/opscan.mjs -q -rung base \
    $FW/fw-hello.c4r $FW/fw-ram.c4r $FW/fw-drives.c4r $FW/fw.c4r > /dev/null \
    || { echo "test-firmware: a stage reaches above rung base FAILED"; fail=1; }

if [ $fail = 0 ]; then
    rm -rf $T
    echo "test-firmware: OK -- four stages, each doing its own job and"
    echo "                    refusing the next one's"
else
    echo "test-firmware: FAILURES -- logs in $T"
    exit 1
fi
