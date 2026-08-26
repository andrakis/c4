\ c4fc gen.f -- code emission, one method per node kind.
\
\ Nothing here dispatches. A kind reaches a phase by having a method in
\ it, and a kind that cannot be assigned to simply has no GEN-ADDR --
\ which is the whole lvalue rule, enforced by the method table rather
\ than by a flag.

:M GEN n_num   >val @ oIMM OP2, ;M
:M GEN n_str   {: n -- :}  n >off @ n >slen @ D-STR, IMMD, ;M

\ A variable's symbol carries the LEA operand it was given: positive for
\ a parameter, negative for a local. c4 addresses both the same way, so
\ one method covers both and the symbol table is where the difference
\ lives.
: LOAD, ( ct -- )   t_char = IF oLC OP, ELSE oLI OP, THEN ;
: STORE, ( ct -- )  t_char = IF oSC OP, ELSE oSI OP, THEN ;

\ An AGGREGATE -- an array, or a struct variable -- is a name that
\ stands for its own address, so its value IS the LEA and there is no
\ load. That one flag is the whole of array decay.
:M GEN-ADDR n_var  >sym @ y.val @ oLEA OP2, ;M
:M GEN      n_var {: n | y -- :}
   n >sym @ TO y   y y.val @ oLEA OP2,
   y y.agg @ IF EXIT THEN   y y.ct @ LOAD, ;M
\ An initialised global's address is final the moment it is declared,
\ because region 1 begins at zero; every other global is a slot number
\ until the end of the program.
: GADDR, ( y -- ) {: y -- :}
   y y.class @ c_extg = IF oIMM y y.val @ EXTREF, EXIT THEN
   y y.ini @ IF y y.val @ IMMI, ELSE y y.val @ IMMG, THEN ;
\ Calling through a POINTER, which is what a threaded Forth spends its
\ life doing: JSRI through a global, JSRS through a frame slot. Only a
\ name whose class is fun or ext is a JSR to a known address.
: GJSRI, ( y -- ) {: y -- :}
   y y.ini @ IF oJSRI y y.val @ DOP, ELSE oJSRI y y.val @ GOP, THEN ;
:M GEN-ADDR n_gvar >sym @ GADDR, ;M
:M GEN      n_gvar {: n | y -- :}
   n >sym @ TO y   y GADDR,
   y y.agg @ IF EXIT THEN   y y.ct @ LOAD, ;M

: FNADDR, ( y -- ) {: y -- :}
   y y.class @ c_ext = IF oIMM y y.val @ EXTREF, EXIT THEN
   oIMM y FREF, ;
:M GEN      n_fnref  >sym @ FNADDR, ;M
:M GEN-ADDR n_fnref  >sym @ FNADDR, ;M
:M CT       n_fnref  >sym @ y.ct @ ;M

:M GEN      n_cast >expr @ GEN ;M
:M GEN-ADDR n_cast >expr @ GEN-ADDR ;M
:M CT       n_cast >ctype @ ;M

:M CT n_num  DROP t_int ;M
:M CT n_str  DROP 2 ;M                  \ char *
:M CT n_var  >sym @ y.ct @ ;M
:M CT n_gvar >sym @ y.ct @ ;M
:M CT n_bin {: n | lt -- :}
   n >lhs @ CT TO lt
   n >op @ oADD = lt T-PTR? AND IF lt EXIT THEN
   n >op @ oSUB = lt T-PTR? AND IF
      n >rhs @ CT lt = IF t_int EXIT THEN  lt EXIT THEN
   t_int ;M
:M CT n_call >fn @ y.ct @ ;M
:M CT n_asgn >lhs @ CT ;M

\ -mcisc: LXI and SXI fold scale-add-load (or -store) into one opcode,
\ but only for `var[expr]` whose base is a PLAIN VARIABLE of statically
\ known 8-byte scalar element type. Anything more complex -- a member, a
\ dereference, a call -- falls back, because establishing its type here
\ would mean either evaluating it twice or duplicating the inference
\ that codegen already does as a side effect. Deliberately the same
\ conservatism as c4lc's, so the two agree.
: CISC-INDEX? ( n -- f ) {: n | b t -- f :}
   CISC @ 0= IF 0 EXIT THEN
   n >TAG n_index <> IF 0 EXIT THEN
   n >lhs @ TO b
   b >TAG n_var <> b >TAG n_gvar <> AND IF 0 EXIT THEN
   b CT TO t
   t T-PTR? 0= IF 0 EXIT THEN
   t T-DEREF TO t
   t T-STRUCT? IF 0 EXIT THEN
   t t_char <> ;

:M GEN n_asgn {: n -- :}
   \ base and index both stay on the stack, unscaled, until the value is
   \ evaluated -- SXI wants all three, and computing the address first
   \ would need a slot to hold it across the right-hand side.
   n >lhs @ CISC-INDEX? IF
      n >lhs @ >lhs @ GEN  oPSH OP,
      n >lhs @ >rhs @ GEN  oPSH OP,
      n >rhs @ GEN  oSXI OP, EXIT THEN
   n >lhs @ GEN-ADDR  oPSH OP,
   n >rhs @ GEN
   n >lhs @ CT STORE, ;M

\ Pointer arithmetic scales by what the pointer points AT, and a step of
\ one emits no multiply at all -- which is not an optimisation but what
\ c4lc does: char * walks byte by byte with no MUL in sight.
: SCALE, ( t -- )   T-STEP DUP 1 = IF DROP EXIT THEN oPSH OP, oIMM OP2, oMUL OP, ;
: STEP-OF ( t -- n ) DUP T-PTR? IF T-STEP ELSE DROP 1 THEN ;

:M GEN n_bin {: n | lt op -- :}
   n >lhs @ CT TO lt   n >op @ TO op
   op oADD = lt T-PTR? AND IF
      n >lhs @ GEN oPSH OP,  n >rhs @ GEN  lt SCALE,  oADD OP, EXIT THEN
   op oSUB = lt T-PTR? AND IF
      n >rhs @ CT lt = IF                \ pointer minus pointer is a count
         n >lhs @ GEN oPSH OP,  n >rhs @ GEN  oSUB OP,
         lt T-STEP DUP 1 = IF DROP EXIT THEN
         oPSH OP, oIMM OP2, oDIV OP, EXIT THEN
      n >lhs @ GEN oPSH OP,  n >rhs @ GEN  lt SCALE,  oSUB OP, EXIT THEN
   n >lhs @ GEN  oPSH OP,  n >rhs @ GEN  op OP, ;M

:M GEN-ADDR n_index {: n -- :}
   n >lhs @ GEN  oPSH OP,
   n >rhs @ GEN  n >lhs @ CT SCALE,
   oADD OP, ;M
:M GEN n_index {: n -- :}
   n CISC-INDEX? IF
      n >lhs @ GEN  oPSH OP,  n >rhs @ GEN  oLXI OP, EXIT THEN
   n GEN-ADDR  n CT LOAD, ;M
:M CT  n_index  >lhs @ CT T-DEREF ;M

:M GEN-ADDR n_member {: n -- :}
   n >lhs @ GEN
   n >moff @ ?DUP IF oPSH OP, oIMM OP2, oADD OP, THEN ;M
:M GEN n_member {: n -- :}
   n GEN-ADDR
   n >magg @ IF EXIT THEN               \ an array member is its address
   n >mtype @ LOAD, ;M
:M CT  n_member  >mtype @ ;M

\ Arguments push left to right, then the call, then the drop. A builtin
\ is the same shape with an opcode where the JSR goes -- which is why
\ printf needs no special case anywhere else.
\ Calling a variadic function: push everything, push how many of them
\ were EXTRA, call __c4cc_make_va, drop the count and the extras, and
\ push what it returned. The callee sees its fixed parameters plus one
\ more -- which is why `int vsum(int n, ...)` finds n at bp+3 and not
\ bp+2, and why the ... needs no prologue of its own.
\ Every operand is evaluated, in order, and the value is the last one's
\ -- so the type is the last one's too.
:M CT n_comma {: n -- t :}  n >args @ n >argn @ 1- CELLS + @ CT ;M
:M GEN n_comma {: n | v -- :}
   n >args @ TO v
   n >argn @ 0 ?DO I CELLS v + @ GEN LOOP ;M

\ What a call compiles to is decided entirely by the CLASS of the name
\ being called: an opcode for a builtin, JSR to a known address, JSRI
\ through a global, JSRS through a frame slot, and an unresolved
\ reference for anything this unit only declares.
: CALL, ( y -- ) {: f -- :}
   f y.class @ c_builtin = IF f y.val @ OP, EXIT THEN
   f y.class @ c_glo     = IF f GJSRI, EXIT THEN
   f y.class @ c_loc     = IF f y.val @ oJSRS OP2, EXIT THEN
   f y.class @ c_ext     = IF oJSR  f y.val @ EXTREF, EXIT THEN
   f y.class @ c_extg    = IF oJSRI f y.val @ EXTREF, EXIT THEN
   f JSRF, ;

:M GEN n_call {: n | f k -- :}
   n >fn @ TO f
   n >argn @ TO k
   k 0 ?DO  n >args @ I CELLS + @ GEN  oPSH OP,  LOOP
   f y.va @ IF
      k f y.nfix @ - TO k                \ how many were extra
      k oIMM OP2,  oPSH OP,
      VA-MAKE @ 0= IF
         ." c4fc: a variadic call needs __c4cc_make_va -- include stdarg.h"
         CR ABORT THEN
      VA-MAKE @ CALL,
      k 1+ oADJ OP2,
      oPSH OP,
      f CALL,
      f y.nfix @ 1+ oADJ OP2,
      EXIT
   THEN
   f CALL,
   k ?DUP IF oADJ OP2, THEN ;M

\ A scalar takes SI even when it is a char, which is what c4cc did and
\ what c4lc kept; an array is stored element by element, the elements
\ past the initialiser list zeroed.
:M STMT n_linit {: n | y off v c s val -- :}
   n >isym @ TO y   y y.val @ TO off
   n >icount @ TO s
   s 0< IF
      off oLEA OP2,  oPSH OP,  n >expr @ GEN  oSI OP,  EXIT THEN
   n >ivals @ TO v   n >ivn @ TO c
   s 0 ?DO
      I c < IF I CELLS v + @ ELSE 0 THEN TO val
      n >ibyte @ IF
         off oLEA OP2,  oPSH OP,  I oIMM OP2,  oADD OP,  oPSH OP,
         val oIMM OP2,  oSC OP,
      ELSE
         off I + oLEA OP2,  oPSH OP,  val oIMM OP2,  oSI OP,
      THEN
   LOOP ;M

:M STMT n_expst  >expr @ GEN ;M
:M STMT n_ret    >expr @ ?DUP IF GEN THEN  oLEV OP, ;M
:M STMT n_blk {: n -- :}
   n >len @ 0 ?DO n >list @ I CELLS + @ STMT LOOP ;M

\ -- unary --------------------------------------------------------------
\ Each is what c4 emits, and each is worth reading once: ! is a compare
\ against zero, ~ is an XOR with -1, and unary minus is a multiply by -1
\ with the -1 pushed FIRST, because C4's MUL takes its left operand off
\ the stack.

:M GEN n_not   >opnd @ GEN  oPSH OP,  0 oIMM OP2,  oEQ  OP, ;M
:M GEN n_bnot  >opnd @ GEN  oPSH OP, -1 oIMM OP2,  oXOR OP, ;M
:M GEN n_neg   -1 oIMM OP2, oPSH OP,  >opnd @ GEN  oMUL OP, ;M

\ A pointer's VALUE is the address it points at, so dereferencing for an
\ address is the operand's value and nothing else.
:M GEN-ADDR n_deref  >opnd @ GEN ;M
:M GEN      n_deref {: n -- :}  n >opnd @ GEN  n CT LOAD, ;M
:M GEN      n_addr   >opnd @ GEN-ADDR ;M

:M CT n_not   DROP t_int ;M
:M CT n_bnot  DROP t_int ;M
:M CT n_neg   DROP t_int ;M
:M CT n_addr  >opnd @ CT 2 + ;M
:M CT n_deref >opnd @ CT 2 - ;M

\ ++ and -- reuse one address for both the load and the store, which is
\ why the sequence is LEA, PSH, LI rather than LEA, LI, PSH. The postfix
\ forms undo the change on the RESULT afterwards, which is exactly how
\ c4 gets the old value without a temporary.
: INCDEC, ( node op -- ) {: n op | t -- :}
   n >opnd @ CT TO t
   n >opnd @ GEN-ADDR  oPSH OP,
   t LOAD,
   oPSH OP,  t STEP-OF oIMM OP2,  op OP,
   t STORE, ;
:M GEN n_preinc   oADD INCDEC, ;M
:M GEN n_predec   oSUB INCDEC, ;M
:M GEN n_postinc {: n -- :}
   n oADD INCDEC,  oPSH OP, n >opnd @ CT STEP-OF oIMM OP2, oSUB OP, ;M
:M GEN n_postdec {: n -- :}
   n oSUB INCDEC,  oPSH OP, n >opnd @ CT STEP-OF oIMM OP2, oADD OP, ;M
:M CT n_preinc   >opnd @ CT ;M
:M CT n_predec   >opnd @ CT ;M
:M CT n_postinc  >opnd @ CT ;M
:M CT n_postdec  >opnd @ CT ;M

\ -- short-circuit and the conditional ---------------------------------

:M GEN n_lor {: n | m -- :}
   n >lhs @ GEN   oBNZ BR, TO m   n >rhs @ GEN   m >RES ;M
:M GEN n_land {: n | m -- :}
   n >lhs @ GEN   oBZ  BR, TO m   n >rhs @ GEN   m >RES ;M
:M GEN n_cond {: n | m1 m2 -- :}
   n >cond @ GEN   oBZ BR, TO m1
   n >body @ GEN   oJMP BR, TO m2
   m1 >RES
   n >else @ GEN
   m2 >RES ;M
:M CT n_lor  DROP t_int ;M
:M CT n_land DROP t_int ;M
:M CT n_cond >body @ CT ;M

\ -- statements ---------------------------------------------------------
\ break and continue are forward branches recorded on two stacks and
\ resolved when the loop that owns them closes. continue is forward even
\ in a while loop, where its target is behind it -- a mark is a hole to
\ fill, and filling it with an address already known is the same work.

1024 CONSTANT MMAX
CREATE BRKM MMAX CELLS ALLOT   VARIABLE BRKN   0 BRKN !
CREATE CNTM MMAX CELLS ALLOT   VARIABLE CNTN   0 CNTN !
: BRK, ( -- )  oJMP BR, BRKM BRKN @ CELLS + !  1 BRKN +! ;
: CNT, ( -- )  oJMP BR, CNTM CNTN @ CELLS + !  1 CNTN +! ;
: RESOLVE-LOOP ( brkbase cntbase brktarget cnttarget -- ) {: bb cb bt ct -- :}
   BRKN @ bb ?DO BRKM I CELLS + @ bt RESTO LOOP   bb BRKN !
   CNTN @ cb ?DO CNTM I CELLS + @ ct RESTO LOOP   cb CNTN ! ;

:M STMT n_break  DROP BRK, ;M
:M STMT n_cont   DROP CNT, ;M
:M STMT n_empty  DROP ;M

:M STMT n_if {: n | m1 m2 -- :}
   n >cond @ GEN   oBZ BR, TO m1
   n >body @ STMT
   n >else @ IF
      oJMP BR, TO m2   m1 >RES   n >else @ STMT   m2 >RES
   ELSE
      m1 >RES
   THEN ;M

:M STMT n_while {: n | top m bb cb -- :}
   BRKN @ TO bb   CNTN @ TO cb
   CHERE TO top
   n >cond @ GEN   oBZ BR, TO m
   n >body @ STMT
   oJMP top BACK,
   m >RES
   bb cb CHERE top RESOLVE-LOOP ;M

:M STMT n_do {: n | top m bb cb cont -- :}
   BRKN @ TO bb   CNTN @ TO cb
   CHERE TO top
   n >body @ STMT
   CHERE TO cont
   n >cond @ GEN
   oBNZ top BACK,
   bb cb CHERE cont RESOLVE-LOOP ;M

\ continue in a for loop goes to the STEP, not to the condition, and the
\ step is emitted after the body -- so it is a genuine forward branch.
:M STMT n_for {: n | top m bb cb cont -- :}
   BRKN @ TO bb   CNTN @ TO cb
   n >init @ ?DUP IF GEN THEN
   CHERE TO top
   -1 TO m                              \ patch 0 is a real patch index
   n >cond @ ?DUP IF GEN  oBZ BR, TO m THEN
   n >body @ STMT
   CHERE TO cont
   n >step @ ?DUP IF GEN THEN
   oJMP top BACK,
   m 0< 0= IF m >RES THEN
   bb cb CHERE cont RESOLVE-LOOP ;M

\ -- switch -------------------------------------------------------------
\ A jump table, not a chain of compares, because that is what c4lc emits:
\
\    <expr>                     JMP dispatch
\    <case bodies, each labelled where it starts>
\    JMP end                    -- the fall-out of the last case
\  dispatch:
\    PSH IMM lo SUB             -- index = value - lowest case
\    PSH PSH PSH                -- three copies: two compares and the index
\    IMM hi-lo GT  BNZ oob1
\    IMM 0     LT  BNZ oob2
\    IMM 8 MUL PSH IMM table ADD LI JMPA
\  oob1: ADJ 2 JMP default      -- each path drops what its compare left
\  oob2: ADJ 1 JMP default
\  end:
\
\ Entries with no case of their own hold the default target, which is the
\ end when there is no default at all.

BEGIN-STRUCTURE SWC
   FIELD: w.tab  FIELD: w.lo  FIELD: w.hi  FIELD: w.def  FIELD: w.ent
END-STRUCTURE
VARIABLE CURSW   0 CURSW !

:M STMT n_case {: n | w i -- :}
   CURSW @ TO w
   w 0= IF ." c4fc: case outside a switch" CR ABORT THEN
   n >val @ w w.lo @ - TO i
   i 0< i w w.hi @ w w.lo @ - > OR IF
      ." c4fc: case " n >val @ .N ." is outside its switch's range " CR ABORT THEN
   CHERE  w w.ent @  i CELLS +  ! ;M
:M STMT n_default {: n | w -- :}
   CURSW @ TO w
   w 0= IF ." c4fc: default outside a switch" CR ABORT THEN
   CHERE w w.def ! ;M

:M STMT n_switch {: n | w save m e bb m1 m2 m3 m4 end def k -- :}
   CURSW @ TO save
   SWC ALLOT: TO w
   n >lo @ w w.lo !  n >hi @ w w.hi !  -1 w w.def !
   n >hi @ n >lo @ - 1+ TO k
   k CELLS ALLOT: w w.ent !
   k 0 ?DO -1 w w.ent @ I CELLS + ! LOOP
   w CURSW !
   BRKN @ TO bb
   n >cond @ GEN
   oJMP BR, TO m
   n >body @ STMT
   oJMP BR, TO e                        \ the last case falls out here
   m >RES
   \ The table is allocated HERE, after the body and before the dispatch
   \ code, which is where c4lc puts it: a string literal inside the
   \ switch gets the lower address.
   D-ALIGN  k CELLS D-ALLOT w w.tab !
   \ Subtracting the lowest case is skipped when it is zero -- four
   \ words c4lc does not spend, and a difference invisible until a
   \ switch happens to start at case 0.
   n >lo @ ?DUP IF oPSH OP, oIMM OP2, oSUB OP, THEN
   oPSH OP,  oPSH OP,  oPSH OP,
   k 1- oIMM OP2,  oGT OP,   oBNZ BR, TO m1
   0 oIMM OP2,     oLT OP,   oBNZ BR, TO m2
   1 CELLS oIMM OP2,  oMUL OP,
   oPSH OP,  w w.tab @ IMMD,  oADD OP,
   oLI OP,   oJMPA OP,
   m1 >RES  2 oADJ OP2,  oJMP BR, TO m3
   m2 >RES  1 oADJ OP2,  oJMP BR, TO m4
   CHERE TO end
   e >RES
   w w.def @ 0< IF end ELSE w w.def @ THEN TO def
   m3 def RESTO   m4 def RESTO
   k 0 ?DO
      w w.tab @ I CELLS +
      w w.ent @ I CELLS + @ DUP 0< IF DROP def THEN
      TABPAT,
   LOOP
   BRKN @ bb ?DO BRKM I CELLS + @ end RESTO LOOP   bb BRKN !
   save CURSW ! ;M
