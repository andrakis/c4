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

8192 CONSTANT NSYM
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
   TK <> IF ." c4fc: " TOK@ .WHERE ." : unexpected token" CR ABORT THEN
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
DEFER BASE-TYPE                         \ sizeof appears inside constants
DEFER STARS ( t -- t' )

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
   ." c4fc: " TOK@ .WHERE ." : a constant was expected" CR ABORT ;

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

\ -- expressions --------------------------------------------------------

DEFER EXPR                              \ ( lev -- node )

: TYPE? TK Int = TK Char = OR TK Struct = OR TK Union = OR ;

\ Members are laid out one cell at a time: c4lc gives a char member a
\ whole cell, which is visible the moment a struct starts with two of
\ them -- { char a; char b; int c; } is twenty-four bytes and b is at
\ eight, not one.
: PARSE-MEMBERS ( k -- ) {: k | t a u off n ag mt et -- :}
   Lbrace WANT
   0 TO off
   BEGIN TK Rbrace <> WHILE
      BASE-TYPE TO t
      BEGIN
         t STARS TO mt
         TV TO a  TL TO u  Id WANT
         1 TO n   0 TO ag
         TK Brak = IF
            TNEXT CONST-EXPR TO n Rbrak WANT
            1 TO ag   mt DUP TO et 2 + TO mt
         THEN
         off 1 CELLS 1- + 1 CELLS 1- INVERT AND TO off
         k a u mt off ag MEM,
         off n ag IF et ELSE mt THEN T-SIZE * + TO off
         TK Comma = WHILE TNEXT
      REPEAT
      Semi WANT
   REPEAT
   Rbrace WANT
   off 1 CELLS 1- + 1 CELLS 1- INVERT AND  k CELLS ST-SIZE + ! ;

: (BASE-TYPE) ( -- t ) {: | t k a u -- t :}
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
         ." c4fc: " TOK@ .WHERE ." : a type was expected" CR ABORT
      THEN THEN
      TNEXT
   THEN
   t ;
' (BASE-TYPE) IS BASE-TYPE
\ Stars belong to the DECLARATOR, not to the base type: `int *a, *b;`
\ is two pointers and `int *a, b;` is a pointer and an int. Parsing them
\ with the base type made the second declarator inherit the first's, and
\ nothing caught it until a real header wrote both forms.
: (STARS) ( t -- t' )  BEGIN TK Mul = WHILE 2 + TNEXT REPEAT ;
' (STARS) IS STARS
: (PARSE-TYPE) ( -- t )  BASE-TYPE STARS ;
' (PARSE-TYPE) IS PARSE-TYPE
: SKIP-TYPE PARSE-TYPE DROP ;

: ARGS ( -- args n ) {: | v n -- :}
   8 CELLS ALLOT: TO v   0 TO n
   TK Rparen <> IF
      BEGIN 1 EXPR v n CELLS + !  n 1+ TO n  TK Comma = WHILE TNEXT REPEAT
   THEN
   Rparen WANT
   v n ;

\ Adjacent string literals are one string, which is how a long message
\ is written across two lines. Expressions only, as in c4lc: an
\ initialiser takes the first literal and nothing more.
\
\ The bytes are copied into the ARENA and the data offset is not handed
\ out until emission, because c4lc allocates its literals during code
\ generation and the tree passes run before that: a string inside
\ `if (0)` costs nothing in c4lc's image and must cost nothing here.
CREATE SLBUF 8192 ALLOT
: STR-LIT ( -- a u ) {: | n d -- a u :}
   0 TO n
   BEGIN TK Str = WHILE
      n TL + 8192 > IF ." c4fc: string literal too long" CR ABORT THEN
      TV SLBUF n + TL MOVE   n TL + TO n   TNEXT
   REPEAT
   n 1+ ALLOT: TO d   SLBUF d n MOVE
   d n ;

: PRIMARY ( -- node ) {: | s a u v k nd ct -- n :}
   TK Num = IF TV TNEXT n_num N1 EXIT THEN
   TK Str = IF STR-LIT n_str N2 EXIT THEN
   \ sizeof(type) folds to a constant, and so does sizeof(array): the
   \ operand is a NAME rather than a type exactly when it lexes as an
   \ identifier, and then it has to be an array, because that is the
   \ only case where the answer is not the size of a machine word.
   TK Sizeof = IF
      TNEXT Lparen WANT
      TK Id = IF
         TV TO a  TL TO u  TNEXT
         a u ST-FIND TO s
         \ The discovery pass runs before anything is known, and a call may
      \ come before the definition with no prototype anywhere -- which
      \ is the very thing that pass exists to find out. Stand a
      \ placeholder in and carry on; its output is thrown away.
      s 0= IF
         COLLECT @ IF
            a u c_fun -1 t_int 0 0 ST,  STN @ 1- ST[] TO s
         ELSE ." c4fc: undeclared " a u TYPE CR ABORT THEN
      THEN
         s y.sz @ 0= IF ." c4fc: sizeof needs an array: " a u TYPE CR ABORT THEN
         s y.sz @  Rparen WANT  n_num N1 EXIT
      THEN
      PARSE-TYPE T-SIZE  Rparen WANT  n_num N1 EXIT
   THEN
   TK Lparen = IF
      TNEXT
      TYPE? IF
         PARSE-TYPE TO ct   Rparen WANT   13 EXPR TO nd
         n_cast 3 CELLS NEW TO v
         nd v >expr !  ct v >ctype !  v EXIT THEN
      1 EXPR TO nd
      TK Comma = IF
         16 CELLS ALLOT: TO v   nd v !  1 TO k
         BEGIN TK Comma = WHILE TNEXT  1 EXPR v k CELLS + !  k 1+ TO k REPEAT
         n_comma 4 CELLS NEW TO nd
         0 nd >fn !  v nd >args !  k nd >argn !
      THEN
      Rparen WANT  nd EXIT
   THEN
   TK Id = IF
      TV TO a  TL TO u  TNEXT
      a u ST-FIND TO s
      \ The discovery pass runs before anything is known, and a call may
      \ come before the definition with no prototype anywhere -- which
      \ is the very thing that pass exists to find out. Stand a
      \ placeholder in and carry on; its output is thrown away.
      s 0= IF
         COLLECT @ IF
            a u c_fun -1 t_int 0 0 ST,  STN @ 1- ST[] TO s
         ELSE ." c4fc: undeclared " a u TYPE CR ABORT THEN
      THEN
      TK Lparen = IF
         TNEXT ARGS TO k TO v
         n_call 4 CELLS NEW TO nd
         s nd >fn !  v nd >args !  k nd >argn !
         nd EXIT
      THEN
      s y.class @ c_const = IF s y.val @ n_num N1 EXIT THEN
      s y.class @ c_fun = s y.class @ c_ext = OR IF s n_fnref N1 EXIT THEN
      s  s y.class @ c_glo = s y.class @ c_extg = OR IF n_gvar ELSE n_var THEN
      N1 EXIT
   THEN
   ." c4fc: " TOK@ .WHERE ." : an expression was expected" CR ABORT ;

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

\ An array or a struct takes as many frame slots as it takes cells, and
\ its name stands for the address of the LOWEST one -- locals grow
\ downwards, so int a[4] as the first local is slots 1..4 and a is
\ LEA -4.
: LOCAL-DECLS ( v -- cnt ) {: v | ct dt t a u sz cnt ag na iv ivn y nd nn -- cnt :}
   0 TO cnt
   BEGIN TYPE? WHILE
      BASE-TYPE TO ct
      BEGIN
         ct STARS TO dt
         TV TO a  TL TO u  Id WANT
         dt TO t   0 TO sz   0 TO ag   -1 TO na
         TK Brak = IF
            TNEXT
            TK Rbrak = IF -1 TO na ELSE CONST-EXPR TO na THEN
            Rbrak WANT
            1 TO ag   dt 2 + TO t
         ELSE
            dt T-STRUCT? IF dt T-SIZE TO sz THEN
         THEN
         \ the initialiser, which is CODE and belongs to the block
         0 TO iv   0 TO ivn   0 TO nd
         TK Assign = IF
            TNEXT
            ag IF
               TK Str = IF
                  TL TO ivn   ivn CELLS ALLOT: TO iv
                  ivn 0 ?DO TV I + C@ iv I CELLS + ! LOOP
                  na 0< IF ivn 1+ TO na THEN
                  TNEXT
               ELSE
                  Lbrace WANT
                  256 CELLS ALLOT: TO iv   0 TO ivn
                  BEGIN TK Rbrace <> WHILE
                     CONST-EXPR iv ivn CELLS + !  ivn 1+ TO ivn
                     TK Comma = IF TNEXT THEN
                  REPEAT
                  Rbrace WANT
                  na 0< IF ivn TO na THEN
               THEN
            ELSE
               1 EXPR TO nd
            THEN
         THEN
         ag IF  na 1 < IF 1 TO na THEN  na dt T-SIZE * TO sz  THEN
         sz 0= IF
            1 NLOC +!
            a u c_loc NLOC @ NEGATE dt 0 0 ST,
         ELSE
            sz 1 CELLS 1- + 1 CELLS / NLOC +!
            a u c_loc NLOC @ NEGATE t 1 sz ST,
         THEN
         nd 0<> ivn 0<> OR IF
            STN @ 1- ST[] TO y
            n_linit 7 CELLS NEW TO nn
            nd nn >expr !  y nn >isym !  iv nn >ivals !  ivn nn >ivn !
            ag IF na ELSE -1 THEN nn >icount !
            dt t_char = ag AND nn >ibyte !
            nn v cnt CELLS + !  cnt 1+ TO cnt
         THEN
         TK Comma = WHILE TNEXT
      REPEAT
      Semi WANT
   REPEAT
   cnt ;

\ Every block takes declarations, not just a function's outermost one:
\ C allows them at the top of any block and real code writes them there.
\ The frame only ever grows -- a nested block's slots are not reused
\ once it closes, which is what c4lc does too.
: COMPOUND ( -- node ) {: | v n -- :}
   Lbrace WANT
   256 CELLS ALLOT: TO v
   v LOCAL-DECLS TO n
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
      n_switch 5 CELLS NEW TO nd
      c nd >cond !  b nd >body !  lo nd >lo !  hi nd >hi !
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
   \ `return;` with no value is a void function leaving, and is a LEV
   \ with nothing computed before it.
   TK Return   = IF
      TNEXT  TK Semi = IF 0 ELSE 1 EXPR THEN  Semi WANT n_ret N1 EXIT THEN
   1 EXPR Semi WANT n_expst N1 ;
' (STATEMENT) IS STATEMENT


: BLOCK ( -- node )  COMPOUND ;

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

\ Skip a function body at the TOKEN level -- no parse, so no string
\ literal is allocated and no symbol is made. A dead function has to
\ cost nothing at all, not merely emit nothing.
: SKIP-BODY ( -- ) {: | d -- :}
   0 TO d
   BEGIN
      TK Eof = IF ." c4fc: unterminated function body" CR ABORT THEN
      TK Lbrace = IF d 1+ TO d THEN
      TK Rbrace = IF d 1- TO d THEN
      TNEXT
      d 0=
   UNTIL ;

: FUNCTION ( a u ct sc at -- )
   {: a u ct sc at | base body y n va base0 fi -- :}
   STN @ TO base0
   a u ST-FIND TO y
   y 0= IF
      a u c_fun -1 ct 0 0 ST,
      STN @ 1- ST[] TO y
   THEN
   STN @ TO base   0 NLOC !
   PARAMS TO va TO n
   va y y.va !   n va - y y.nfix !
   \ Recorded from the PROTOTYPE as well as the definition: a variadic
   \ call site needs __c4cc_make_va, and a unit that only declares it --
   \ every C4IX module does -- still calls it.
   a u S" __c4cc_make_va" NAME=? IF y VA-MAKE ! THEN
   TK Semi = IF                              \ a prototype and nothing more
      TNEXT
      COLLECT @ IF a u ct va PROTO, THEN
      OBJECT @ COLLECT @ 0= AND IF
         a u EXT-FIND TO fi
         fi 0< 0= IF c_ext y y.class !  fi y y.val ! THEN
      THEN
      base STN ! EXIT THEN
   \ Pass two of -O: a function nothing live can reach is skipped whole
   \ -- no symbol, no code, no strings, as if it had not been written.
   OPTIMIZE @ COLLECT @ 0= AND IF
      a u FN-LIVE? 0= IF
         1 NDROP +!  SKIP-BODY  base0 STN !  EXIT THEN
   THEN
   CHERE y y.val !
   a u S" main" NAME=? IF CHERE ENTRY ! THEN
   \ 0x20 marks a variadic function, which is one byte and the last
   \ difference to fall out of the whole of F7.
   a u ct 129 CHERE  sc at OR  va IF 32 OR THEN  SYM,
   at 1 AND IF CHERE CONSN @ CELLS CONS @ + !  1 CONSN +! THEN
   at 2 AND IF CHERE DESN  @ CELLS DESS @ + !  1 DESN  +! THEN
   BLOCK TO body                        \ parse first: ENT needs the count
   OPTIMIZE @ IF body FOLD TO body THEN
   \ Pass one of -O: who does this function reach, and is it a root?
   COLLECT @ IF
      a u FN-DEF TO fi
      fi ct  n va -  va FN-SIG!
      a u DG-DEF
      at 3 AND IF a u ROOT, THEN
      \ In object mode every non-static function is exported, so every
      \ one of them is a root: another unit may call it.
      OBJECT @ sc 8 <> AND IF a u ROOT, THEN
      a u S" main" NAME=? IF a u ROOT, THEN
      a u S" __c4cc_make_va" NAME=? IF a u ROOT, THEN
      fi CURFN !  body REFS  -1 CURFN !
   THEN
   LABEL-RESET
   NLOC @ oENT OP2,
   body STMT
   \ return emits its own LEV, so a function ending in one gets one LEV
   \ -- unless a branch lands here, in which case the LEV that is here
   \ belongs to the arm that took it and the other arm needs its own.
   LAST-OP oLEV <> LABEL-HERE? OR IF oLEV OP, THEN
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

: DECL {: | a u ct dt t n sz ag sc at y ivn off es fa fu fr fs fx fk -- :}
   TK Enum = IF ENUM-DECL EXIT THEN
   STORAGE TO sc
   TYPE? 0= IF ." c4fc: a declaration was expected" CR ABORT THEN
   BASE-TYPE TO ct
   ATTRS TO at
   TK Semi = IF TNEXT EXIT THEN         \ a struct definition and nothing more
   BEGIN
      ct STARS TO dt
      TV TO a  TL TO u  Id WANT
      TK Lparen = IF a u dt sc 0 MAX at FUNCTION EXIT THEN
      dt TO t   0 TO ag   1 CELLS TO sz   -1 TO n   0 TO ivn   dt TO es
      TK Brak = IF
         TNEXT
         TK Rbrak = IF 0 TO n ELSE CONST-EXPR TO n THEN
         Rbrak WANT
         dt 2 + TO t   1 TO ag
      ELSE
         dt T-STRUCT? IF dt T-SIZE TO sz  1 TO ag THEN
      THEN
      \ the initialiser, if there is one
      -1 TO fr   -1 TO fs
      TK Assign = IF
         TNEXT
         TK And = IF                              \ int *fp = &fn;
            TNEXT  TV TO fa  TL TO fu  Id WANT
            fa fu ST-FIND TO y
            y 0= IF ." c4fc: & initialiser needs a function name" CR ABORT THEN
            y y.class @ c_fun <> y y.val @ 0< OR IF
               ." c4fc: & initialiser requires a defined function" CR ABORT THEN
            y y.val @ TO fr
            COLLECT @ IF fa fu ROOT, THEN   \ a global holding &fn is a root
            0 IVAL !  1 TO ivn
         ELSE
         TK Str = IF
            ag IF
               TV TL 1+ TO ivn                    \ the nul counts
               ivn 0 ?DO TV I + C@ IVAL I CELLS + ! LOOP
               0 IVAL ivn 1- CELLS + !
            ELSE
               \ char *s = "..." is a POINTER: the bytes go to region 2
               \ and the global's own word is a data-to-data patch.
               TV TL ID-STR, TO fs   0 IVAL !  1 TO ivn
            THEN
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
         THEN THEN THEN
         ag IF
            n 1 < IF ivn TO n THEN
            n es T-SIZE * TO sz
         THEN
         sz ID-ALLOT TO off
         off es ivn INIT-WRITE
         fr 0< 0= IF off fr FNPAT, THEN
         fs 0< 0= IF off fs STRPAT, THEN
         \ an aggregate records its byte size whether or not it was
         \ initialised, because sizeof(name) is answered from it
         a u c_glo off t ag  ag IF sz ELSE 0 THEN  ST,   \ region 1: final
         STN @ 1- ST[] TO y   1 y y.ini !  sc 0 MAX y y.sc !
      ELSE
         ag IF n 1 < IF 1 TO n THEN n es T-SIZE * TO sz THEN
         \ `extern int x;` with no initialiser declares nothing here.
         \ In object mode it becomes a reference for c4rlink to fill in;
         \ in whole-program mode it is a global like any other.
         COLLECT @ IF
            sc -1 = IF a u t ag IF sz ELSE 0 THEN EXTG, ELSE a u DG-DEF THEN
         THEN
         \ `extern int x;` allocates nothing, whether or not it turns out
         \ to be an extern: if the unit defines x further down, that
         \ definition is what allocates it, and at its own position.
         0 TO fx
         OBJECT @ COLLECT @ 0= AND sc -1 = AND IF
            a u EXT-FIND TO fk
            fk 0< 0= a u ST-FIND 0= AND IF
               a u c_extg fk t ag sz ST,
               STN @ 1- ST[] TO y   0 y y.sc !
            THEN
            1 TO fx
         THEN
         \ extern int x; int x;  is one global, not two -- a repeated
         \ declaration of a name already known is the same object.
         fx 0= IF
            a u ST-FIND TO y
            y IF y y.class @ c_glo <> IF 0 TO y THEN THEN
            y 0= IF
               a u c_glo NGLO @ t ag sz ST,
               STN @ 1- ST[] TO y   sc 0 MAX y y.sc !
               1 NGLO +!
            THEN
         THEN
      THEN
      TK Comma = WHILE TNEXT
   REPEAT
   Semi WANT ;

\ Every function this unit defines, registered before a line of it is
\ read -- so a call that comes before the definition and has no
\ prototype resolves, which is what c4lc's own pre-pass buys. Only the
\ live ones: a function -O has dropped is not there to be called.
: PREREGISTER ( -- ) {: | y -- :}
   #FNS @ 0 ?DO
      OPTIMIZE @ 0= I CELLS FN-LIVE + @ 0<> OR IF
         I CELLS FN-A + @  I CELLS FN-U + @  c_fun -1
         I CELLS FN-T + @  0 0 ST,
         STN @ 1- ST[] TO y
         I CELLS FN-V + @ y y.va !   I CELLS FN-N + @ y y.nfix !
      THEN
   LOOP ;

\ Where the three data regions actually land. Region 1 was placed as it
\ was declared; regions 2 and 3 are relative until now, so every patch
\ that names one gets shifted here.
8192 CONSTANT GMAX
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
