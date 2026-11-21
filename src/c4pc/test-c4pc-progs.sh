#!/bin/bash
# test-c4pc-progs.sh -- c4pc-host against libjs on the programs
# test-libjs runs byte for byte against native c4m (its part_native):
# the same output and the same status on both. Run by test-c4pc.
set -u
IMAGES=${IMAGES:-src/c4bb/images}
fail=0
ok () { echo "  ok    $*"; }
bad () { echo "  FAIL  $*"; fail=1; }
check () {
  local name=$1; shift
  local a b sa sb
  a=$(timeout 300 node libjs/cli.js --no-accel "$@" 2>&1); sa=$?
  b=$(timeout 300 ./c4pc-host "$@" 2>&1); sb=$?
  if [ "$a" = "$b" ] && [ $sa = $sb ]; then ok "$name (status $sa)"
  else bad "$name (status libjs $sa, c4pc $sb)"; diff <(echo "$a") <(echo "$b") | head -8; fi
}
for t in hello32 test_basic test_static test_vprintf tests multifun test-order test-ptrs \
         test_continue test_exit test_printloop mandel test_float cycles factorial test_malloc \
         bb_customop bb_preempt bb_pm gui-demo fb-demo libjs-strc test_printf; do
  [ -f $IMAGES/$t.c4r ] && check "$t" $IMAGES/$t.c4r || echo "  skip  $t (no image)"
done
check test_args $IMAGES/test_args.c4r arg1 arg2
check raycast $IMAGES/disk/raycast.c4r 21x21 -s 7 -d -n 20 -g 80x22
check "fused all ten" -m 96 -d $IMAGES/fused $IMAGES/fused/c4th32.c4r core.f asm.f fused.f
# the PIT runs on the host's clock: what it prints is the same, not its timing
out=$(timeout 120 ./c4pc-host -m 8 $IMAGES/bb_pit.c4r 2>&1)
if [ "$(echo "$out" | grep -c "real time")" = 2 ] && echo "$out" | grep -q "pit: held"; then ok "pit (free running, masked, held)"
else bad "pit"; echo "$out" | tail -4; fi
[ $fail = 0 ] && echo "test-c4pc-progs: OK" || { echo "test-c4pc-progs: FAILED"; exit 1; }
