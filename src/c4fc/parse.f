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

\ || and && are NOT here: they are short-circuit branches, so they are
\ node kinds with methods rather than rows with an opcode.
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
   TK Sizeof = IF                       \ sizeof(type) folds to a constant
      TNEXT Lparen WANT
      PARSE-TYPE t_char = IF 1 ELSE 1 CELLS THEN
      Rparen WANT n_num N1 EXIT
   THEN
   TK Lparen = IF
      TNEXT
      TYPE? IF SKIP-TYPE Rparen WANT 13 EXPR EXIT THEN   \ a cast emits nothing
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

: POSTFIX ( -- node )
   PRIMARY
   BEGIN
      TK Inc = IF TNEXT n_postinc N1 ELSE
      TK Dec = IF TNEXT n_postdec N1 ELSE EXIT THEN THEN
   AGAIN ;

\ Unary operators bind tighter than any infix one, so each parses its
\ operand at the Inc level. Negating a literal folds, as c4 does it --
\ which is why -1 is one IMM and not a multiply.
: UNARY ( -- node )
   TK Not   = IF TNEXT 13 EXPR n_not   N1 EXIT THEN
   TK Tilde = IF TNEXT 13 EXPR n_bnot  N1 EXIT THEN
   TK Sub   = IF TNEXT
                 TK Num = IF TV NEGATE TNEXT n_num N1 EXIT THEN
                 13 EXPR n_neg N1 EXIT THEN
   TK Add   = IF TNEXT 13 EXPR EXIT THEN            \ unary plus is nothing
   TK Mul   = IF TNEXT 13 EXPR n_deref N1 EXIT THEN
   TK And   = IF TNEXT 13 EXPR n_addr  N1 EXIT THEN
   TK Inc   = IF TNEXT 13 EXPR n_preinc N1 EXIT THEN
   TK Dec   = IF TNEXT 13 EXPR n_predec N1 EXIT THEN
   POSTFIX ;

: (EXPR) ( lev -- node ) {: lev | n i p r q nd -- n :}
   UNARY TO n
   BEGIN
      TK Assign = lev 1 <= AND IF
         TNEXT  n 1 EXPR n_asgn N2 TO n
      ELSE TK Cond = lev 2 <= AND IF
         TNEXT  1 EXPR TO r  Colon WANT  2 EXPR TO q
         n_cond 4 CELLS NEW TO nd
         n nd >cond !  r nd >body !  q nd >else !  nd TO n
      ELSE TK Lor = lev 3 <= AND IF
         TNEXT  n 4 EXPR n_lor N2 TO n
      ELSE TK Lan = lev 4 <= AND IF
         TNEXT  n 5 EXPR n_land N2 TO n
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
      THEN THEN THEN THEN
   AGAIN ;
' (EXPR) IS EXPR

\ -- statements ---------------------------------------------------------

DEFER STATEMENT                         \ blocks and statements nest

: COMPOUND ( -- node ) {: | v n -- :}   \ { ... } with no declarations in it
   Lbrace WANT
   256 CELLS ALLOT: TO v   0 TO n
   BEGIN TK Rbrace <> WHILE  STATEMENT v n CELLS + !  n 1+ TO n  REPEAT
   Rbrace WANT
   v n n_blk N2 ;

256 CONSTANT CVMAX
CREATE CVAL CVMAX CELLS ALLOT   VARIABLE CVN   0 CVN !
: CASE-VALUE ( -- v )
   TK Sub = IF TNEXT TV NEGATE Num WANT EXIT THEN
   TV Num WANT ;

: (STATEMENT) ( -- node ) {: | c b e i s nd lo hi -- n :}
   TK Semi   = IF TNEXT n_empty 1 CELLS NEW EXIT THEN
   TK Lbrace = IF COMPOUND EXIT THEN
   TK If = IF
      TNEXT Lparen WANT 1 EXPR TO c Rparen WANT
      STATEMENT TO b   0 TO e
      TK Else = IF TNEXT STATEMENT TO e THEN
      n_if 4 CELLS NEW TO nd
      c nd >cond !  b nd >body !  e nd >else !  nd EXIT
   THEN
   TK While = IF
      TNEXT Lparen WANT 1 EXPR TO c Rparen WANT  STATEMENT TO b
      n_while 3 CELLS NEW TO nd
      c nd >cond !  b nd >body !  nd EXIT
   THEN
   TK Do = IF
      TNEXT STATEMENT TO b
      While WANT Lparen WANT 1 EXPR TO c Rparen WANT Semi WANT
      n_do 3 CELLS NEW TO nd
      c nd >cond !  b nd >body !  nd EXIT
   THEN
   TK For = IF
      TNEXT Lparen WANT
      0 TO i   TK Semi   <> IF 1 EXPR TO i THEN  Semi WANT
      0 TO c   TK Semi   <> IF 1 EXPR TO c THEN  Semi WANT
      0 TO s   TK Rparen <> IF 1 EXPR TO s THEN  Rparen WANT
      STATEMENT TO b
      n_for 5 CELLS NEW TO nd
      c nd >cond !  b nd >body !  i nd >init !  s nd >step !  nd EXIT
   THEN
   TK Switch = IF
      TNEXT Lparen WANT 1 EXPR TO c Rparen WANT
      CVN @ TO i
      STATEMENT TO b
      CVN @ i = IF ." c4fc: a switch with no cases" CR ABORT THEN
      CVAL i CELLS + @ TO lo   lo TO hi
      CVN @ i ?DO
         CVAL I CELLS + @ TO s
         s lo < IF s TO lo THEN
         s hi > IF s TO hi THEN
      LOOP
      n_switch 6 CELLS NEW TO nd
      c nd >cond !  b nd >body !  lo nd >lo !  hi nd >hi !
      \ The table is allocated HERE, after the body -- which is where
      \ c4lc puts it: a string literal inside the switch gets the lower
      \ address.
      D-ALIGN  hi lo - 1+ CELLS D-ALLOT nd >tab !
      i CVN !
      nd EXIT
   THEN
   TK Case = IF
      TNEXT CASE-VALUE TO c Colon WANT
      c CVAL CVN @ CELLS + !  1 CVN +!
      c n_case N1 EXIT
   THEN
   TK Default = IF TNEXT Colon WANT n_default 1 CELLS NEW EXIT THEN
   TK Break    = IF TNEXT Semi WANT n_break 1 CELLS NEW EXIT THEN
   TK Continue = IF TNEXT Semi WANT n_cont  1 CELLS NEW EXIT THEN
   TK Return   = IF TNEXT 1 EXPR Semi WANT n_ret N1 EXIT THEN
   1 EXPR Semi WANT n_expst N1 ;
' (STATEMENT) IS STATEMENT

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
   PLACE-GLOBALS
   EMIT-TABPATS ;
