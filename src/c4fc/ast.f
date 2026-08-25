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
