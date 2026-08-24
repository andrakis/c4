#!/bin/sh
# How many of the instructions a workload ACTUALLY EXECUTES would a given
# set of fused opcodes remove, and which of them earn their keep?
#
# A greedy left-to-right peephole over the executed instruction stream,
# which is a different and more interesting question than a static count
# over an image -- the hot code is a small part of any image. Each rule
# reports how much it removes IN THE SET, so a rule whose pattern is
# always swallowed by a longer one shows up as worth nothing.
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
src = src.replace("char *c4m_opcodes;",
                  "long long fu_tot;\nvoid fu_feed (int i);\nchar *c4m_opcodes;", 1)

PROBE = r'''
/* Candidate rules, longest first: a greedy match takes the first that
   fits, so a three-instruction fusion always wins over the two it
   contains. RULES is {length, op0, op1, op2, saving}. */
#define FU_NRULE 26
int fu_rop[FU_NRULE][3];
int fu_rlen[FU_NRULE];
char *fu_rname[FU_NRULE];
long long fu_rhit[FU_NRULE];
int fu_ninit;

void fu_rule (int k, int len, int a, int b, int c, char *nm) {
  fu_rlen[k] = len; fu_rop[k][0] = a; fu_rop[k][1] = b; fu_rop[k][2] = c;
  fu_rname[k] = nm;
}
void fu_rules_init () {
  int k;
  k = 0;
  /* three-instruction fusions */
  fu_rule(k++, 3, PSH, IMM, ADD, "ADDI  a = a + n         (PSH IMM ADD)");
  fu_rule(k++, 3, PSH, IMM, SUB, "SUBI  a = a - n         (PSH IMM SUB)");
  fu_rule(k++, 3, PSH, IMM, MUL, "MULI  a = a * n         (PSH IMM MUL)");
  fu_rule(k++, 3, PSH, IMM, DIV, "DIVI  a = a / n         (PSH IMM DIV)");
  fu_rule(k++, 3, PSH, IMM, MOD, "MODI  a = a % n         (PSH IMM MOD)");
  fu_rule(k++, 3, PSH, IMM, AND, "ANDI  a = a & n         (PSH IMM AND)");
  fu_rule(k++, 3, PSH, IMM, OR,  "ORI   a = a | n         (PSH IMM OR)");
  fu_rule(k++, 3, PSH, IMM, XOR, "XORI  a = a ^ n         (PSH IMM XOR)");
  fu_rule(k++, 3, PSH, IMM, SHL, "SHLI  a = a << n        (PSH IMM SHL)");
  fu_rule(k++, 3, PSH, IMM, SHR, "SHRI  a = a >> n        (PSH IMM SHR)");
  fu_rule(k++, 3, PSH, IMM, EQ,  "EQI   a = a == n        (PSH IMM EQ)");
  fu_rule(k++, 3, PSH, IMM, NE,  "NEI   a = a != n        (PSH IMM NE)");
  fu_rule(k++, 3, PSH, IMM, LT,  "LTI   a = a < n         (PSH IMM LT)");
  fu_rule(k++, 3, PSH, IMM, GT,  "GTI   a = a > n         (PSH IMM GT)");
  fu_rule(k++, 3, PSH, IMM, LE,  "LEI   a = a <= n        (PSH IMM LE)");
  fu_rule(k++, 3, PSH, IMM, GE,  "GEI   a = a >= n        (PSH IMM GE)");
  fu_rule(k++, 3, LEA, LI,  PSH, "PSHL  push local n      (LEA LI PSH)");
  fu_rule(k++, 3, IMM, LI,  PSH, "PSHG  push global n     (IMM LI PSH)");
  fu_rule(k++, 3, LEA, LC,  PSH, "PSHLC push local char   (LEA LC PSH)");
  fu_rule(k++, 3, IMM, LC,  PSH, "PSHGC push global char  (IMM LC PSH)");
  /* two-instruction fusions */
  fu_rule(k++, 2, LEA, LI,  -1,  "LDL   a = *(bp+n)       (LEA LI)");
  fu_rule(k++, 2, IMM, LI,  -1,  "LDG   a = *(int *)n     (IMM LI)");
  fu_rule(k++, 2, LEA, PSH, -1,  "LEAP  push bp+n         (LEA PSH)");
  fu_rule(k++, 2, IMM, PSH, -1,  "IMMP  push n            (IMM PSH)");
  fu_rule(k++, 2, LI,  PSH, -1,  "LIP   a = *a; push a    (LI PSH)");
  fu_rule(k++, 2, ADD, LI,  -1,  "ADDL  a = *(*sp++ + a)  (ADD LI)");
  fu_ninit = k;
}

int fu_q[4], fu_n;
void fu_feed (int i) {
  int r, k, j, m;
  if (!fu_ninit) fu_rules_init();
  fu_q[fu_n++] = i;
  if (fu_n < 3) return;
  m = -1;
  for (r = 0; r < fu_ninit; ++r) {
    k = fu_rlen[r];
    if (fu_rop[r][0] == fu_q[0] && fu_rop[r][1] == fu_q[1]
        && (k == 2 || fu_rop[r][2] == fu_q[2])) { m = r; break; }
  }
  if (m >= 0) {
    ++fu_rhit[m];
    k = fu_rlen[m];
    fu_n = 0;
    for (j = k; j < 3; ++j) fu_q[fu_n++] = fu_q[j];
  } else {
    for (j = 1; j < 3; ++j) fu_q[j-1] = fu_q[j];
    fu_n = 2;
  }
}

void fu_report () {
  int r; long long saved, s;
  if (!fu_tot) return;
  saved = 0;
  for (r = 0; r < fu_ninit; ++r) saved = saved + fu_rhit[r] * (fu_rlen[r] - 1);
  fprintf(stderr, "FUSE: %lld instructions executed; the set below removes %lld = %.2f%%\n",
          fu_tot, saved, 100.0 * saved / fu_tot);
  for (r = 0; r < fu_ninit; ++r) {
    s = fu_rhit[r] * (fu_rlen[r] - 1);
    fprintf(stderr, "FUSE:   %-34s %10lld hits  %6.2f%%\n",
            fu_rname[r], fu_rhit[r], 100.0 * s / fu_tot);
  }
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
"$OUT" load-c4r.c -- c4cc.c4r -o "$OUT.c4r" c4.c > /dev/null
echo "--- c4sp -R running c4lc's lexer over c4.c ---"
"$OUT" load-c4r.c -- c4sp.c4r -R -c 8000000 src/c4sp/lisp/c4lc-tokens.lisp -count c4.c > /dev/null
