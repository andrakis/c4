\ c4fc: the DSL, exercised. docs/c4fc-design.md.
\
\ The last section is the point of the whole file: adding a construct is
\ one NODE: line and one :M per phase, and NOTHING ABOVE IT IS EDITED.
\ That is the claim the design makes, checked rather than asserted.

65536 ARENA-INIT

\ -- Forth-2012 the kernel does not carry ------------------------------

: CLASSIFY ( n -- )
   CASE
      0 OF ." zero"  ENDOF
      1 OF ." one"   ENDOF
      2 OF ." two"   ENDOF
      ." many"
   ENDCASE SPACE ;
0 CLASSIFY 1 CLASSIFY 2 CLASSIFY 7 CLASSIFY CR

DEFER GREET
: HELLO ." hello " ;
: GOODBYE ." goodbye " ;
' HELLO IS GREET     GREET
' GOODBYE IS GREET   GREET   CR

BEGIN-STRUCTURE POINT  FIELD: p.x  FIELD: p.y  END-STRUCTURE
CREATE P POINT ALLOT   3 P p.x !  4 P p.y !
P p.x @ . P p.y @ . POINT . CR

5 VALUE COUNT   COUNT .  9 TO COUNT  COUNT . CR

\ -- named locals -------------------------------------------------------

: ATOI {: a u -- n :}  0  u 0 ?DO 10 * a I + C@ 48 - + LOOP ;
S" 90210" ATOI . CR

: HYPOT2 {: x y | s -- n :}  x x *  TO s  s y y * +  ;
3 4 HYPOT2 . CR

\ Recursion is the reason locals exist rather than scratch VARIABLEs.
: FACT {: n -- n! :}  n 2 < IF 1 EXIT THEN  n  n 1- RECURSE * ;
10 FACT . CR

\ -- vectors ------------------------------------------------------------

CREATE TOKS VEC ALLOT   TOKS 64 VEC-INIT
: FILL-TOKS  10 0 ?DO I DUP * TOKS V, LOOP ;
FILL-TOKS
TOKS V# .  3 TOKS V@ .  9 TOKS V@ . CR

\ -- nodes, and two phases over them ------------------------------------

NODE: n_num  NFIELD: >val            ;NODE
NODE: n_add  NFIELD: >lhs NFIELD: >rhs ;NODE
NODE: n_mul  NFIELD: >lhs NFIELD: >rhs ;NODE

: NUM ( v -- n )        n_num 2 CELLS NEW TUCK >val ! ;
: BIN ( a b tag -- n )  3 CELLS NEW {: a b n -- n :} a n >lhs ! b n >rhs ! n ;

GENERIC: EVAL                        \ phase one: fold it
GENERIC: SHOW                        \ phase two: print it

:M EVAL n_num  >val @ ;M
:M EVAL n_add  {: n -- v :} n >lhs @ EVAL  n >rhs @ EVAL + ;M
:M EVAL n_mul  {: n -- v :} n >lhs @ EVAL  n >rhs @ EVAL * ;M

:M SHOW n_num  >val @ . ;M
:M SHOW n_add  {: n -- :} ." (+ " n >lhs @ SHOW n >rhs @ SHOW ." ) " ;M
:M SHOW n_mul  {: n -- :} ." (* " n >lhs @ SHOW n >rhs @ SHOW ." ) " ;M

: E1 ( -- n )  2 NUM 3 NUM n_add BIN  4 NUM n_mul BIN ;
E1 SHOW  E1 EVAL . CR

\ -- and now a new construct, added the way every C feature will be -----
\ One NODE:, one :M per phase. Nothing above this line changed.

NODE: n_sub  NFIELD: >lhs NFIELD: >rhs ;NODE
:M EVAL n_sub  {: n -- v :} n >lhs @ EVAL  n >rhs @ EVAL - ;M
:M SHOW n_sub  {: n -- :} ." (- " n >lhs @ SHOW n >rhs @ SHOW ." ) " ;M

NODE: n_neg  NFIELD: >opnd ;NODE
: UN ( a tag -- n )  2 CELLS NEW TUCK >opnd ! ;
:M EVAL n_neg  >opnd @ EVAL NEGATE ;M
:M SHOW n_neg  {: n -- :} ." (- " n >opnd @ SHOW ." ) " ;M

: E2 ( -- n )  E1  7 NUM n_sub BIN  n_neg UN ;
E2 SHOW  E2 EVAL . CR
