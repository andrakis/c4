\ c4fc gen.f -- code emission, one method per node kind.
\
\ Nothing here dispatches. A kind reaches a phase by having a method in
\ it, and a kind that cannot be assigned to simply has no GEN-ADDR --
\ which is the whole lvalue rule, enforced by the method table rather
\ than by a flag.

:M GEN n_num   >val @ oIMM OP2, ;M
:M GEN n_str   >off @ IMMD, ;M

\ A variable's symbol carries the LEA operand it was given: positive for
\ a parameter, negative for a local. c4 addresses both the same way, so
\ one method covers both and the symbol table is where the difference
\ lives.
: LOAD, ( ct -- )   t_char = IF oLC OP, ELSE oLI OP, THEN ;
: STORE, ( ct -- )  t_char = IF oSC OP, ELSE oSI OP, THEN ;

:M GEN-ADDR n_var  >sym @ y.val @ oLEA OP2, ;M
:M GEN      n_var  >sym @ DUP y.val @ oLEA OP2, y.ct @ LOAD, ;M
:M GEN-ADDR n_gvar >sym @ y.val @ IMMG, ;M
:M GEN      n_gvar >sym @ DUP y.val @ IMMG, y.ct @ LOAD, ;M

:M CT n_num  DROP t_int ;M
:M CT n_str  DROP 2 ;M                  \ char *
:M CT n_var  >sym @ y.ct @ ;M
:M CT n_gvar >sym @ y.ct @ ;M
:M CT n_bin  DROP t_int ;M
:M CT n_call DROP t_int ;M
:M CT n_asgn >lhs @ CT ;M

:M GEN n_asgn {: n -- :}
   n >lhs @ GEN-ADDR  oPSH OP,
   n >rhs @ GEN
   n >lhs @ CT STORE, ;M

:M GEN n_bin {: n -- :}
   n >lhs @ GEN  oPSH OP,
   n >rhs @ GEN
   n >op @ OP, ;M

\ Arguments push left to right, then the call, then the drop. A builtin
\ is the same shape with an opcode where the JSR goes -- which is why
\ printf needs no special case anywhere else.
:M GEN n_call {: n | f -- :}
   n >fn @ TO f
   n >argn @ 0 ?DO
      n >args @ I CELLS + @ GEN  oPSH OP,
   LOOP
   f y.class @ 2 = IF f y.val @ OP, ELSE f y.val @ JSRC, THEN
   n >argn @ ?DUP IF oADJ OP2, THEN ;M

:M STMT n_expst  >expr @ GEN ;M
:M STMT n_ret    >expr @ GEN  oLEV OP, ;M
:M STMT n_blk {: n -- :}
   n >len @ 0 ?DO n >list @ I CELLS + @ STMT LOOP ;M
