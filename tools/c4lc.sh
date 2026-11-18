# tools/c4lc.sh -- sourced, from the top of c4's tree, by the test scripts
# that compile with c4lc. It picks the host's c4lc the way the Makefile's
# $(call lc64), $(call lc32) and $(call c4opt32) do: compiled by c4sc
# (c4sc-host, c4sc-host32), unless C4LC64 or C4LC32, or C4LC for both,
# says interp. The two write the same bytes (make test-c4sc32).
#
#   lc 64|32 [CELLS]   prints the command, which takes c4lc.lisp's arguments
#   c4opt 64|32        prints c4opt-run.lisp's, or c4sc-host's opt mode
#   lc_bin 64|32       prints the binary that runs it, to test for
#
# CELLS is c4sp's heap (default 400000). The compiled c4lc's arena grows, so
# it starts at its default and takes no CELLS.
lc_mode () {
  local m
  if [ "$1" = 32 ]; then m=${C4LC32:-${C4LC:-compiled}}; else m=${C4LC64:-${C4LC:-compiled}}; fi
  case $m in
    interp|compiled) echo $m ;;
    *) echo "tools/c4lc.sh: C4LC$1 is \"compiled\" or \"interp\", not \"$m\"" >&2; echo compiled ;;
  esac
}
lc_bin () {
  local w=; [ "$1" = 32 ] && w=32
  if [ $(lc_mode $1) = interp ]; then echo ./c4sp$w; else echo ./c4sc-host$w; fi
}
lc () {
  local w=; [ "$1" = 32 ] && w=32
  if [ $(lc_mode $1) = interp ]; then echo "./c4sp$w -c ${2:-400000} src/c4sp/lisp/c4lc.lisp"
  else echo "./c4sc-host$w compile"; fi
}
c4opt () {
  local w=; [ "$1" = 32 ] && w=32
  if [ $(lc_mode $1) = interp ]; then echo "./c4sp$w src/c4sp/lisp/c4opt-run.lisp"
  else echo "./c4sc-host$w opt"; fi
}
