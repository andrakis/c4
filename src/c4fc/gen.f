\ c4fc gen.f -- code emission, one method per node kind.
\
\ Nothing here dispatches. A kind reaches a phase by having a method in
\ it, and a kind that cannot be assigned to simply has no GEN-ADDR --
\ which is the whole lvalue rule, enforced by the method table rather
\ than by a flag.

:M GEN n_num   >val @ oIMM OP2, ;M
:M GEN n_str   {: n -- :}  n >off @ n >slen @ D-STR, IMMD, ;M

\ -- M4: the helpers -----------------------------------------------------
\ Six small routines called where the VM has no opcode: a 16-bit load
\ (sign- or zero-extending), a 16-bit store, unsigned word division and
\ remainder, and a logical right shift by a run-time count. Each is laid
\ down ONCE, after the last function, and only if something called it;
\ a call is a JSR to an address in the same unit, so there is no symbol
\ and nothing for c4rlink to know. c4lc-gen.lisp's g:helpbody is the
\ same code, instruction for instruction.
0 CONSTANT h_ld16s  1 CONSTANT h_ld16u  2 CONSTANT h_st16
3 CONSTANT h_udiv   4 CONSTANT h_umod   5 CONSTANT h_lsr
CREATE HM-ID VEC ALLOT   CREATE HM-MK VEC ALLOT
: HELP-RESET ( -- )  HM-ID 64 VEC-INIT  HM-MK 64 VEC-INIT ;
HELP-RESET
\ the argument(s) are on the stack but for the one in A
: HCALL ( h n -- ) {: h n -- :}
   oPSH OP,  oJSR BR, HM-MK V,  h HM-ID V,  n oADJ OP2, ;
: HUSED? ( h -- f ) {: h -- f :}
   HM-ID V# 0 ?DO I HM-ID V@ h = IF -1 UNLOOP EXIT THEN LOOP 0 ;
: HRES ( h -- ) {: h -- :}               \ every call to it lands here
   HM-ID V# 0 ?DO I HM-ID V@ h = IF I HM-MK V@ >RES THEN LOOP ;

: L2, ( n -- )  oLEA OP2, ;
: I2, ( n -- )  oIMM OP2, ;
: H-LD16 ( signed -- ) {: sg -- :}
   0 oENT OP2,
   2 L2, oLI OP, oPSH OP, 1 I2, oADD OP, oLC OP,
   sg 0= IF oPSH OP, 255 I2, oAND OP, THEN
   oPSH OP, 8 I2, oSHL OP,
   oPSH OP, 2 L2, oLI OP, oLC OP, oPSH OP, 255 I2, oAND OP, oOR OP, oLEV OP, ;
: H-ST16 ( -- )
   0 oENT OP2,
   3 L2, oLI OP, oPSH OP, 2 L2, oLI OP, oSC OP,
   3 L2, oLI OP, oPSH OP, 1 I2, oADD OP, oPSH OP,
   2 L2, oLI OP, oPSH OP, 8 I2, oSHR OP, oSC OP,
   2 L2, oLI OP, oLEV OP, ;
: H-ULT, ( -- )                          \ A := stack (^MIN) vs b (^MIN), flag to come
   oPSH OP, WMIN I2, oXOR OP, oPSH OP, 2 L2, oLI OP, oPSH OP, WMIN I2, oXOR OP, ;
: H-UDIV ( -- ) {: | m1 m2 -- :}
   1 oENT OP2,
   2 L2, oLI OP, oPSH OP, 0 I2, oLT OP, oBZ BR, TO m1
   3 L2, oLI OP, H-ULT, oGE OP, oLEV OP,
   m1 >RES
   3 L2, oLI OP, oPSH OP, 0 I2, oGE OP, oBZ BR, TO m2
   3 L2, oLI OP, oPSH OP, 2 L2, oLI OP, oDIV OP, oLEV OP,
   m2 >RES
   -1 L2, oPSH OP, 3 L2, oLI OP, oPSH OP, 1 I2, oSHR OP,
   oPSH OP, WMIN INVERT I2, oAND OP, oPSH OP, 2 L2, oLI OP, oDIV OP,
   oPSH OP, 1 I2, oSHL OP, oSI OP,
   3 L2, oLI OP, oPSH OP, -1 L2, oLI OP, oPSH OP, 2 L2, oLI OP, oMUL OP, oSUB OP,
   H-ULT, oGE OP,
   oPSH OP, -1 L2, oLI OP, oADD OP, oLEV OP, ;
: H-UMOD ( -- ) {: | m1 m2 m3 m4 -- :}
   1 oENT OP2,
   2 L2, oLI OP, oPSH OP, 0 I2, oLT OP, oBZ BR, TO m1
   3 L2, oLI OP, H-ULT, oLT OP, oBZ BR, TO m2
   3 L2, oLI OP, oLEV OP,
   m2 >RES
   3 L2, oLI OP, oPSH OP, 2 L2, oLI OP, oSUB OP, oLEV OP,
   m1 >RES
   3 L2, oLI OP, oPSH OP, 0 I2, oGE OP, oBZ BR, TO m3
   3 L2, oLI OP, oPSH OP, 2 L2, oLI OP, oMOD OP, oLEV OP,
   m3 >RES
   -1 L2, oPSH OP, 3 L2, oLI OP, oPSH OP, 3 L2, oLI OP, oPSH OP, 1 I2, oSHR OP,
   oPSH OP, WMIN INVERT I2, oAND OP, oPSH OP, 2 L2, oLI OP, oDIV OP,
   oPSH OP, 1 I2, oSHL OP, oPSH OP, 2 L2, oLI OP, oMUL OP, oSUB OP, oSI OP,
   -1 L2, oLI OP, H-ULT, oLT OP, oBZ BR, TO m4
   -1 L2, oLI OP, oLEV OP,
   m4 >RES
   -1 L2, oLI OP, oPSH OP, 2 L2, oLI OP, oSUB OP, oLEV OP, ;
: H-LSR ( -- ) {: | m -- :}
   0 oENT OP2,
   2 L2, oLI OP, oBZ BR, TO m
   3 L2, oLI OP, oPSH OP, 2 L2, oLI OP, oSHR OP,
   oPSH OP, -1 I2, oPSH OP, WBITS I2, oPSH OP, 2 L2, oLI OP, oSUB OP,
   oSHL OP, oPSH OP, -1 I2, oXOR OP, oAND OP, oLEV OP,
   m >RES
   3 L2, oLI OP, oLEV OP, ;
: EMIT-HELPERS ( -- )
   h_ld16s HUSED? IF h_ld16s HRES -1 H-LD16 THEN
   h_ld16u HUSED? IF h_ld16u HRES  0 H-LD16 THEN
   h_st16  HUSED? IF h_st16  HRES H-ST16 THEN
   h_udiv  HUSED? IF h_udiv  HRES H-UDIV THEN
   h_umod  HUSED? IF h_umod  HRES H-UMOD THEN
   h_lsr   HUSED? IF h_lsr   HRES H-LSR  THEN ;

\ -- M4: loads, stores and conversions by type ----------------------------
: MASK, ( m -- )  oPSH OP, oIMM OP2, oAND OP, ;
: SEXT, ( n -- )  WBITS SWAP - DUP oPSH OP, oIMM OP2, oSHL OP, oPSH OP, oIMM OP2, oSHR OP, ;
: XMIN, ( -- )    oPSH OP, WMIN oIMM OP2, oXOR OP, ;

\ A variable's symbol carries the LEA operand it was given: positive for
\ a parameter, negative for a local. c4 addresses both the same way, so
\ one method covers both and the symbol table is where the difference
\ lives.
: LOAD, ( ct -- ) {: t -- :}
   t t_char = t t_schar = OR IF oLC OP, EXIT THEN
   t t_uchar = IF oLC OP, 255 MASK, EXIT THEN
   t t_short = IF h_ld16s 1 HCALL EXIT THEN
   t t_ushort = IF h_ld16u 1 HCALL EXIT THEN
   t t_uint = W64? AND IF oLI OP, M32 MASK, EXIT THEN
   oLI OP, ;
: STORE, ( ct -- ) {: t -- :}
   t T-BYTE? IF oSC OP, EXIT THEN
   t T-HALF? IF h_st16 2 HCALL EXIT THEN
   oSI OP, ;

\ The conversion code for a value of type f in A, to type t: only to an
\ integer type, and nothing when it already fits (c4lc's g:convert!).
: CONVERT, ( f t -- ) {: f t -- :}
   t T-ARITH? 0= IF EXIT THEN
   t t_char = t t_schar = OR IF f t_char = f t_schar = OR 0= IF 8 SEXT, THEN EXIT THEN
   t t_uchar = IF f t_uchar <> IF 255 MASK, THEN EXIT THEN
   t t_short = IF f T-BYTE? f t_short = OR 0= IF 16 SEXT, THEN EXIT THEN
   t t_ushort = IF f t_uchar = f t_ushort = OR 0= IF 65535 MASK, THEN EXIT THEN
   t t_uint = W64? AND IF
      f t_uchar = f t_ushort = OR f t_uint = OR 0= IF M32 MASK, THEN THEN ;
\ ... and the one an assignment, a ++ or a return makes. char keeps c4's
\ own rule (SC drops the rest, the value stays); the new types get C's.
: SCONV, ( f t -- ) {: f t -- :}
   t t_char = t t_int = OR t t_ulong = OR IF EXIT THEN
   f t CONVERT, ;

\ -- M4: const --------------------------------------------------------------
\ Worded exactly as c4lc-gen.lisp words them, because the diagnostics
\ battery holds the two to the same output. A warning prints only when
\ the image goes to a file (-o): c4th has no stderr, and on stdout it
\ would be part of the image.
\ FNA/FNU and RETT/RETQ, the function being generated and its return
\ type and mask, are in emit.f: the inliner swaps the latter too.
: FN-NAME! ( a u -- )  FNU ! FNA ! ;
: FN-RET! ( t q -- )   RETQ ! RETT ! ;
357913940 CONSTANT CQ1                  \ the const bits of levels 1..14
: .INFN ( -- )  ."  (in function " FNA @ FNU @ TYPE ." )" CR ;
: WCHECK ( lvalue a u -- ) {: n a u -- :}
   n CQ 1 AND IF ." error: " a u TYPE ."  of read-only location" .INFN ABORT THEN ;
: DROPPED? ( lq rq -- f )  SWAP INVERT AND CQ1 AND 0<> ;
: WARN? ( -- f )  COLLECT @ 0= OUTNAME @ 0<> AND ;
: QWARN ( a u -- )
   WARN? 0= IF 2DROP EXIT THEN
   ." warning: " TYPE ."  discards const qualifier" .INFN ;
\ argument i of a call to f, after it is generated
: ARGCHECK ( arg i f -- ) {: a i f -- :}
   f y.pt @ 0= IF EXIT THEN
   i f y.nfix @ < 0= IF EXIT THEN
   f y.pt @ i CELLS + @ T-PTR? 0= IF EXIT THEN
   f y.pq @ i CELLS + @  a CQ  DROPPED? 0= IF EXIT THEN
   WARN? 0= IF EXIT THEN
   ." warning: passing argument " i 1+ .N ."  of " f y.name @ f y.nlen @ TYPE
   ."  discards const qualifier" .INFN ;

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

:M GEN      n_cast {: n -- :}  n >expr @ GEN  n >expr @ CT n >ctype @ CONVERT, ;M
:M GEN-ADDR n_cast >expr @ GEN-ADDR ;M
:M CT       n_cast >ctype @ ;M
:M CQ       n_cast >cq @ ;M
:M CQ       n_fnref DROP 0 ;M

:M CT n_num  >nty @ ;M
:M CT n_str  DROP 2 ;M                  \ char *
:M CT n_var  >sym @ y.ct @ ;M
:M CT n_gvar >sym @ y.ct @ ;M
\ c4lc's g:typeof, rule for rule: c4 gives + and - the left operand's
\ type (a band type promotes), char * - char * stays a char *, and an
\ unsigned operand brings the usual arithmetic conversions.
\ Two locals, as before M4: this recurses down the left operand, and
\ c4th's locals stack is 512 cells with no check (see c4fc-design.md).
:M CT n_bin {: n | lt -- :}
   n >lhs @ CT TO lt
   n >op @ oADD = n >op @ oSUB = OR IF
      lt T-PTR? IF
         n >op @ oADD = IF lt EXIT THEN
         lt T-STEP 1 > n >rhs @ CT lt = AND IF t_int EXIT THEN  lt EXIT THEN
      lt n >rhs @ CT T-UNSBIN? IF lt n >rhs @ CT T-UAC EXIT THEN
      lt T-SUMTY EXIT THEN
   n >op @ oSHL = n >op @ oSHR = OR IF lt T-ARITH? IF lt T-UNRES ELSE t_int THEN EXIT THEN
   n >op @ CMPOP? IF t_int EXIT THEN
   lt n >rhs @ CT T-UNSBIN? IF lt n >rhs @ CT T-UAC ELSE t_int THEN ;M
:M CT n_call >fn @ y.ct @ ;M
:M CT n_icall DROP t_int ;M
:M CT n_asgn >lhs @ CT ;M

\ The qualifier mask of an expression (c4lc's g:qualof): two bits a
\ pointer level, level 0 the object the expression designates.
:M CQ n_num  DROP 0 ;M
:M CQ n_str  DROP 0 ;M
:M CQ n_bin  DROP 0 ;M
:M CQ n_var  >sym @ y.q @ ;M
:M CQ n_gvar >sym @ y.q @ ;M
:M CQ n_call >fn @ y.rq @ ;M
:M CQ n_icall DROP 0 ;M
:M CQ n_asgn >lhs @ CQ ;M

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
   t T-BAND0 < 0= IF 0 EXIT THEN        \ a band type needs a mask or a helper
   t t_char <> ;

\ struct = struct (M7b): the bytes, by the VM's own memcpy -- what a
\ builtin memcpy(&L, &R, sizeof L) compiles to, as c4lc-gen's
\ g:structassign. The right side must be an lvalue, so it has GEN-ADDR.
: STRUCT-ASGN ( n -- ) {: n -- :}
   n >lhs @ GEN-ADDR   oPSH OP,
   n >rhs @ GEN-ADDR   oPSH OP,
   n >lhs @ CT T-SIZE oIMM OP2,   oPSH OP,
   oMCPY OP,   3 oADJ OP2, ;
:M GEN n_asgn {: n -- :}
   n >lhs @ CT T-STRUCT? IF n STRUCT-ASGN EXIT THEN
   \ base and index both stay on the stack, unscaled, until the value is
   \ evaluated -- SXI wants all three, and computing the address first
   \ would need a slot to hold it across the right-hand side.
   n >lhs @ CISC-INDEX? IF
      n >lhs @ >lhs @ GEN  oPSH OP,
      n >lhs @ >rhs @ GEN  oPSH OP,
      n >rhs @ GEN  oSXI OP, EXIT THEN
   n >lhs @ GEN-ADDR
   n >lhs @ S" assignment" WCHECK
   oPSH OP,
   n >rhs @ GEN
   n >rhs @ CT  n >lhs @ CT  SCONV,
   n >lhs @ CT T-PTR? IF
      n >lhs @ CQ  n >rhs @ CQ  DROPPED? IF S" assignment" QWARN THEN THEN
   n >lhs @ CT STORE, ;M

\ Pointer arithmetic scales by what the pointer points AT, and a step of
\ one emits no multiply at all -- which is not an optimisation but what
\ c4lc does: char * walks byte by byte with no MUL in sight.
: SCALE, ( t -- )   T-STEP DUP 1 = IF DROP EXIT THEN oPSH OP, oIMM OP2, oMUL OP, ;
: STEP-OF ( t -- n ) DUP T-PTR? IF T-STEP ELSE DROP 1 THEN ;

\ M4: an unsigned operand. uint on a 64-bit host ("narrow") is kept in
\ [0, 2^32), so the signed opcodes are right once the other side of a
\ compare or a division is cut into range, and a result that can leave
\ the range is cut back. A whole unsigned word flips the sign bit for an
\ ordering, calls a helper to divide, and masks a right shift.
: NARROW? ( n -- f )                     \ the result is a 64-bit host's uint
   W64? 0= IF DROP 0 EXIT THEN
   DUP >op @ DUP oSHL = SWAP oSHR = OR IF >lhs @ CT T-PROMOTE ELSE
   DUP >lhs @ CT SWAP >rhs @ CT T-UAC THEN  t_uint = ;
: ORD? ( op -- f )  DUP CMPOP? OVER oEQ <> AND SWAP oNE <> AND ;
: CUT? ( op t -- f )                     \ narrow: must this operand be masked?
   T-PROMOTE t_uint <> SWAP DUP CMPOP? SWAP DUP oDIV = SWAP oMOD = OR OR AND ;
\ does this operator have an unsigned operand (after promotion)?
: UNSBIN? ( n -- f )
   DUP >op @ DUP oSHL = SWAP oSHR = OR IF
      >lhs @ CT DUP T-ARITH? IF T-PROMOTE T-UNS? ELSE DROP 0 THEN EXIT THEN
   DUP >lhs @ CT SWAP >rhs @ CT T-UNSBIN? ;

\ ONE word, no locals, and the node kept on the DATA stack. Code
\ generation recurses with the expression, through this word and GEN
\ alone, so this is two return-stack cells a level -- what it was before
\ M4 -- and no locals frame at all. c4th's return stack is 256 cells
\ and checked; its locals stack is 512 and not (c4fc-design.md), and a
\ frame per level is what a 60-term unsigned expression overflowed.
:M GEN n_bin
   DUP UNSBIN? IF
      \ an unsigned operand: the usual arithmetic conversions
      DUP >lhs @ GEN
      DUP NARROW? IF DUP >op @ OVER >lhs @ CT CUT? IF M32 MASK, THEN
      ELSE DUP >op @ ORD? IF XMIN, THEN THEN
      oPSH OP,
      DUP >rhs @ GEN
      DUP NARROW? IF
         DUP >op @ OVER >rhs @ CT CUT? IF M32 MASK, THEN
         >op @ DUP OP,
         DUP oADD = OVER oSUB = OR OVER oMUL = OR OVER oSHL = OR OVER oOR = OR
         SWAP oXOR = OR IF M32 MASK, THEN
         EXIT THEN
      DUP >op @ ORD? IF XMIN, THEN
      DUP >op @ oDIV = IF DROP h_udiv 2 HCALL EXIT THEN
      DUP >op @ oMOD = IF DROP h_umod 2 HCALL EXIT THEN
      DUP >op @ oSHR = IF
         >rhs @ DUP NUM? IF
            >val @ oSHR OP,
            DUP 0> OVER WBITS < AND IF -1 WBITS ROT - LSHIFT INVERT MASK, ELSE DROP THEN
         ELSE DROP h_lsr 2 HCALL THEN
         EXIT THEN
      >op @ OP, EXIT
   THEN
   \ c4's own operators, as they always were
   DUP >lhs @ CT T-PTR? IF
      DUP >op @ oADD = IF
         DUP >lhs @ GEN oPSH OP,  DUP >rhs @ GEN  >lhs @ CT SCALE,  oADD OP, EXIT THEN
      DUP >op @ oSUB = IF
         DUP >rhs @ CT OVER >lhs @ CT = IF   \ pointer minus pointer is a count
            DUP >lhs @ GEN oPSH OP,  DUP >rhs @ GEN  oSUB OP,
            >lhs @ CT T-STEP DUP 1 = IF DROP EXIT THEN
            oPSH OP, oIMM OP2, oDIV OP, EXIT THEN
         DUP >lhs @ GEN oPSH OP,  DUP >rhs @ GEN  >lhs @ CT SCALE,  oSUB OP, EXIT THEN
   THEN
   DUP >lhs @ GEN  oPSH OP,  DUP >rhs @ GEN  >op @ OP, ;M

:M GEN-ADDR n_index {: n -- :}
   n >lhs @ GEN  oPSH OP,
   n >rhs @ GEN  n >lhs @ CT SCALE,
   oADD OP, ;M
:M GEN n_index {: n -- :}
   n CISC-INDEX? IF
      n >lhs @ GEN  oPSH OP,  n >rhs @ GEN  oLXI OP, EXIT THEN
   n GEN-ADDR  n CT LOAD, ;M
:M CT  n_index  >lhs @ CT T-DEREF ;M
:M CQ  n_index  >lhs @ CQ 2 RSHIFT ;M

:M GEN-ADDR n_member {: n -- :}
   n >lhs @ GEN
   n >moff @ ?DUP IF oPSH OP, oIMM OP2, oADD OP, THEN ;M
:M GEN n_member {: n -- :}
   n GEN-ADDR
   n >magg @ IF EXIT THEN               \ an array member is its address
   n >mtype @ LOAD, ;M
:M CT  n_member  >mtype @ ;M
\ a member inherits the qualifiers of the struct it is read out of; an
\ ARRAY member hands them on to its elements. >lhs is the struct's
\ ADDRESS (x.m is (&x)->m), so the struct's own level is level 1 there.
:M CQ  n_member {: n | sq -- q :}
   n >lhs @ CQ 2 RSHIFT 3 AND TO sq
   n >mq @  n >magg @ IF sq 2 LSHIFT ELSE sq THEN OR ;M

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
:M CQ n_comma {: n -- q :}  n >args @ n >argn @ 1- CELLS + @ CQ ;M
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

:M GEN n_call {: n | f k i -- :}
   \ The splice replaces the call entirely -- arguments into the
   \ callee's parameter slots, then its body generated in place. The
   \ value arrives the same way it would have: in A.
   n INL-TRY TO i
   i 0< 0= IF  n i INL-SPLICE  EXIT THEN
   n >fn @ TO f
   n >argn @ TO k
   k 0 ?DO
      n >args @ I CELLS + @ DUP GEN  I f ARGCHECK
      oPSH OP,
   LOOP
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

\ A call through an expression (M7b), as c4lc-gen's g:callexpr: the
\ arguments, then what JSR would have pushed -- the return address, an
\ IMM of the code just past the jump -- then the target in A and JMPA.
\ The callee finds the frame a JSR builds.
:M GEN n_icall {: n | k m -- :}
   n >argn @ TO k
   k 0 ?DO  n >args @ I CELLS + @ GEN  oPSH OP,  LOOP
   oIMM BR, TO m   oPSH OP,
   n >fn @ GEN   oJMPA OP,
   m >RES
   k ?DUP IF oADJ OP2, THEN ;M

\ goto and labels (M7b), by name and per function, as c4lc-gen's g:ulab.
\ A label's address is -1 until it is placed; a goto that gets there
\ first leaves a branch mark, resolved when the label is placed. Placing
\ a label tells LABEL@ about it, so a function ending on one gets the
\ LEV that c4lc gives it.
32 CONSTANT ULAB-MAX
CREATE ULA ULAB-MAX CELLS ALLOT   CREATE ULU ULAB-MAX CELLS ALLOT
CREATE ULADDR ULAB-MAX CELLS ALLOT
VARIABLE ULN   0 ULN !
256 CONSTANT UFWD-MAX
CREATE UFL UFWD-MAX CELLS ALLOT   CREATE UFM UFWD-MAX CELLS ALLOT
VARIABLE UFN   0 UFN !
: ULAB-RESET ( -- )  0 ULN !  0 UFN ! ;
: ULAB ( a u -- i ) {: a u -- i :}
   ULN @ 0 ?DO
      I CELLS ULU + @ u = IF
         I CELLS ULA + @ a u BYTES= IF I UNLOOP EXIT THEN
      THEN
   LOOP
   ULN @ ULAB-MAX < 0= IF ." c4fc: too many labels in one function" CR ABORT THEN
   a ULN @ CELLS ULA + !  u ULN @ CELLS ULU + !  -1 ULN @ CELLS ULADDR + !
   1 ULN +!  ULN @ 1- ;
: ULAB-CHECK ( -- )
   UFN @ 0 ?DO
      I CELLS UFL + @ CELLS ULADDR + @ 0< IF
         ." c4fc: goto to a label the function does not have: "
         I CELLS UFL + @ DUP CELLS ULA + @ SWAP CELLS ULU + @ TYPE CR ABORT
      THEN
   LOOP ;
:M STMT n_goto {: n | i t -- :}
   n >lname @ n >llen @ ULAB TO i
   i CELLS ULADDR + @ TO t
   t 0< IF
      UFN @ UFWD-MAX < 0= IF ." c4fc: too many gotos in one function" CR ABORT THEN
      oJMP BR,  UFN @ CELLS UFM + !  i UFN @ CELLS UFL + !  1 UFN +!
   ELSE oJMP t BACK, THEN ;M
:M STMT n_labeled {: n | i -- :}
   n >lname @ n >llen @ ULAB TO i
   i CELLS ULADDR + @ 0< 0= IF
      ." c4fc: duplicate label: " n >lname @ n >llen @ TYPE CR ABORT THEN
   CHERE i CELLS ULADDR + !   CHERE LABEL@
   UFN @ 0 ?DO  I CELLS UFL + @ i = IF I CELLS UFM + @ >RES THEN  LOOP
   n >body @ STMT ;M

\ A scalar takes SI even when it is a char, which is what c4cc did and
\ what c4lc kept; an array is stored element by element, the elements
\ past the initialiser list zeroed.
: LIBYTE, ( off idx v -- ) {: off i v -- :}
   off oLEA OP2,  oPSH OP,  i oIMM OP2,  oADD OP,  oPSH OP,
   v oIMM OP2,  oSC OP, ;
:M STMT n_linit {: n | y off v c s val -- :}
   n >isym @ TO y   y y.val @ TO off
   n >icount @ TO s
   s 0< IF
      off oLEA OP2,  oPSH OP,  n >expr @ GEN
      y y.ct @ T-PTR? IF
         y y.q @  n >expr @ CQ  DROPPED? IF S" initialization" QWARN THEN THEN
      oSI OP,  EXIT THEN
   n >ivals @ TO v   n >ivn @ TO c
   s 0 ?DO
      I c < IF I CELLS v + @ ELSE 0 THEN TO val
      n >ibyte @ 1 = IF
         off I val LIBYTE,
      ELSE n >ibyte @ 2 = IF               \ a 16-bit element: two bytes, low first
         off I 2* val 255 AND LIBYTE,
         off I 2* 1+ val 8 RSHIFT 255 AND LIBYTE,
      ELSE
         off I + oLEA OP2,  oPSH OP,  val oIMM OP2,  oSI OP,
      THEN THEN
   LOOP ;M

\ c4lc-gen's g:localaggr: MSET zeroes the object, then each value goes to
\ its offset -- LEA's own when it is word-aligned, else added to the base
: LADDR, ( base off -- ) {: b o -- :}
   o 1 CELLS 1- AND 0= IF b o 1 CELLS / + oLEA OP2, EXIT THEN
   b oLEA OP2,  oPSH OP,  o oIMM OP2,  oADD OP, ;
:M STMT n_lagg {: n | b v r e -- :}
   n >lsym @ y.val @ TO b
   b oLEA OP2,  oPSH OP,  0 oIMM OP2,  oPSH OP,  n >lbytes @ oIMM OP2,  oPSH OP,
   oMSET OP,  3 oADJ OP2,
   n >lrecs @ TO v
   v V# 0 ?DO
      I v V@ TO r
      b r @ LADDR,  oPSH OP,
      r 2 CELLS + @ TO e
      e IF
         e GEN
         e CT T-STRUCT? IF ." c4fc: a struct value in an initializer is not supported" CR ABORT THEN
      ELSE r 3 CELLS + @ oIMM OP2, THEN
      r CELL+ @ STORE,
   LOOP ;M

:M STMT n_expst  >expr @ GEN ;M
\ Inside a spliced body there is no frame to leave, so a return is a
\ jump to the end of the splice rather than a LEV. The value is already
\ in A either way, which is the whole reason this needs no rewriting of
\ the tree -- see inline.f.
:M STMT n_ret {: n | e -- :}
   n >expr @ TO e
   e IF
      e GEN
      e CT RETT @ SCONV,
      RETT @ T-PTR? IF RETQ @ e CQ DROPPED? IF S" return" QWARN THEN THEN
   THEN
   INLRET @ IF oJMP BR, INLM, ELSE oLEV OP, THEN ;M
:M STMT n_blk {: n -- :}
   n >len @ 0 ?DO n >list @ I CELLS + @ STMT LOOP ;M

\ -- unary --------------------------------------------------------------
\ Each is what c4 emits, and each is worth reading once: ! is a compare
\ against zero, ~ is an XOR with -1, and unary minus is a multiply by -1
\ with the -1 pushed FIRST, because C4's MUL takes its left operand off
\ the stack.

:M GEN n_not   >opnd @ GEN  oPSH OP,  0 oIMM OP2,  oEQ  OP, ;M
\ an unsigned operand stays unsigned, and a 64-bit host's uint is cut
\ back into its 32 bits
: UNCUT, ( n -- )  >opnd @ CT T-UNRES t_uint = W64? AND IF M32 MASK, THEN ;
:M GEN n_bnot {: n -- :}  n >opnd @ GEN  oPSH OP, -1 oIMM OP2,  oXOR OP,  n UNCUT, ;M
:M GEN n_neg  {: n -- :}  -1 oIMM OP2, oPSH OP,  n >opnd @ GEN  oMUL OP,  n UNCUT, ;M

\ A pointer's VALUE is the address it points at, so dereferencing for an
\ address is the operand's value and nothing else.
:M GEN-ADDR n_deref  >opnd @ GEN ;M
:M GEN      n_deref {: n -- :}  n >opnd @ GEN  n CT LOAD, ;M
:M GEN      n_addr   >opnd @ GEN-ADDR ;M

:M CT n_not   DROP t_int ;M
:M CT n_bnot  >opnd @ CT T-UNRES ;M
:M CT n_neg   >opnd @ CT T-UNRES ;M
:M CT n_addr  >opnd @ CT 2 + ;M
:M CT n_deref >opnd @ CT 2 - ;M
:M CQ n_not   DROP 0 ;M
:M CQ n_bnot  DROP 0 ;M
:M CQ n_neg   DROP 0 ;M
:M CQ n_addr  >opnd @ CQ 2 LSHIFT ;M
:M CQ n_deref >opnd @ CQ 2 RSHIFT ;M

\ ++ and -- reuse one address for both the load and the store, which is
\ why the sequence is LEA, PSH, LI rather than LEA, LI, PSH. The postfix
\ forms undo the change on the RESULT afterwards, which is exactly how
\ c4 gets the old value without a temporary.
\ The post forms store the new value unconverted and take the step back
\ off it: converting first would lose the carry the undo needs, and the
\ loads convert anyway.
: INCDEC, ( node op post -- ) {: n op post | t -- :}
   n >opnd @ CT TO t
   n >opnd @ GEN-ADDR
   n >opnd @ op oADD = IF S" increment" ELSE S" decrement" THEN WCHECK
   oPSH OP,
   t LOAD,
   oPSH OP,  t STEP-OF oIMM OP2,  op OP,
   post 0= IF t T-PROMOTE t SCONV, THEN
   t STORE, ;
:M GEN n_preinc   oADD 0 INCDEC, ;M
:M GEN n_predec   oSUB 0 INCDEC, ;M
:M GEN n_postinc {: n -- :}
   n oADD 1 INCDEC,  oPSH OP, n >opnd @ CT STEP-OF oIMM OP2, oSUB OP, ;M
:M GEN n_postdec {: n -- :}
   n oSUB 1 INCDEC,  oPSH OP, n >opnd @ CT STEP-OF oIMM OP2, oADD OP, ;M
:M CT n_preinc   >opnd @ CT ;M
:M CT n_predec   >opnd @ CT ;M
:M CT n_postinc  >opnd @ CT ;M
:M CT n_postdec  >opnd @ CT ;M
:M CQ n_preinc   >opnd @ CQ ;M
:M CQ n_predec   >opnd @ CQ ;M
:M CQ n_postinc  >opnd @ CQ ;M
:M CQ n_postdec  >opnd @ CQ ;M

\ -- short-circuit and the conditional ---------------------------------

:M GEN n_lor {: n | m -- :}
   n >lhs @ GEN   oBNZ BR, TO m   n >rhs @ GEN   m >RES ;M
:M GEN n_land {: n | m -- :}
   n >lhs @ GEN   oBZ  BR, TO m   n >rhs @ GEN   m >RES ;M
\ c4's ?: has its else-arm's type; with an unsigned arm in it both arms
\ convert to the usual arithmetic type instead, as C says.
: CONDU? ( n -- f )  DUP >body @ CT SWAP >else @ CT T-UNSBIN? ;
: CONDT ( n -- t )   DUP >body @ CT SWAP >else @ CT T-UAC ;
:M GEN n_cond {: n | m1 m2 -- :}
   n >cond @ GEN   oBZ BR, TO m1
   n >body @ GEN
   n CONDU? IF n >body @ CT n CONDT CONVERT, THEN
   oJMP BR, TO m2
   m1 >RES
   n >else @ GEN
   n CONDU? IF n >else @ CT n CONDT CONVERT, THEN
   m2 >RES ;M
:M CT n_lor  DROP t_int ;M
:M CT n_land DROP t_int ;M
:M CT n_cond {: n -- t :}  n CONDU? IF n CONDT ELSE n >else @ CT THEN ;M
:M CQ n_lor  DROP 0 ;M
:M CQ n_land DROP 0 ;M
:M CQ n_cond {: n -- q :}  n >body @ CQ  n >else @ CQ  OR ;M

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
