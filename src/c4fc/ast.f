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
\ A string literal carries its BYTES, not a data offset: the offset is
\ handed out during emission, and a literal inside a branch the tree
\ pass deletes must never be handed one at all.
NODE: n_str    NFIELD: >off  NFIELD: >slen     ;NODE
NODE: n_var    NFIELD: >sym                    ;NODE
NODE: n_gvar   NFIELD: >sym                    ;NODE
NODE: n_asgn   NFIELD: >lhs  NFIELD: >rhs      ;NODE
NODE: n_bin    NFIELD: >lhs  NFIELD: >rhs  NFIELD: >op ;NODE
NODE: n_call   NFIELD: >fn   NFIELD: >args NFIELD: >argn ;NODE
\ (a, b, c) -- the comma operator, and only inside parentheses, which is
\ where C puts it everywhere it is not a separator. va_arg is written
\ with one, so the whole of stdarg.h needs it.
\ >fn is unused and present so that >args and >argn keep n_call's
\ offsets: the field names are shared deliberately (see dsl.f), so a
\ kind that spells one differently silently rewrites it for every kind
\ compiled after it.
NODE: n_comma  NFIELD: >fn NFIELD: >args NFIELD: >argn ;NODE
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
NODE: n_member NFIELD: >lhs  NFIELD: >moff NFIELD: >mtype NFIELD: >magg ;NODE

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
\ A switch carries the range its jump table covers. WHERE the table
\ lives is decided during emission, not here: data addresses are handed
\ out in generation order, and a switch the tree pass deletes must not
\ have taken one.
NODE: n_switch NFIELD: >cond NFIELD: >body
               NFIELD: >lo   NFIELD: >hi                   ;NODE
NODE: n_case   NFIELD: >val                                ;NODE
NODE: n_default                                            ;NODE
NODE: n_break                                  ;NODE
NODE: n_cont                                   ;NODE
NODE: n_empty                                  ;NODE

NODE: n_ret    NFIELD: >expr                   ;NODE
\ A cast emits nothing and changes everything: (char *)p is the same
\ address and a different type, and the type is what decides LC against
\ LI, SC against SI, and whether p[i] scales by one or by eight.
NODE: n_cast   NFIELD: >expr NFIELD: >ctype     ;NODE
\ A function's name used as a value: its address. Not an lvalue, and no
\ load follows it -- the same shape an array name has.
NODE: n_fnref  NFIELD: >sym                    ;NODE
\ A local's initialiser, which is CODE: it runs every time the block is
\ entered, which is the whole difference between a local and a global
\ and the reason two calls to the same function see fresh values.
\ >icount is -1 for a scalar and the element count for an array; >ivn is
\ how many values were actually supplied, the rest being zero.
NODE: n_linit  NFIELD: >expr NFIELD: >isym NFIELD: >ivals
               NFIELD: >ivn  NFIELD: >icount NFIELD: >ibyte ;NODE
NODE: n_expst  NFIELD: >expr                   ;NODE
NODE: n_blk    NFIELD: >list NFIELD: >len      ;NODE

\ Constructors for the one- and two-field shapes, which is most of them.
: N1 ( v tag -- n )   2 CELLS NEW TUCK 1 CELLS + ! ;
: N2 ( a b tag -- n ) 3 CELLS NEW {: a b n -- n :}
   a n 1 CELLS + !  b n 2 CELLS + !  n ;

GENERIC: GEN                            \ an expression, as a value
GENERIC: GEN-ADDR                       \ an expression, as an address
GENERIC: STMT                           \ a statement
GENERIC: CT                             \ an expression's C type

\ char is 0, int is 1, and every * adds two -- c4's own encoding. The
\ only question codegen asks of a type is "is it char", because that is
\ LC/SC against LI/SI, and it is asked of the lvalue.
0 CONSTANT t_char   1 CONSTANT t_int

\ Symbol classes. They live here rather than with the parser because gen
\ reads them too: what a call compiles to -- an opcode, a JSR, a JSRI or
\ a JSRS -- is decided entirely by the class of the name being called.
0 CONSTANT c_glo   1 CONSTANT c_fun   2 CONSTANT c_builtin   3 CONSTANT c_loc
4 CONSTANT c_const
\ Object mode only: declared here, defined in another unit. The value
\ field holds the extern index rather than an address.
5 CONSTANT c_ext   6 CONSTANT c_extg
