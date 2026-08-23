#!/bin/sh
# The B5c opcode probe's measuring half: how many of the instructions a
# workload ACTUALLY EXECUTES would a given set of fused opcodes remove?
#
# A greedy left-to-right peephole over the executed instruction stream,
# which is a different and more interesting question than a static count
# over an image -- the hot code is a small part of any image.
#
# The instrumentation is generated into a throwaway copy of c4m.c rather
# than living in it. c4m.c is compiled by PLAIN C4 as well as by gcc, and
# plain c4 skips only the "#" line itself (c4.c:74), so both arms of an
# #ifdef end up compiled there -- an #ifdef'd probe using long long,
# fprintf and function-pointer parameters would break `./c4 ./c4m.c`.
#
# Usage:  sh src/c4th/bench/fuse-probe.sh
set -e
cd "$(dirname "$0")/../../.."
OUT=${TMPDIR:-/tmp}/c4m-fuse
python3 - "$OUT.c" <<'PY'
import sys
src = open('c4m.c').read()
src = src.replace("#include <stdio.h>", "#include <stdio.h>\n#include <stdlib.h>", 1)
src = src.replace("char *c4m_opcodes;", "long long fu_tot, fu_saveA, fu_saveB;\nvoid fu_feed (int i);\nchar *c4m_opcodes;", 1)

PROBE = r'''
int fuA_q[4], fuA_n, fuB_q[4], fuB_n;
int fu_isalu (int x) {
  return x == ADD || x == SUB || x == MUL || x == DIV || x == MOD
      || x == AND || x == OR  || x == XOR || x == SHL || x == SHR
      || x == EQ  || x == NE  || x == LT  || x == GT || x == LE || x == GE;
}
/* Set A: the five opcodes the probe actually built into c4m and measured
   end to end. STL and POPA replace patterns that are not adjacent pairs,
   so this UNDERSTATES set A. */
int fuA_match (int *q, int n) {
  if (n >= 3 && q[0] == PSH && q[1] == IMM && (q[2] == ADD || q[2] == MUL)) return 3;
  if (n >= 2 && q[0] == LEA && q[1] == LI) return 2;
  return 0;
}
/* Set B: what the instruction profile actually asks for -- load local,
   load global, push local, push global, and the whole immediate-ALU
   family. Not built; this is the estimate that says whether it is worth
   building. */
int fuB_match (int *q, int n) {
  if (n >= 3 && q[0] == PSH && q[1] == IMM && fu_isalu(q[2])) return 3;
  if (n >= 3 && q[0] == LEA && q[1] == LI && q[2] == PSH) return 3;
  if (n >= 3 && q[0] == IMM && q[1] == LI && q[2] == PSH) return 3;
  if (n >= 2 && q[0] == LEA && q[1] == LI) return 2;
  if (n >= 2 && q[0] == IMM && q[1] == LI) return 2;
  if (n >= 2 && q[0] == LEA && q[1] == PSH) return 2;
  return 0;
}
void fu_step (int *q, int *np, long long *save, int (*m)(int *, int), int x) {
  int k, j;
  q[(*np)++] = x;
  if (*np < 3) return;
  k = m(q, *np);
  if (k) { *save = *save + (k - 1); *np = 0;
           for (j = k; j < 3; ++j) q[(*np)++] = q[j]; }
  else   { for (j = 1; j < 3; ++j) q[j-1] = q[j]; *np = 2; }
}
void fu_feed (int i) {
  fu_step(fuA_q, &fuA_n, &fu_saveA, fuA_match, i);
  fu_step(fuB_q, &fuB_n, &fu_saveB, fuB_match, i);
}
void fu_report () {
  if (!fu_tot) return;
  fprintf(stderr, "FUSE: %lld instructions executed\n", fu_tot);
  fprintf(stderr, "FUSE:   set A (LDL, ADDI, MULI)                   removes %lld = %.2f%%\n",
          fu_saveA, 100.0 * fu_saveA / fu_tot);
  fprintf(stderr, "FUSE:   set B (LDL LDG PSHL PSHG + ALU-immediate) removes %lld = %.2f%%\n",
          fu_saveB, 100.0 * fu_saveB / fu_tot);
}

// types
enum { CHAR, INT, PTR };'''
assert "\n// types\nenum { CHAR, INT, PTR };" in src
src = src.replace("\n// types\nenum { CHAR, INT, PTR };", PROBE, 1)

anchor = "    i = *pc++;"
assert anchor in src
src = src.replace(anchor, anchor + "\n    if (i >= 0 && i < INS_SIZE) { ++fu_tot; fu_feed(i); }", 1)
src = src.replace("int main(int argc, char **argv) {",
                  "int main(int argc, char **argv) {\n  atexit(fu_report);", 1)
open(sys.argv[1], 'w').write(src)
PY
gcc -O2 -fwrapv -g -idirafter include -I . "$OUT.c" c4m_float.c -o "$OUT" -lm 2>/dev/null
echo "--- c4cc compiling c4.c ---"
"$OUT" load-c4r.c -- c4cc.c4r -o "$OUT.c4r" c4.c > /dev/null 2>>/dev/stderr || true
echo "--- c4sp -R running c4lc's lexer over c4.c ---"
"$OUT" load-c4r.c -- c4sp.c4r -R -c 8000000 src/c4sp/lisp/c4lc-tokens.lisp -count c4.c > /dev/null
