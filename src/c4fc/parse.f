\ c4fc parse.f -- tokens to AST.
\
\ Recursive descent, and a symbol table that is three scopes deep at
\ most: builtins, globals, and the function in hand. C wants a table
\ rather than a grammar for exactly one reason -- a name means different
\ things depending on what has been declared -- which is also why the
\ design does not reach for a grammar DSL.
\
\ Forth-2012 allows one {: :} per definition, so every local a word uses
\ is declared at its top. That is a real constraint on how these words
\ are shaped and it is worth stating rather than working around.

0 CONSTANT c_glo   1 CONSTANT c_fun   2 CONSTANT c_builtin   3 CONSTANT c_loc

512 CONSTANT NSYM
VARIABLE STAB   VARIABLE STN
VARIABLE NLOC                           \ locals in the function in hand
VARIABLE NGLO                           \ globals, numbered; placed at the end
VARIABLE TP

: ST[] ( i -- a )  SYMR * STAB @ + ;
: ST, ( a u class val ct -- ) {: a u c v ct | y -- :}
   STN @ NSYM < 0= IF ." c4fc: symbol table full" CR ABORT THEN
   STN @ ST[] TO y
   a y y.name !  u y y.nlen !  1 y y.type !  c y y.class !  v y y.val !
   ct y y.ct !
   1 STN +! ;
: ST-FIND ( a u -- sym|0 ) {: a u | y -- s :}    \ newest first: locals win
   STN @ 0 ?DO
      STN @ 1- I - ST[] TO y
      y y.nlen @ u = IF
         y y.name @ a u BYTES= IF y UNLOOP EXIT THEN
      THEN
   LOOP 0 ;

\ -- the token cursor ---------------------------------------------------

: TOK@ ( -- t )    TP @ TOKS V@ ;
: TK   ( -- kind ) TOK@ t.kind @ ;
: TV   ( -- v )    TOK@ t.val @ ;
: TL   ( -- n )    TOK@ t.len @ ;
: TNEXT            1 TP +! ;
: WANT ( kind -- )
   TK <> IF ." c4fc: line " TOK@ t.line @ .N ." : unexpected token" CR ABORT THEN
   TNEXT ;

\ -- the infix table ----------------------------------------------------
\   <token kind> <precedence> <C4 opcode> INFIX      one row per operator

64 CONSTANT IMAX
VARIABLE #IN   0 #IN !
CREATE INK IMAX CELLS ALLOT
CREATE INP IMAX CELLS ALLOT
CREATE INO IMAX CELLS ALLOT
: INFIX ( kind prec opcode -- )
   #IN @ CELLS INO + !  #IN @ CELLS INP + !  #IN @ CELLS INK + !  1 #IN +! ;
: IN-FIND ( kind -- i|-1 ) {: k -- i :}
   #IN @ 0 ?DO k I CELLS INK + @ = IF I UNLOOP EXIT THEN LOOP -1 ;

Lor  3 oOR  INFIX   Lan 4 oAND INFIX
Or   5 oOR  INFIX   Xor 6 oXOR INFIX   And 7 oAND INFIX
Eq   8 oEQ  INFIX   Ne  8 oNE  INFIX
Lt   9 oLT  INFIX   Gt  9 oGT  INFIX   Le  9 oLE  INFIX   Ge 9 oGE INFIX
Shl 10 oSHL INFIX   Shr 10 oSHR INFIX
Add 11 oADD INFIX   Sub 11 oSUB INFIX
Mul 12 oMUL INFIX   Div 12 oDIV INFIX  Mod 12 oMOD INFIX

\ -- nodes --------------------------------------------------------------

: N1 ( v tag -- n )   2 CELLS NEW TUCK 1 CELLS + ! ;
: N2 ( a b tag -- n ) 3 CELLS NEW {: a b n -- n :}
   a n 1 CELLS + !  b n 2 CELLS + !  n ;

\ -- expressions --------------------------------------------------------

DEFER EXPR                              \ ( lev -- node )

: TYPE? TK Int = TK Char = OR ;
: PARSE-TYPE ( -- t ) {: | t -- t :}
   TK Char = IF t_char TO t ELSE
   TK Int  = IF t_int  TO t ELSE
      ." c4fc: line " TOK@ t.line @ .N ." : a type was expected" CR ABORT
   THEN THEN
   TNEXT
   BEGIN TK Mul = WHILE t 2 + TO t TNEXT REPEAT
   t ;
: SKIP-TYPE PARSE-TYPE DROP ;

: ARGS ( -- args n ) {: | v n -- :}
   8 CELLS ALLOT: TO v   0 TO n
   TK Rparen <> IF
      BEGIN 1 EXPR v n CELLS + !  n 1+ TO n  TK Comma = WHILE TNEXT REPEAT
   THEN
   Rparen WANT
   v n ;

: PRIMARY ( -- node ) {: | s a u v k nd -- n :}
   TK Num = IF TV TNEXT n_num N1 EXIT THEN
   TK Str = IF TV TL D-STR, TNEXT n_str N1 EXIT THEN
   TK Lparen = IF
      TNEXT
      TYPE? IF SKIP-TYPE Rparen WANT 12 EXPR EXIT THEN    \ a cast emits nothing
      1 EXPR Rparen WANT EXIT
   THEN
   TK Id = IF
      TV TO a  TL TO u  TNEXT
      a u ST-FIND TO s
      s 0= IF ." c4fc: undeclared " a u TYPE CR ABORT THEN
      TK Lparen = IF
         TNEXT ARGS TO k TO v
         n_call 4 CELLS NEW TO nd
         s nd >fn !  v nd >args !  k nd >argn !
         nd EXIT
      THEN
      s  s y.class @ c_glo = IF n_gvar ELSE n_var THEN  N1 EXIT
   THEN
   ." c4fc: line " TOK@ t.line @ .N ." : an expression was expected" CR ABORT ;

: (EXPR) ( lev -- node ) {: lev | n i p r nd -- n :}
   PRIMARY TO n
   BEGIN
      TK Assign = lev 1 <= AND IF
         TNEXT  n 1 EXPR n_asgn N2 TO n
      ELSE
         TK IN-FIND TO i
         i 0< IF n EXIT THEN
         i CELLS INP + @ TO p
         p lev < IF n EXIT THEN
         TNEXT
         p 1+ EXPR TO r
         n_bin 4 CELLS NEW TO nd
         n nd >lhs !  r nd >rhs !  i CELLS INO + @ nd >op !
         nd TO n
      THEN
   AGAIN ;
' (EXPR) IS EXPR

\ -- statements ---------------------------------------------------------

: STATEMENT ( -- node )
   TK Return = IF TNEXT 1 EXPR Semi WANT n_ret N1 EXIT THEN
   1 EXPR Semi WANT n_expst N1 ;

: LOCAL-DECLS {: | ct -- :}
   BEGIN TYPE? WHILE
      PARSE-TYPE TO ct
      BEGIN
         1 NLOC +!
         TV TL c_loc NLOC @ NEGATE ct ST,
         Id WANT
         TK Comma = WHILE TNEXT
      REPEAT
      Semi WANT
   REPEAT ;

: BLOCK ( -- node ) {: | v n -- :}
   Lbrace WANT
   LOCAL-DECLS
   256 CELLS ALLOT: TO v   0 TO n       \ one block's worth; blocks nest by
   BEGIN TK Rbrace <> WHILE             \ each taking its own
      STATEMENT v n CELLS + !  n 1+ TO n
   REPEAT
   Rbrace WANT
   v n n_blk N2 ;

\ -- declarations -------------------------------------------------------

: PARAMS ( -- n ) {: | n base ct -- n :}
   Lparen WANT
   STN @ TO base   0 TO n
   TK Rparen <> IF
      BEGIN
         PARSE-TYPE TO ct
         TV TL c_loc 0 ct ST,   n 1+ TO n
         Id WANT
         TK Comma = WHILE TNEXT
      REPEAT
   THEN
   Rparen WANT
   \ c4 puts the LAST argument at bp+2, so the kth of n sits at bp+(n+2-k)
   n 0 ?DO  n 2 + I 1+ -  base I + ST[] y.val !  LOOP
   n ;

: NAME=? ( a1 u1 a2 u2 -- f ) {: a u b v -- f :}
   u v <> IF 0 EXIT THEN  a b u BYTES= ;

: FUNCTION ( a u ct -- ) {: a u ct | base body -- :}
   a u c_fun CHERE ct ST,
   a u ct 129 CHERE SYM,                \ the record's "type" IS the C type
   a u S" main" NAME=? IF CHERE ENTRY ! THEN
   STN @ TO base   0 NLOC !
   PARAMS DROP
   BLOCK TO body                        \ parse first: ENT needs the count
   NLOC @ oENT OP2,
   body STMT
   \ return emits its own LEV, so a function ending in one gets one LEV
   LAST-OP oLEV <> IF oLEV OP, THEN
   base STN ! ;                         \ parameters and locals go out of scope

: DECL {: | a u ct -- :}
   TYPE? 0= IF ." c4fc: a declaration was expected" CR ABORT THEN
   PARSE-TYPE TO ct
   TV TO a  TL TO u  Id WANT
   TK Lparen = IF a u ct FUNCTION EXIT THEN
   a u c_glo NGLO @ ct ST,  1 NGLO +!
   BEGIN TK Comma = WHILE
      TNEXT  TV TO a  TL TO u  Id WANT
      a u c_glo NGLO @ ct ST,  1 NGLO +!
   REPEAT
   Semi WANT ;

\ Globals go AFTER every string literal, so their addresses are the last
\ thing known and the patches that name them are revisited here. Their
\ symbols come after every function's for the same reason -- which is
\ why c4lc's table lists g last even though g is declared first.
: PLACE-GLOBALS {: | base y -- :}
   D-ALIGN  DN @ TO base
   base FIX-GLOBALS
   STN @ 0 ?DO
      I ST[] TO y
      y y.class @ c_glo = IF
         base y y.val @ CELLS +  y y.val !
         y y.name @ y y.nlen @ y y.ct @ 131 y y.val @ SYM,
      THEN
   LOOP
   NGLO @ CELLS DN +! ;

: PROGRAM
   BEGIN TK Eof <> WHILE DECL REPEAT
   PLACE-GLOBALS ;
