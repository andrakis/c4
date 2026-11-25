\ SEE: what a Forth definition compiles to as C4 VM code.
\
\ The accumulator model is visible in the listing and worth reading once.
\ DROP is `IMM 0; ADD` -- a = *sp++ + 0 -- because C4's ALU already pops
\ the stack, so dropping needs no opcode of its own. And a flag is
\ `LT; PSH; IMM -1; MUL`, because Forth wants -1 for true where C4's
\ comparisons give 1.

: SQ    DUP * ;
: ABS3  DUP 0< IF NEGATE THEN ;

." : SQ DUP * ;" CR
1 SEE SQ
." : ABS3 DUP 0< IF NEGATE THEN ;" CR
1 SEE ABS3

\ With locals.f loaded, EXIT is an IMMEDIATE word that COMPILES the
\ original one -- so a body ends with an xt no longer reachable by that
\ name. native.f used to look for `' EXIT`, find nothing, and report
\ every body as EMPTY; the inliner then compiled nothing and said it had
\ succeeded. This listing is the check: if that regressed, SQL below
\ would disassemble to ENT and LEV and nothing in between.
: SQL {: n -- r :}  n n * ;
." : SQL {: n -- r :}  n n * ;" CR
1 SEE SQL
