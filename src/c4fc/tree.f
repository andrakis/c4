\ c4fc tree.f -- the AST passes (c4lc L6), run by -O before code
\ emission. Two things the peephole optimizer structurally cannot see,
\ because by the time it runs they are not there any more:
\
\   T1 CONSTANT FOLDING. Arithmetic on literals, ?: and if/while/for
\      with constant conditions reduced to the branch that runs, and
\      && / || with a constant left side collapsed. The peephole pass
\      folds `IMM 3; PSH; IMM 4; MUL` but cannot delete the BRANCH
\      around an arm that was never going to run, nor the code in it.
\
\   T2 DEAD FUNCTION ELIMINATION, in parse.f, where the two passes over
\      the token stream live.
\
\ && and || use c4's EXACT result semantics, which are not the
\ normalising ones: `a && b` yields b's VALUE when a is truthy, and
\ `a || b` yields a's VALUE when a is truthy. Neither reduces to 1, so
\ neither may be folded as though it did.
\
\ One method per node kind, as every phase here is. A kind that does not
\ fold says so by returning itself, which is also what a new kind gets
\ for free the day someone adds one -- FOLD has no default, so a missing
\ method is a named abort rather than a wrong answer.

\ -O. It lives here rather than with the driver because the parser has
\ to ask about it: the tree passes run between parsing a function body
\ and generating it.
VARIABLE OPTIMIZE   0 OPTIMIZE !

GENERIC: FOLD ( node -- node' )

: NUM? ( n -- f )    >TAG n_num = ;
: MKNUM ( v -- n )   n_num N1 ;
: EMPTY-STMT ( -- n ) n_empty 1 CELLS NEW ;

VARIABLE NFOLD   0 NFOLD !
: FOLDED ( -- )  1 NFOLD +! ;

\ -- the kinds that are already as small as they get ---------------------
\ &x, ++x and x++ are left alone rather than descended into, which is
\ what c4lc does: their operand is an lvalue, and folding inside one
\ could only ever turn it into something that is not.

:M FOLD n_num     ;M
:M FOLD n_str     ;M
:M FOLD n_var     ;M
:M FOLD n_gvar    ;M
:M FOLD n_fnref   ;M
:M FOLD n_addr    ;M
:M FOLD n_preinc  ;M
:M FOLD n_predec  ;M
:M FOLD n_postinc ;M
:M FOLD n_postdec ;M
:M FOLD n_case    ;M
:M FOLD n_default ;M
:M FOLD n_break   ;M
:M FOLD n_cont    ;M
:M FOLD n_empty   ;M

\ -- arithmetic ----------------------------------------------------------

:M FOLD n_bin {: n | a b v ok -- n :}
   n >lhs @ FOLD DUP n >lhs !  TO a
   n >rhs @ FOLD DUP n >rhs !  TO b
   a NUM? b NUM? AND IF
      a >val @  b >val @  n >op @ FOLD1 TO ok TO v
      ok IF FOLDED v MKNUM EXIT THEN
   THEN
   n ;M

:M FOLD n_not  {: n | a -- n :}
   n >opnd @ FOLD DUP n >opnd ! TO a
   a NUM? IF FOLDED a >val @ 0= 1 AND MKNUM EXIT THEN  n ;M
:M FOLD n_bnot {: n | a -- n :}
   n >opnd @ FOLD DUP n >opnd ! TO a
   a NUM? IF FOLDED a >val @ -1 XOR MKNUM EXIT THEN  n ;M
:M FOLD n_neg  {: n | a -- n :}
   n >opnd @ FOLD DUP n >opnd ! TO a
   a NUM? IF FOLDED 0 a >val @ - MKNUM EXIT THEN  n ;M

\ -- the ones that only descend ------------------------------------------

:M FOLD n_asgn   {: n -- n :}  n >rhs @ FOLD n >rhs !  n ;M
:M FOLD n_cast   {: n -- n :}  n >expr @ FOLD n >expr !  n ;M
:M FOLD n_deref  {: n -- n :}  n >opnd @ FOLD n >opnd !  n ;M
:M FOLD n_member {: n -- n :}  n >lhs @ FOLD n >lhs !  n ;M
:M FOLD n_index  {: n -- n :}
   n >lhs @ FOLD n >lhs !  n >rhs @ FOLD n >rhs !  n ;M
: FOLD-ARGS ( n -- ) {: n | v -- :}
   n >args @ TO v
   n >argn @ 0 ?DO I CELLS v + @ FOLD  I CELLS v + ! LOOP ;
:M FOLD n_call  {: n -- n :}  n FOLD-ARGS  n ;M
:M FOLD n_comma {: n -- n :}  n FOLD-ARGS  n ;M

\ -- control flow inside an expression ------------------------------------

:M FOLD n_cond {: n | c -- n :}
   n >cond @ FOLD DUP n >cond ! TO c
   c NUM? IF
      FOLDED
      c >val @ IF n >body @ FOLD ELSE n >else @ FOLD THEN EXIT THEN
   n >body @ FOLD n >body !  n >else @ FOLD n >else !  n ;M

\ c4's a && b is BZ over b: the value is b's when a is truthy, and a's
\ (which is zero) when it is not.
:M FOLD n_land {: n | a -- n :}
   n >lhs @ FOLD DUP n >lhs ! TO a
   a NUM? IF
      FOLDED  a >val @ 0= IF a ELSE n >rhs @ FOLD THEN EXIT THEN
   n >rhs @ FOLD n >rhs !  n ;M
\ a || b is BNZ over b: a's value when truthy, b's when not.
:M FOLD n_lor {: n | a -- n :}
   n >lhs @ FOLD DUP n >lhs ! TO a
   a NUM? IF
      FOLDED  a >val @ 0= IF n >rhs @ FOLD ELSE a THEN EXIT THEN
   n >rhs @ FOLD n >rhs !  n ;M

\ -- statements ------------------------------------------------------------

:M FOLD n_blk {: n | v -- n :}
   n >list @ TO v
   n >len @ 0 ?DO I CELLS v + @ FOLD  I CELLS v + ! LOOP
   n ;M
:M FOLD n_expst {: n -- n :}  n >expr @ FOLD n >expr !  n ;M
:M FOLD n_ret   {: n -- n :}
   n >expr @ ?DUP IF FOLD n >expr ! THEN  n ;M
:M FOLD n_linit {: n -- n :}
   n >expr @ ?DUP IF FOLD n >expr ! THEN  n ;M
:M FOLD n_switch {: n -- n :}
   n >cond @ FOLD n >cond !  n >body @ FOLD n >body !  n ;M

:M FOLD n_if {: n | c -- n :}
   n >cond @ FOLD DUP n >cond ! TO c
   c NUM? IF
      FOLDED
      c >val @ IF n >body @ FOLD EXIT THEN
      n >else @ ?DUP IF FOLD ELSE EMPTY-STMT THEN EXIT THEN
   n >body @ FOLD n >body !
   n >else @ ?DUP IF FOLD n >else ! THEN
   n ;M

\ while (0) disappears whole. Any break or continue inside it belongs to
\ it, so nothing outside can be left dangling.
:M FOLD n_while {: n | c -- n :}
   n >cond @ FOLD DUP n >cond ! TO c
   c NUM? c >val @ 0= AND IF FOLDED EMPTY-STMT EXIT THEN
   n >body @ FOLD n >body !  n ;M

\ do/while runs its body at least once, so only the parts fold.
:M FOLD n_do {: n -- n :}
   n >cond @ FOLD n >cond !  n >body @ FOLD n >body !  n ;M

\ for (i; 0; s) keeps the initialiser's side effects and nothing else.
:M FOLD n_for {: n | c i -- n :}
   n >init @ ?DUP IF FOLD DUP n >init ! THEN
   n >cond @ ?DUP IF FOLD DUP n >cond ! TO c ELSE 0 TO c THEN
   c IF c NUM? c >val @ 0= AND IF
      FOLDED
      n >init @ ?DUP IF n_expst N1 ELSE EMPTY-STMT THEN EXIT THEN THEN
   n >step @ ?DUP IF FOLD n >step ! THEN
   n >body @ FOLD n >body !
   n ;M

\ -- T2: dead function elimination ---------------------------------------
\ A function is live if it can be reached from a ROOT: main, a
\ constructor or destructor, a global initialised with its address, or
\ __c4cc_make_va, which every variadic call site reaches implicitly and
\ no call site names.
\
\ References are collected from the FOLDED tree, which is the whole
\ point of the ordering: `if (0) helper();` has already lost its call by
\ the time anything asks who calls helper.
\
\ Collection is by NAME rather than by symbol, exactly as c4lc does it.
\ A local variable that shadows a function keeps that function alive --
\ which is only ever too careful, and is cheaper than being clever about
\ scopes in a pass whose job is to delete things.

1024 CONSTANT FNMAX
CREATE FN-A    FNMAX CELLS ALLOT
CREATE FN-U    FNMAX CELLS ALLOT
CREATE FN-LIVE FNMAX CELLS ALLOT
\ ...and what a call site needs to know before the definition is read:
\ its return type, how many fixed parameters it takes, and whether it is
\ variadic. c4lc registers every defined function in a pre-pass for the
\ same reason -- a call may come first, with no prototype anywhere.
CREATE FN-T    FNMAX CELLS ALLOT
CREATE FN-N    FNMAX CELLS ALLOT
CREATE FN-V    FNMAX CELLS ALLOT
VARIABLE #FNS   0 #FNS !
VARIABLE CURFN  -1 CURFN !              \ which function is being walked

16384 CONSTANT REFMAX
CREATE REF-F REFMAX CELLS ALLOT
CREATE REF-A REFMAX CELLS ALLOT
CREATE REF-U REFMAX CELLS ALLOT
VARIABLE #REFS  0 #REFS !

\ Pass one records; pass two consults. Both parse the same tokens.
VARIABLE COLLECT   0 COLLECT !
VARIABLE NDROP     0 NDROP !

: T2-RESET  0 #FNS !  0 #REFS !  -1 CURFN !  0 NDROP ! ;
: FN-FIND ( a u -- i|-1 ) {: a u -- i :}
   #FNS @ 0 ?DO
      u I CELLS FN-U + @ = IF
         a  I CELLS FN-A + @  u BYTES= IF I UNLOOP EXIT THEN
      THEN
   LOOP -1 ;
: FN-DEF ( a u -- i ) {: a u | i -- i :}
   a u FN-FIND TO i
   i 0< 0= IF i EXIT THEN
   #FNS @ FNMAX < 0= IF ." c4fc: too many functions" CR ABORT THEN
   a #FNS @ CELLS FN-A + !   u #FNS @ CELLS FN-U + !
   0 #FNS @ CELLS FN-LIVE + !
   1 #FNS @ CELLS FN-T + !  0 #FNS @ CELLS FN-N + !  0 #FNS @ CELLS FN-V + !
   #FNS @   1 #FNS +! ;
: FN-SIG! ( i ct nfix va -- ) {: i ct n va -- :}
   ct i CELLS FN-T + !  n i CELLS FN-N + !  va i CELLS FN-V + ! ;
: REF, ( a u -- ) {: a u -- :}
   #REFS @ REFMAX < 0= IF ." c4fc: too many references" CR ABORT THEN
   CURFN @ #REFS @ CELLS REF-F + !
   a #REFS @ CELLS REF-A + !   u #REFS @ CELLS REF-U + !
   1 #REFS +! ;
: ROOT, ( a u -- )  FN-DEF CELLS FN-LIVE + 1 SWAP ! ;
: FN-LIVE? ( a u -- f ) {: a u | i -- f :}
   a u FN-FIND TO i
   i 0< IF -1 EXIT THEN                 \ never defined here: not ours to drop
   i CELLS FN-LIVE + @ ;

\ Reach a fixed point. The reference list is small and the graph is
\ shallow, so sweeping it until nothing changes costs less than the
\ worklist it would take to avoid the sweeps.
: T2-CLOSE {: | ch i f -- :}
   BEGIN
      0 TO ch
      #REFS @ 0 ?DO
         I CELLS REF-F + @ TO f
         f 0< f 0< 0= IF f CELLS FN-LIVE + @ ELSE 0 THEN OR IF
            I CELLS REF-A + @  I CELLS REF-U + @ FN-FIND TO i
            i 0< 0= IF
               i CELLS FN-LIVE + @ 0= IF 1 i CELLS FN-LIVE + !  1 TO ch THEN
            THEN
         THEN
      LOOP
   ch 0= UNTIL ;

\ -- the reference walk ---------------------------------------------------
\ One method per kind, as every phase here is. Unlike FOLD it descends
\ into &x and ++x too: `&handler` is how a function reaches a table, and
\ a pass that could not see it would delete the handler.

GENERIC: REFS ( node -- )

: SYM-REF ( y -- )  DUP y.name @ SWAP y.nlen @ REF, ;
: REFS? ( n -- )    ?DUP IF REFS THEN ;
: REFS-ARGS ( n -- ) {: n | v -- :}
   n >args @ TO v
   n >argn @ 0 ?DO I CELLS v + @ REFS LOOP ;

:M REFS n_num     DROP ;M
:M REFS n_str     DROP ;M
:M REFS n_case    DROP ;M
:M REFS n_default DROP ;M
:M REFS n_break   DROP ;M
:M REFS n_cont    DROP ;M
:M REFS n_empty   DROP ;M

:M REFS n_var   >sym @ SYM-REF ;M
:M REFS n_gvar  >sym @ SYM-REF ;M
:M REFS n_fnref >sym @ SYM-REF ;M

:M REFS n_not     >opnd @ REFS ;M
:M REFS n_bnot    >opnd @ REFS ;M
:M REFS n_neg     >opnd @ REFS ;M
:M REFS n_deref   >opnd @ REFS ;M
:M REFS n_addr    >opnd @ REFS ;M
:M REFS n_preinc  >opnd @ REFS ;M
:M REFS n_predec  >opnd @ REFS ;M
:M REFS n_postinc >opnd @ REFS ;M
:M REFS n_postdec >opnd @ REFS ;M

:M REFS n_asgn  {: n -- :}  n >lhs @ REFS  n >rhs @ REFS ;M
:M REFS n_bin   {: n -- :}  n >lhs @ REFS  n >rhs @ REFS ;M
:M REFS n_index {: n -- :}  n >lhs @ REFS  n >rhs @ REFS ;M
:M REFS n_lor   {: n -- :}  n >lhs @ REFS  n >rhs @ REFS ;M
:M REFS n_land  {: n -- :}  n >lhs @ REFS  n >rhs @ REFS ;M
:M REFS n_member  >lhs @ REFS ;M
:M REFS n_cast    >expr @ REFS ;M
:M REFS n_expst   >expr @ REFS ;M
:M REFS n_ret     >expr @ REFS? ;M
:M REFS n_linit   >expr @ REFS? ;M
:M REFS n_comma   REFS-ARGS ;M
:M REFS n_call  {: n -- :}  n >fn @ SYM-REF  n REFS-ARGS ;M

:M REFS n_cond   {: n -- :}
   n >cond @ REFS  n >body @ REFS  n >else @ REFS ;M
:M REFS n_if     {: n -- :}
   n >cond @ REFS  n >body @ REFS  n >else @ REFS? ;M
:M REFS n_while  {: n -- :}  n >cond @ REFS  n >body @ REFS ;M
:M REFS n_do     {: n -- :}  n >cond @ REFS  n >body @ REFS ;M
:M REFS n_switch {: n -- :}  n >cond @ REFS  n >body @ REFS ;M
:M REFS n_for    {: n -- :}
   n >init @ REFS?  n >cond @ REFS?  n >step @ REFS?  n >body @ REFS ;M
:M REFS n_blk    {: n | v -- :}
   n >list @ TO v
   n >len @ 0 ?DO I CELLS v + @ REFS LOOP ;M

\ -- what the unit DEFINES, and what it only declares ---------------------
\ Object mode has to know, at a prototype, whether the function is
\ defined further down the file -- which is exactly what the pass the
\ tree passes already run is for, so `-c` runs it too.
\
\ The extern ids are handed out between the passes and in c4lc's order:
\ every prototype first, in the order they appear, then every extern
\ datum. Not interleaved, because c4lc numbers them in two walks.

1024 CONSTANT DGMAX
CREATE DG-A DGMAX CELLS ALLOT
CREATE DG-U DGMAX CELLS ALLOT
VARIABLE #DG   0 #DG !
: DG-DEF ( a u -- ) {: a u -- :}
   #DG @ DGMAX < 0= IF ." c4fc: too many definitions" CR ABORT THEN
   a #DG @ CELLS DG-A + !  u #DG @ CELLS DG-U + !  1 #DG +! ;
: DG-DEFINED? ( a u -- f ) {: a u -- f :}
   #DG @ 0 ?DO
      u I CELLS DG-U + @ = IF
         a I CELLS DG-A + @ u BYTES= IF -1 UNLOOP EXIT THEN
      THEN
   LOOP 0 ;

512 CONSTANT PLMAX
CREATE PL-A PLMAX CELLS ALLOT   CREATE PL-U PLMAX CELLS ALLOT
CREATE PL-T PLMAX CELLS ALLOT   CREATE PL-V PLMAX CELLS ALLOT
VARIABLE #PL   0 #PL !
: PROTO, ( a u ct va -- ) {: a u ct va -- :}
   #PL @ PLMAX < 0= IF ." c4fc: too many prototypes" CR ABORT THEN
   a #PL @ CELLS PL-A + !  u #PL @ CELLS PL-U + !
   ct #PL @ CELLS PL-T + !  va #PL @ CELLS PL-V + !  1 #PL +! ;

CREATE XG-A PLMAX CELLS ALLOT   CREATE XG-U PLMAX CELLS ALLOT
CREATE XG-T PLMAX CELLS ALLOT   CREATE XG-G PLMAX CELLS ALLOT
VARIABLE #XG   0 #XG !
: EXTG, ( a u ct agg -- ) {: a u ct ag -- :}
   #XG @ PLMAX < 0= IF ." c4fc: too many extern data" CR ABORT THEN
   a #XG @ CELLS XG-A + !  u #XG @ CELLS XG-U + !
   ct #XG @ CELLS XG-T + !  ag #XG @ CELLS XG-G + !  1 #XG +! ;

: DECL-RESET  0 #DG !  0 #PL !  0 #XG ! ;

: MAKE-EXTERNS ( -- ) {: | a u -- :}
   #PL @ 0 ?DO
      I CELLS PL-A + @ TO a   I CELLS PL-U + @ TO u
      a u DG-DEFINED? 0=  a u EXT-FIND 0< AND IF
         a u  I CELLS PL-T + @  129  I CELLS PL-V + @  0  EXT-NEW DROP
      THEN
   LOOP
   #XG @ 0 ?DO
      I CELLS XG-A + @ TO a   I CELLS XG-U + @ TO u
      a u DG-DEFINED? 0=  a u EXT-FIND 0< AND IF
         a u  I CELLS XG-T + @  131  0  I CELLS XG-G + @  EXT-NEW DROP
      THEN
   LOOP ;
