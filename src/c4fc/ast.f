\ c4fc ast.f -- the node kinds, and the phases over them.
\
\ Each kind is one NODE: row. Each phase is a GENERIC:, and a kind joins
\ a phase by having a method rather than by appearing in a chain.
\
\ Two generics for expressions, not one with a flag: GEN emits a VALUE,
\ GEN-ADDR emits an ADDRESS. Only the kinds that can be assigned to
\ implement GEN-ADDR, so "is this an lvalue" is answered by the method
\ table rather than by a predicate somebody has to maintain.

NODE: n_num    NFIELD: >val                    ;NODE
NODE: n_str    NFIELD: >off                    ;NODE
NODE: n_var    NFIELD: >sym                    ;NODE
NODE: n_gvar   NFIELD: >sym                    ;NODE
NODE: n_asgn   NFIELD: >lhs  NFIELD: >rhs      ;NODE
NODE: n_bin    NFIELD: >lhs  NFIELD: >rhs  NFIELD: >op ;NODE
NODE: n_call   NFIELD: >fn   NFIELD: >args NFIELD: >argn ;NODE
\ Unary operators. Each is one row here, one GEN method, and (where it
\ can be assigned through) one GEN-ADDR.
NODE: n_not    NFIELD: >opnd                   ;NODE
NODE: n_bnot   NFIELD: >opnd                   ;NODE
NODE: n_neg    NFIELD: >opnd                   ;NODE
NODE: n_deref  NFIELD: >opnd                   ;NODE
NODE: n_addr   NFIELD: >opnd                   ;NODE
NODE: n_preinc NFIELD: >opnd                   ;NODE
NODE: n_predec NFIELD: >opnd                   ;NODE
NODE: n_postinc NFIELD: >opnd                  ;NODE
NODE: n_postdec NFIELD: >opnd                  ;NODE

\ Short-circuit operators are control flow, not arithmetic: a || b is a
\ branch around b, which is why they are node kinds of their own rather
\ than rows in the infix table.
\ a[i] and x.m / p->m. There is one member kind, not two: x.m is (&x)->m
\ once the parser has wrapped the base, so the node always holds an
\ address-producing expression and codegen never asks which spelling it
\ came from.
NODE: n_index  NFIELD: >lhs  NFIELD: >rhs      ;NODE
NODE: n_member NFIELD: >lhs  NFIELD: >moff NFIELD: >mtype ;NODE

NODE: n_lor    NFIELD: >lhs  NFIELD: >rhs      ;NODE
NODE: n_land   NFIELD: >lhs  NFIELD: >rhs      ;NODE
NODE: n_cond   NFIELD: >cond NFIELD: >body NFIELD: >else ;NODE

\ Statements. The field ORDER is chosen so that shared names keep
\ shared offsets -- do-while stores its condition first even though it
\ evaluates it last, so that >cond means cell 1 everywhere.
NODE: n_if     NFIELD: >cond NFIELD: >body NFIELD: >else ;NODE
NODE: n_while  NFIELD: >cond NFIELD: >body                ;NODE
NODE: n_do     NFIELD: >cond NFIELD: >body                ;NODE
NODE: n_for    NFIELD: >cond NFIELD: >body NFIELD: >init NFIELD: >step ;NODE
\ A switch carries where its jump table lives and the range it covers;
\ the table's CONTENTS are code addresses, filled in as the case labels
\ are reached during codegen.
NODE: n_switch NFIELD: >cond NFIELD: >body NFIELD: >tab
               NFIELD: >lo   NFIELD: >hi                   ;NODE
NODE: n_case   NFIELD: >val                                ;NODE
NODE: n_default                                            ;NODE
NODE: n_break                                  ;NODE
NODE: n_cont                                   ;NODE
NODE: n_empty                                  ;NODE

NODE: n_ret    NFIELD: >expr                   ;NODE
NODE: n_expst  NFIELD: >expr                   ;NODE
NODE: n_blk    NFIELD: >list NFIELD: >len      ;NODE

GENERIC: GEN                            \ an expression, as a value
GENERIC: GEN-ADDR                       \ an expression, as an address
GENERIC: STMT                           \ a statement
GENERIC: CT                             \ an expression's C type

\ char is 0, int is 1, and every * adds two -- c4's own encoding. The
\ only question codegen asks of a type is "is it char", because that is
\ LC/SC against LI/SI, and it is asked of the lvalue.
0 CONSTANT t_char   1 CONSTANT t_int
