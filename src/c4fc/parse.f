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
4 CONSTANT c_const

512 CONSTANT NSYM
VARIABLE STAB   VARIABLE STN
VARIABLE NLOC                           \ locals in the function in hand
VARIABLE NGLO                           \ globals, numbered; placed at the end
VARIABLE TP

: ST[] ( i -- a )  SYMR * STAB @ + ;
: ST, ( a u class val ct agg sz -- ) {: a u c v ct ag sz | y -- :}
   STN @ NSYM < 0= IF ." c4fc: symbol table full" CR ABORT THEN
   STN @ ST[] TO y
   a y y.name !  u y y.nlen !  1 y y.type !  c y y.class !  v y y.val !
   ct y y.ct !  ag y y.agg !  sz y y.sz !
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

DEFER PARSE-TYPE                        \ members are declarations too, and
                                        \ sizeof appears inside constants

: NAME=? ( a1 u1 a2 u2 -- f ) {: a u b v -- f :}
   u v <> IF 0 EXIT THEN  a b u BYTES= ;

\ -- constant expressions ------------------------------------------------
\ enum bodies and initialisers need values, not code, and they need the
\ same precedence the code path uses -- so this walks the same table and
\ applies the opcodes numerically instead of emitting them. C's
\ comparisons yield 0 or 1 where Forth's yield 0 or -1, which is one AND.

DEFER CEXPR
: CAPPLY ( a b op -- v )
   DUP oADD = IF DROP +      EXIT THEN
   DUP oSUB = IF DROP -      EXIT THEN
   DUP oMUL = IF DROP *      EXIT THEN
   DUP oDIV = IF DROP /      EXIT THEN
   DUP oMOD = IF DROP MOD    EXIT THEN
   DUP oAND = IF DROP AND    EXIT THEN
   DUP oOR  = IF DROP OR     EXIT THEN
   DUP oXOR = IF DROP XOR    EXIT THEN
   DUP oSHL = IF DROP LSHIFT EXIT THEN
   DUP oSHR = IF DROP RSHIFT EXIT THEN
   DUP oEQ  = IF DROP =  1 AND EXIT THEN
   DUP oNE  = IF DROP <> 1 AND EXIT THEN
   DUP oLT  = IF DROP <  1 AND EXIT THEN
   DUP oGT  = IF DROP >  1 AND EXIT THEN
   DUP oLE  = IF DROP <= 1 AND EXIT THEN
   DUP oGE  = IF DROP >= 1 AND EXIT THEN
   DROP ." c4fc: that operator is not allowed in a constant" CR ABORT ;

: CPRIMARY ( -- v ) {: | s -- v :}
   TK Num    = IF TV TNEXT EXIT THEN
   TK Sub    = IF TNEXT 13 CEXPR NEGATE EXIT THEN
   TK Add    = IF TNEXT 13 CEXPR EXIT THEN
   TK Not    = IF TNEXT 13 CEXPR 0= 1 AND EXIT THEN
   TK Tilde  = IF TNEXT 13 CEXPR INVERT EXIT THEN
   TK Lparen = IF TNEXT 1 CEXPR Rparen WANT EXIT THEN
   TK Sizeof = IF TNEXT Lparen WANT PARSE-TYPE T-SIZE Rparen WANT EXIT THEN
   TK Id = IF
      TV TL ST-FIND TO s
      s 0= IF ." c4fc: unknown name in a constant" CR ABORT THEN
      s y.class @ c_const <> IF ." c4fc: not a constant" CR ABORT THEN
      TNEXT s y.val @ EXIT
   THEN
   ." c4fc: line " TOK@ t.line @ .N ." : a constant was expected" CR ABORT ;

: (CEXPR) ( lev -- v ) {: lev | v i p -- v :}
   CPRIMARY TO v
   BEGIN
      TK IN-FIND TO i
      i 0< IF v EXIT THEN
      i CELLS INP + @ TO p
      p lev < IF v EXIT THEN
      TNEXT
      v  p 1+ CEXPR  i CELLS INO + @ CAPPLY TO v
   AGAIN ;
' (CEXPR) IS CEXPR
: CONST-EXPR ( -- v )  1 CEXPR ;

\ -- nodes --------------------------------------------------------------

: N1 ( v tag -- n )   2 CELLS NEW TUCK 1 CELLS + ! ;
: N2 ( a b tag -- n ) 3 CELLS NEW {: a b n -- n :}
   a n 1 CELLS + !  b n 2 CELLS + !  n ;

\ -- expressions --------------------------------------------------------

DEFER EXPR                              \ ( lev -- node )

: TYPE? TK Int = TK Char = OR TK Struct = OR TK Union = OR ;

\ Members are laid out one cell at a time: c4lc gives a char member a
\ whole cell, which is visible the moment a struct starts with two of
\ them -- { char a; char b; int c; } is twenty-four bytes and b is at
\ eight, not one.
: PARSE-MEMBERS ( k -- ) {: k | t a u off n ag mt -- :}
   Lbrace WANT
   0 TO off
   BEGIN TK Rbrace <> WHILE
      PARSE-TYPE TO t
      BEGIN
         TV TO a  TL TO u  Id WANT
         1 TO n   0 TO ag   t TO mt
         TK Brak = IF
            TNEXT CONST-EXPR TO n Rbrak WANT
            1 TO ag   t 2 + TO mt
         THEN
         off 1 CELLS 1- + 1 CELLS 1- INVERT AND TO off
         k a u mt off ag MEM,
         off n t T-SIZE * + TO off
         TK Comma = WHILE TNEXT
      REPEAT
      Semi WANT
   REPEAT
   Rbrace WANT
   off 1 CELLS 1- + 1 CELLS 1- INVERT AND  k CELLS ST-SIZE + ! ;

: (PARSE-TYPE) ( -- t ) {: | t k a u -- t :}
   TK Struct = TK Union = OR IF
      TNEXT
      0 TO a  0 TO u
      TK Id = IF TV TO a  TL TO u  TNEXT THEN
      a u ST-FINDS TO k
      k 0< IF a u ST-NEW TO k THEN
      TK Lbrace = IF k PARSE-MEMBERS THEN
      k ST-TYPE TO t
   ELSE
      TK Char = IF t_char TO t ELSE
      TK Int  = IF t_int  TO t ELSE
         ." c4fc: line " TOK@ t.line @ .N ." : a type was expected" CR ABORT
      THEN THEN
      TNEXT
   THEN
   BEGIN TK Mul = WHILE t 2 + TO t TNEXT REPEAT
   t ;
' (PARSE-TYPE) IS PARSE-TYPE
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
      TNEXT Lparen WANT  PARSE-TYPE T-SIZE  Rparen WANT  n_num N1 EXIT
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
      s y.class @ c_const = IF s y.val @ n_num N1 EXIT THEN
      s  s y.class @ c_glo = IF n_gvar ELSE n_var THEN  N1 EXIT
   THEN
   ." c4fc: line " TOK@ t.line @ .N ." : an expression was expected" CR ABORT ;

: MEMBER ( base -- node ) {: b | t k i a u nd -- n :}
   \ n_member is four cells: base, offset, type, aggregate
   b CT T-DEREF TO t
   t T-STRUCT? 0= IF ." c4fc: not a structure" CR ABORT THEN
   t T-INDEX TO k
   TV TO a  TL TO u  Id WANT
   k a u MEM-FIND TO i
   i 0< IF ." c4fc: no such member: " a u TYPE CR ABORT THEN
   n_member 5 CELLS NEW TO nd
   b nd >lhs !
   i CELLS MB-OFF  + @ nd >moff !
   i CELLS MB-TYPE + @ nd >mtype !
   i CELLS MB-AGG  + @ nd >magg !
   nd ;

: POSTFIX ( -- node ) {: | n -- n :}
   PRIMARY TO n
   BEGIN
      TK Brak = IF
         TNEXT  n  1 EXPR  n_index N2 TO n  Rbrak WANT
      ELSE TK Dot = IF
         TNEXT  n n_addr N1 MEMBER TO n     \ x.m is (&x)->m
      ELSE TK Arrow = IF
         TNEXT  n MEMBER TO n
      ELSE TK Inc = IF TNEXT n n_postinc N1 TO n
      ELSE TK Dec = IF TNEXT n n_postdec N1 TO n
      ELSE n EXIT
      THEN THEN THEN THEN THEN
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
: CASE-VALUE ( -- v )  CONST-EXPR ;

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

\ An array or a struct takes as many frame slots as it takes cells, and
\ its name stands for the address of the LOWEST one -- locals grow
\ downwards, so int a[4] as the first local is slots 1..4 and a is
\ LEA -4.
: LOCAL-DECLS {: | ct t a u n sz -- :}
   BEGIN TYPE? WHILE
      PARSE-TYPE TO ct
      BEGIN
         TV TO a  TL TO u  Id WANT
         ct TO t   0 TO sz
         TK Brak = IF
            TNEXT TV TO n Num WANT Rbrak WANT
            n ct T-SIZE * TO sz   ct 2 + TO t
         ELSE
            ct T-STRUCT? IF ct T-SIZE TO sz THEN
         THEN
         sz 0= IF
            1 NLOC +!
            a u c_loc NLOC @ NEGATE ct 0 0 ST,
         ELSE
            sz 1 CELLS 1- + 1 CELLS / NLOC +!
            a u c_loc NLOC @ NEGATE t 1 sz ST,
         THEN
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

\ ... is not a special form: it is one more parameter, unnamed, and the
\ CALL SITE is where the work happens. That is why int vsum(int n, ...)
\ finds n at bp+3 and not bp+2.
: PARAMS ( -- n va ) {: | n base ct va -- n va :}
   Lparen WANT
   STN @ TO base   0 TO n   0 TO va
   TK Rparen <> IF
      BEGIN
         TK Dot = IF
            TNEXT Dot WANT Dot WANT
            1 TO va   n 1+ TO n
            0
         ELSE
            PARSE-TYPE TO ct
            TV TL c_loc 0 ct 0 0 ST,   n 1+ TO n
            Id WANT
            TK Comma =
         THEN
      WHILE TNEXT REPEAT
   THEN
   Rparen WANT
   \ c4 puts the LAST argument at bp+2, so the kth of n sits at bp+(n+2-k)
   n va - 0 ?DO  n 2 + I 1+ -  base I + ST[] y.val !  LOOP
   n va ;

: FUNCTION ( a u ct sc at -- ) {: a u ct sc at | base body y n va -- :}
   a u ST-FIND TO y
   y 0= IF
      a u c_fun -1 ct 0 0 ST,
      STN @ 1- ST[] TO y
   THEN
   STN @ TO base   0 NLOC !
   PARAMS TO va TO n
   va y y.va !   n va - y y.nfix !
   TK Semi = IF TNEXT base STN ! EXIT THEN    \ a prototype and nothing more
   CHERE y y.val !
   a u S" main" NAME=? IF CHERE ENTRY ! THEN
   \ 0x20 marks a variadic function, which is one byte and the last
   \ difference to fall out of the whole of F7.
   a u ct 129 CHERE  sc at OR  va IF 32 OR THEN  SYM,
   at 1 AND IF CHERE CONSN @ CELLS CONS @ + !  1 CONSN +! THEN
   at 2 AND IF CHERE DESN  @ CELLS DESS @ + !  1 DESN  +! THEN
   a u S" __c4cc_make_va" NAME=? IF y VA-MAKE ! THEN
   BLOCK TO body                        \ parse first: ENT needs the count
   NLOC @ oENT OP2,
   body STMT
   \ return emits its own LEV, so a function ending in one gets one LEV
   LAST-OP oLEV <> IF oLEV OP, THEN
   base STN ! ;                         \ parameters and locals go out of scope

\ enum { A, B = 5, C };  -- constants, folded to literals where used.
: ENUM-DECL {: | a u v -- :}
   Enum WANT
   TK Id = IF TNEXT THEN                \ an optional tag, which c4 ignores
   Lbrace WANT
   0 TO v
   BEGIN TK Rbrace <> WHILE
      TV TO a  TL TO u  Id WANT
      TK Assign = IF TNEXT CONST-EXPR TO v THEN
      a u c_const v t_int 0 0 ST,
      v 1+ TO v
      TK Comma = IF TNEXT THEN
   REPEAT
   Rbrace WANT  Semi WANT ;

\ static is attrs 8, and extern declares without defining. __attribute__
\ ((constructor)) and ((destructor)) are 1 and 2, and also put the
\ function's code index in the image's constructor or destructor list --
\ which is what makes them run.
: STORAGE ( -- sc ) {: | sc -- sc :}
   0 TO sc
   BEGIN
      TK Static = IF 8 TO sc TNEXT 1 ELSE
      TK Extern = IF -1 TO sc TNEXT 1 ELSE 0 THEN THEN
   WHILE REPEAT
   sc ;
: ATTRS ( -- at ) {: | at -- at :}
   0 TO at
   BEGIN TK Attribute = WHILE
      TNEXT Lparen WANT Lparen WANT
      BEGIN TK Rparen <> WHILE
         TK Constructor = IF at 1 OR TO at THEN
         TK Destructor  = IF at 2 OR TO at THEN
         TNEXT
      REPEAT
      Rparen WANT Rparen WANT
   REPEAT
   at ;

\ An initialiser puts its bytes in region 1 and fixes the address there
\ and then, because region 1 begins at zero.
256 CONSTANT IVMAX
CREATE IVAL IVMAX CELLS ALLOT
: INIT-WRITE ( off elemtype n -- ) {: off et n -- :}
   n 0 ?DO
      IVAL I CELLS + @
      et t_char = IF off I + ID-C! ELSE off I CELLS + ID-! THEN
   LOOP ;

: DECL {: | a u ct t n sz ag sc at y ivn off es -- :}
   TK Enum = IF ENUM-DECL EXIT THEN
   STORAGE TO sc
   TYPE? 0= IF ." c4fc: a declaration was expected" CR ABORT THEN
   PARSE-TYPE TO ct
   ATTRS TO at
   TK Semi = IF TNEXT EXIT THEN         \ a struct definition and nothing more
   BEGIN
      TV TO a  TL TO u  Id WANT
      TK Lparen = IF a u ct sc 0 MAX at FUNCTION EXIT THEN
      ct TO t   0 TO ag   1 CELLS TO sz   -1 TO n   0 TO ivn   ct TO es
      TK Brak = IF
         TNEXT
         TK Rbrak = IF 0 TO n ELSE CONST-EXPR TO n THEN
         Rbrak WANT
         ct 2 + TO t   1 TO ag
      ELSE
         ct T-STRUCT? IF ct T-SIZE TO sz  1 TO ag THEN
      THEN
      \ the initialiser, if there is one
      TK Assign = IF
         TNEXT
         TK Str = IF
            TV TL 1+ TO ivn                       \ the nul counts
            ivn 0 ?DO TV I + C@ IVAL I CELLS + ! LOOP
            0 IVAL ivn 1- CELLS + !
            TNEXT
         ELSE TK Lbrace = IF
            TNEXT
            BEGIN TK Rbrace <> WHILE
               CONST-EXPR IVAL ivn CELLS + !  ivn 1+ TO ivn
               TK Comma = IF TNEXT THEN
            REPEAT
            Rbrace WANT
         ELSE
            CONST-EXPR IVAL !  1 TO ivn
         THEN THEN
         ag IF
            n 1 < IF ivn TO n THEN
            n es T-SIZE * TO sz
         THEN
         sz ID-ALLOT TO off
         off es ivn INIT-WRITE
         a u c_glo off t ag 0 ST,                  \ region 1: final already
         STN @ 1- ST[] TO y   1 y y.ini !  sc 0 MAX y y.sc !
      ELSE
         ag IF n 1 < IF 1 TO n THEN n es T-SIZE * TO sz THEN
         \ extern int x; int x;  is one global, not two -- a repeated
         \ declaration of a name already known is the same object.
         a u ST-FIND TO y
         y IF y y.class @ c_glo <> IF 0 TO y THEN THEN
         y 0= IF
            a u c_glo NGLO @ t ag sz ST,
            STN @ 1- ST[] TO y   sc 0 MAX y y.sc !
            1 NGLO +!
         THEN
      THEN
      TK Comma = WHILE TNEXT
   REPEAT
   Semi WANT ;

\ Where the three data regions actually land. Region 1 was placed as it
\ was declared; regions 2 and 3 are relative until now, so every patch
\ that names one gets shifted here.
1024 CONSTANT GMAX
CREATE GOFF GMAX CELLS ALLOT
: PLACE-GLOBALS {: | off y -- :}
   IDN @ ALIGNUP DB2 !
   DB2 @ DN @ ALIGNUP + DB3 !
   DB2 @ FIX-REGION2
   0 TO off
   STN @ 0 ?DO
      I ST[] TO y
      y y.class @ c_glo = y y.ini @ 0= AND IF
         off ALIGNUP TO off
         DB3 @ off +  y y.val @ CELLS GOFF + !
         off y y.sz @ + TO off
      THEN
   LOOP
   GOFF FIX-GLOBALS
   off UDN !
   \ the symbols, in declaration order whichever region they landed in
   STN @ 0 ?DO
      I ST[] TO y
      y y.class @ c_glo = IF
         y y.name @ y y.nlen @ y y.ct @ 131
         y y.ini @ IF y y.val @ ELSE y y.val @ CELLS GOFF + @ THEN
         y y.agg @ IF ATTR-ARRAY ELSE 0 THEN  y y.sc @ OR
         SYM,
      THEN
   LOOP ;

: PROGRAM
   BEGIN TK Eof <> WHILE DECL REPEAT
   FIX-FORWARDS
   PLACE-GLOBALS
   DB2 @ EMIT-TABPATS ;
