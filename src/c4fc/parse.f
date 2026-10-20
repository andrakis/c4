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

\ The compile-time symbol table is the one buffer here that may NOT be
\ grown, because it is the one whose entries are pointed AT: an n_var
\ node holds the symbol itself, and so do the forward-reference list,
\ VA-MAKE and the switch record. Moving it would leave every one of
\ them pointing into freed memory. It is also the cheapest thing in the
\ compiler -- eight thousand entries is under half a megabyte -- so it
\ is simply sized once and generously, and the abort stays an abort.
\ NSYM, STAB, STN, NLOC, NGLO, TP and ST[] moved to emit.f, next to the
\ SYMR they are all about -- inline.f sits between emit.f and gen.f and
\ needs the symbol table and the frame counter to retarget a spliced
\ body's locals.
: ST, ( a u class val ct agg sz -- ) {: a u c v ct ag sz | y -- :}
   STN @ NSYM < 0= IF ." c4fc: symbol table full" CR ABORT THEN
   STN @ ST[] TO y
   a y y.name !  u y y.nlen !  1 y y.type !  c y y.class !  v y y.val !
   ct y y.ct !  ag y y.agg !  sz y y.sz !
   0 y y.atk !
   0 y y.q !  0 y y.rq !  0 y y.pt !  0 y y.pq !  0 y y.cv !
   1 STN +! ;
\ the symbol just made
: ST-LAST ( -- y )  STN @ 1- ST[] ;
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
\ The qualifiers of the base type just parsed (BQ), and of a declarator
\ after its stars (DQ): set DQ from BQ, call STARS, read DQ back.
VARIABLE BQ   0 BQ !
VARIABLE DQ   0 DQ !


: NAME=? ( a1 u1 a2 u2 -- f ) {: a u b v -- f :}
   u v <> IF 0 EXIT THEN  a b u BYTES= ;

\ -- constant expressions ------------------------------------------------
\ enum bodies and initialisers need values, not code, and they need the
\ same precedence the code path uses -- so this walks the same table and
\ applies the opcodes numerically instead of emitting them. C's
\ comparisons yield 0 or 1 where Forth's yield 0 or -1, which is one AND.

\
\ M4: every value carries its type, in CTY, and two are combined by
\ TFOLD (emit.f) -- the arithmetic the tree pass folds with and the one
\ c4lc's p:tfold does -- so `0xFFFFFFFFu >> 4` is a logical shift here
\ exactly as it is at run time, and a plain int >> is arithmetic, which
\ it was in c4lc and was NOT here: this used Forth's RSHIFT.
DEFER CEXPR
DEFER CONST-EXPR                        \ the whole thing, ?: and all
VARIABLE CTY   1 CTY !
: CAPPLY ( a at b bt op -- v ) {: a at b bt o | v ok -- v :}
   a at b bt o TFOLD TO ok TO v
   ok 0= IF ." c4fc: that operator is not allowed in a constant" CR ABORT THEN
   FTY @ CTY !  v ;

\ a literal's type: what a U suffix makes of it
: LIT-TYPE ( v kind -- t ) {: v k -- t :}
   k NumUL = IF t_ulong EXIT THEN
   k NumU = IF
      W64? IF v 0< v M32 > OR IF t_ulong EXIT THEN THEN
      t_uint EXIT THEN
   t_int ;

: CPRIMARY ( -- v ) {: | s v -- v :}
   TK NUMK?      IF TV TK LIT-TYPE CTY !  TV TNEXT EXIT THEN
   TK Sub    = IF TNEXT 13 CEXPR TO v  0 t_int v CTY @ oSUB CAPPLY EXIT THEN
   TK Add    = IF TNEXT 13 CEXPR EXIT THEN
   TK Not    = IF TNEXT 13 CEXPR 0= 1 AND t_int CTY ! EXIT THEN
   TK Tilde  = IF TNEXT 13 CEXPR TO v  -1 t_int v CTY @ oXOR CAPPLY EXIT THEN
   TK Lparen = IF TNEXT CONST-EXPR Rparen WANT EXIT THEN
   \ sizeof of a type, or of a string literal: its bytes and the NUL (M7b)
   TK Sizeof = IF TNEXT Lparen WANT
      TK Str = IF
         0 TO v  BEGIN TK Str = WHILE v TL + TO v TNEXT REPEAT
         Rparen WANT t_int CTY !  v 1+ EXIT THEN
      PARSE-TYPE T-SIZE Rparen WANT t_int CTY ! EXIT THEN
   TK Id = IF
      TV TL ST-FIND TO s
      s 0= IF ." c4fc: unknown name in a constant" CR ABORT THEN
      \ a static const scalar with a constant initialiser is one too
      s y.cv @ IF TNEXT s y.cty @ CTY !  s y.cval @ EXIT THEN
      s y.class @ c_const <> IF ." c4fc: not a constant" CR ABORT THEN
      TNEXT t_int CTY !  s y.val @ EXIT
   THEN
   ." c4fc: " TOK@ .WHERE ." : a constant was expected" CR ABORT ;

: (CEXPR) ( lev -- v ) {: lev | v vt i p r -- v :}
   CPRIMARY TO v   CTY @ TO vt
   BEGIN
      TK IN-FIND TO i
      i 0< IF vt CTY ! v EXIT THEN
      i CELLS INP + @ TO p
      p lev < IF vt CTY ! v EXIT THEN
      TNEXT
      p 1+ CEXPR TO r
      v vt r CTY @ i CELLS INO + @ CAPPLY TO v   CTY @ TO vt
   AGAIN ;
' (CEXPR) IS CEXPR
\ && and || (M7b), both sides always read: a constant has nothing to
\ skip. Then ?: at the top, which libbb.h's compile-time checks need.
: C-LAND ( -- v ) {: | v r -- v :}
   5 CEXPR TO v
   BEGIN TK Lan = WHILE
      TNEXT 5 CEXPR TO r   v 0<> r 0<> AND 1 AND TO v   t_int CTY !
   REPEAT v ;
: C-LOR ( -- v ) {: | v r -- v :}
   C-LAND TO v
   BEGIN TK Lor = WHILE
      TNEXT C-LAND TO r   v 0<> r 0<> OR 1 AND TO v   t_int CTY !
   REPEAT v ;
: C-COND ( -- v ) {: | c a at b bt -- v :}
   C-LOR TO c
   TK Cond = IF
      TNEXT RECURSE TO a  CTY @ TO at  Colon WANT  RECURSE TO b  CTY @ TO bt
      c IF at CTY ! a ELSE bt CTY ! b THEN EXIT THEN
   c ;
' C-COND IS CONST-EXPR

\ -- static locals (M7b) ---------------------------------------------------
\ A static local is a global named FUNCTION.NAME, as c4lc-parse's
\ p:statics names it: DECL declares it, under that name from the start
\ (so it cannot merge with a global of the plain name), and SALIAS maps
\ the plain name to it inside the function. The records are kept past
\ the function's end by STATICS-KEEP, which FUNCTION calls where it used
\ to drop the whole scope.
DEFER DECL-D
VARIABLE CURFN-A   VARIABLE CURFN-U       \ the function being parsed
VARIABLE ST-MANGLE   0 ST-MANGLE !        \ DECL is declaring a static local
VARIABLE FN-STATICS  0 FN-STATICS !
32 CONSTANT SAL-MAX
CREATE SALA SAL-MAX CELLS ALLOT   CREATE SALU SAL-MAX CELLS ALLOT
CREATE SALMA SAL-MAX CELLS ALLOT  CREATE SALMU SAL-MAX CELLS ALLOT
VARIABLE SALN   0 SALN !
CREATE SLI SAL-MAX CELLS ALLOT   VARIABLE SLN   0 SLN !   \ their STN indices
: STATICS-RESET ( a u -- )  CURFN-U ! CURFN-A !  0 SALN !  0 SLN !  0 FN-STATICS ! ;
: SALIAS ( a u -- a' u' ) {: a u -- a u :}
   SALN @ 0 ?DO
      I CELLS SALU + @ u = IF
         I CELLS SALA + @ a u BYTES= IF
            I CELLS SALMA + @  I CELLS SALMU + @  UNLOOP EXIT THEN
      THEN
   LOOP a u ;
: MANGLE ( a u -- a' u' ) {: a u | d n -- a u :}
   SALN @ SAL-MAX < 0= IF ." c4fc: too many static locals in one function" CR ABORT THEN
   CURFN-U @ 1+ u + TO n   n 1+ ALLOT: TO d
   CURFN-A @ d CURFN-U @ MOVE   46 d CURFN-U @ + C!   a d CURFN-U @ + 1+ u MOVE
   a SALN @ CELLS SALA + !   u SALN @ CELLS SALU + !
   d SALN @ CELLS SALMA + !  n SALN @ CELLS SALMU + !   1 SALN +!
   d n ;
: STATIC-DECL ( -- ) {: | sb -- :}
   STN @ TO sb
   -1 ST-MANGLE !  DECL-D  0 ST-MANGLE !
   STN @ sb ?DO
      SLN @ SAL-MAX < 0= IF ." c4fc: too many static locals in one function" CR ABORT THEN
      I SLN @ CELLS SLI + !  1 SLN +!
   LOOP
   -1 FN-STATICS ! ;
\ The function's scope ends at BASE, but its static locals' records live
\ on, after whatever was at BASE: c4lc declares them just before the
\ function, and among globals that is the same place.
: STATICS-KEEP ( base -- ) {: b | buf -- :}
   SLN @ 0= IF b STN ! EXIT THEN
   SLN @ SYMR * ALLOT: TO buf
   SLN @ 0 ?DO  I CELLS SLI + @ ST[]  buf I SYMR * +  SYMR MOVE  LOOP
   b STN !
   SLN @ 0 ?DO  buf I SYMR * +  STN @ ST[]  SYMR MOVE  1 STN +!  LOOP ;

\ -- function-pointer declarators (M7b) -----------------------------------
\ `T (*name)(params)`, `T (*name[N])(params)`, nameless in a cast or a
\ parameter. c4 calls through any word, so the declarator declares an
\ int, or an array of them: c4lc-parse's p:fnptrdecl. The parameter list
\ is skipped token by token there and here, so a struct first named in
\ it gets an id in neither.
: FNPTR? ( -- f )  TK Lparen =  TP @ 1+ TOKS V@ t.kind @ Mul = AND ;
: SKIP-PARENS ( -- ) {: | d -- :}           \ a ( to its matching ), inclusive
   0 TO d
   BEGIN
      TK Eof = IF ." c4fc: unterminated parameter list" CR ABORT THEN
      TK Lparen = IF d 1+ TO d THEN
      TK Rparen = IF d 1- TO d THEN
      TNEXT
      d 0=
   UNTIL ;
\ ( * [qualifiers] [NAME] [[N]] ) ( ... ): the name (0 0 when there is
\ none) and the array size (-1 when it is no array, 0 for [])
: FNPTR-DECL ( -- a u n ) {: | a u n -- a u n :}
   TNEXT TNEXT
   BEGIN TK Const = TK Volatile = OR WHILE TNEXT REPEAT
   0 TO a  0 TO u  -1 TO n
   TK Id = IF TV TO a  TL TO u  TNEXT THEN
   TK Brak = IF
      TNEXT  TK Rbrak = IF 0 TO n ELSE CONST-EXPR TO n THEN  Rbrak WANT
   THEN
   Rparen WANT
   TK Lparen <> IF ." c4fc: the parameter list of a function pointer was expected" CR ABORT THEN
   SKIP-PARENS
   a u n ;

\ -- nodes --------------------------------------------------------------

\ -- expressions --------------------------------------------------------

DEFER EXPR                              \ ( lev -- node )

: KIND-TYPE? ( k -- f ) {: k -- f :}
   k Int = k Char = OR k Void = OR k Struct = OR k Union = OR k Enum = OR
   k Const = OR k Volatile = OR k Signed = OR k Unsigned = OR
   k Short = OR k Long = OR ;
: TYPE? ( -- f )  TK KIND-TYPE? ;

\ Members are laid out one cell at a time: c4lc gives a char member a
\ whole cell, which is visible the moment a struct starts with two of
\ them -- { char a; char b; int c; } is twenty-four bytes and b is at
\ eight, not one.
\
\ A UNION lays every member at 0 and is as big as its biggest, rounded up
\ to a word, as c4lc-gen's g:slayout does it. It used to be laid out as
\ a struct here, so x.c of `union { int i; char c; } x` was at 8 and
\ sizeof was 16 (found by M7b's aggregate initializers).
: PARSE-MEMBERS ( k isu -- ) {: k isu | t a u off n ag mt et bq q fn mx sz -- :}
   Lbrace WANT
   0 TO off   0 TO mx
   BEGIN TK Rbrace <> WHILE
      BASE-TYPE TO t   BQ @ TO bq
      BEGIN
         bq DQ !  t STARS TO mt   DQ @ TO q
         1 TO n   0 TO ag
         FNPTR? IF
            FNPTR-DECL TO fn TO u TO a   t_int TO mt   0 TO q
            a 0= IF ." c4fc: bad struct member name" CR ABORT THEN
            fn 0< 0= IF fn TO n  1 TO ag  mt DUP TO et 2 + TO mt THEN
         ELSE
            TV TO a  TL TO u  Id WANT
            TK Brak = IF
               TNEXT CONST-EXPR TO n Rbrak WANT
               1 TO ag   mt DUP TO et 2 + TO mt   q 2 LSHIFT TO q
            THEN
         THEN
         off 1 CELLS 1- + 1 CELLS 1- INVERT AND TO off
         k a u mt off ag MEM,   q MEM-Q!   ag IF n et T-SIZE * MEM-ASZ! THEN
         n ag IF et ELSE mt THEN T-SIZE * TO sz
         isu IF
            sz 1 CELLS 1- + 1 CELLS 1- INVERT AND  DUP mx > IF TO mx ELSE DROP THEN
         ELSE off sz + TO off THEN
         TK Comma = WHILE TNEXT
      REPEAT
      Semi WANT
   REPEAT
   Rbrace WANT
   isu IF mx ELSE off 1 CELLS 1- + 1 CELLS 1- INVERT AND THEN  k CELLS ST-SIZE + ! ;

\ Any run of the specifiers C allows -- const, volatile, signed,
\ unsigned, short, long, int, char, a struct or union -- in any order.
\ enum [TAG] [{ A, B = 5, C }] as a type specifier (M7b): an int, whose
\ constants the body defines -- c4lc-parse's p:enumspec.
: ENUM-SPEC ( -- ) {: | a u v -- :}
   Enum WANT
   TK Id = IF TNEXT THEN                \ an optional tag, which c4 ignores
   TK Lbrace = IF
      TNEXT  0 TO v
      BEGIN TK Rbrace <> WHILE
         TV TO a  TL TO u  Id WANT
         TK Assign = IF TNEXT CONST-EXPR TO v THEN
         a u c_const v t_int 0 0 ST,
         v 1+ TO v
         TK Comma = IF TNEXT THEN
      REPEAT
      Rbrace WANT
   THEN ;

: (BASE-TYPE) ( -- t ) {: | k a u q sg un sh lg ch it st isu -- t :}
   0 TO q  0 TO sg  0 TO un  0 TO sh  0 TO lg  0 TO ch  0 TO it  -1 TO st
   BEGIN
      TK Const    = IF q 1 OR TO q  TNEXT 1 ELSE
      TK Volatile = IF q 2 OR TO q  TNEXT 1 ELSE
      TK Signed   = IF 1 TO sg TNEXT 1 ELSE
      TK Unsigned = IF 1 TO un TNEXT 1 ELSE
      TK Short    = IF 1 TO sh TNEXT 1 ELSE
      TK Long     = IF lg 1+ TO lg TNEXT 1 ELSE
      TK Char = TK Void = OR IF 1 TO ch TNEXT 1 ELSE
      TK Int      = IF 1 TO it TNEXT 1 ELSE
      TK Struct = TK Union = OR st 0< AND
         sg un + sh + lg + ch + it + 0= AND IF
         TK Union = TO isu
         TNEXT
         0 TO a  0 TO u
         TK Id = IF TV TO a  TL TO u  TNEXT THEN
         a u ST-FINDS TO k
         k 0< IF a u ST-NEW TO k THEN
         TK Lbrace = IF k isu PARSE-MEMBERS THEN
         k ST-TYPE TO st  1
      ELSE
      TK Enum = st 0< AND
         sg un + sh + lg + ch + it + 0= AND IF ENUM-SPEC  1 TO it  1
      ELSE 0
      THEN THEN THEN THEN THEN THEN THEN THEN THEN THEN
   WHILE REPEAT
   q BQ !
   st 0< 0= IF st EXIT THEN
   ch IF un IF t_uchar ELSE sg IF t_schar ELSE t_char THEN THEN EXIT THEN
   sh IF un IF t_ushort ELSE t_short THEN EXIT THEN
   lg IF un IF t_ulong ELSE t_int THEN EXIT THEN
   un IF t_uint EXIT THEN
   sg it OR q OR IF t_int EXIT THEN
   ." c4fc: " TOK@ .WHERE ." : a type was expected" CR ABORT ;
' (BASE-TYPE) IS BASE-TYPE
\ Stars belong to the DECLARATOR, not to the base type: `int *a, *b;`
\ is two pointers and `int *a, b;` is a pointer and an int. Parsing them
\ with the base type made the second declarator inherit the first's, and
\ nothing caught it until a real header wrote both forms.
\ Each star moves DQ up a level, and its own const/volatile follow it.
: (STARS) ( t -- t' )
   BEGIN
      TK Mul = IF 2 + DQ @ 2 LSHIFT DQ ! TNEXT 1 ELSE
      TK Const = IF DQ @ 1 OR DQ ! TNEXT 1 ELSE
      TK Volatile = IF DQ @ 2 OR DQ ! TNEXT 1 ELSE 0 THEN THEN THEN
   WHILE REPEAT ;
' (STARS) IS STARS
: (PARSE-TYPE) ( -- t )  BASE-TYPE  BQ @ DQ !  STARS ;
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

\ What sizeof says of an expression: a string literal is its bytes and
\ the NUL, a named array its storage, an array member its storage, and
\ anything else its type's size -- c4lc-gen's g:sizeofx and sizeofa.
: SIZEOF-NODE ( nd -- n ) {: nd | k s -- n :}
   nd @ TO k
   k n_str = IF nd >slen @ 1+ EXIT THEN
   k n_var = k n_gvar = OR IF
      nd >sym @ TO s
      s y.agg @ IF s y.sz @ ELSE s y.ct @ T-SIZE THEN EXIT
   THEN
   k n_member = IF
      nd >magg @ IF nd >masz @ ELSE nd >mtype @ T-SIZE THEN EXIT
   THEN
   nd CT T-SIZE ;

: PRIMARY ( -- node ) {: | s a u v k nd ct -- n :}
   TK Num = IF TV TNEXT t_int NUM-OF EXIT THEN
   TK NUMK? IF
      TV TK LIT-TYPE TO ct
      TV  ct t_uint = IF M32 AND THEN  ct NUM-OF  TNEXT EXIT THEN
   TK Str = IF STR-LIT n_str N2 EXIT THEN
   \ sizeof folds to a constant here, at parse time (M7b: of any
   \ expression, c4lc's sizeofa and sizeofe). A type in parentheses is a
   \ type; `sizeof(T[n])` is sizeof(T) * n, which is how Busybox writes
   \ BUILD_BUG_ON. Anything else is a unary expression, parenthesized or
   \ not, whose size SIZEOF-NODE reads off it without generating it.
   TK Sizeof = IF
      TNEXT
      TK Lparen =  TP @ 1+ TOKS V@ t.kind @ KIND-TYPE? AND IF
         TNEXT PARSE-TYPE T-SIZE t_int NUM-OF TO v
         TK Brak = IF
            TNEXT  1 EXPR TO nd  Rbrak WANT  Rparen WANT
            n_bin 4 CELLS NEW TO s
            v s >lhs !  nd s >rhs !  oMUL s >op !  s EXIT
         THEN
         Rparen WANT  v EXIT
      THEN
      13 EXPR SIZEOF-NODE t_int NUM-OF EXIT
   THEN
   TK Lparen = IF
      TNEXT
      TYPE? IF
         PARSE-TYPE TO ct   DQ @ TO k
         FNPTR? IF FNPTR-DECL DROP 2DROP  t_int TO ct  0 TO k THEN
         Rparen WANT   13 EXPR TO nd
         n_cast 4 CELLS NEW TO v
         nd v >expr !  ct v >ctype !  k v >cq !  v EXIT THEN
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
      \ __func__ (M7b): the function's name, a string, as c4lc's p:idexpr
      a u S" __func__" NAME=?  a u S" __FUNCTION__" NAME=? OR
      a u S" __PRETTY_FUNCTION__" NAME=? OR IF
         CURFN-A @ CURFN-U @ n_str N2 EXIT THEN
      a u SALIAS TO u TO a                \ a static local's own global
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
      s y.class @ c_const = IF s y.val @ t_int NUM-OF EXIT THEN
      \ A function's name used as a value: its address escapes here, and
      \ this is the only place that can happen. Recording it HERE is why
      \ the inliner needs no walker to find address-taken functions --
      \ and why it cannot repeat c4lc's mistake of knowing only one of
      \ the two spellings (docs/inline-small-functions.md).
      s y.class @ c_fun = s y.class @ c_ext = OR IF
         1 s y.atk !
         s y.name @ s y.nlen @ ATK,
         s n_fnref N1 EXIT THEN
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
   n_member 7 CELLS NEW TO nd
   b nd >lhs !
   i CELLS MB-OFF  + @ nd >moff !
   i CELLS MB-TYPE + @ nd >mtype !
   i CELLS MB-AGG  + @ nd >magg !
   i CELLS MB-QUAL + @ nd >mq !
   i CELLS MB-ASZ  + @ nd >masz !
   nd ;

: POSTFIX ( -- node ) {: | n k v nd -- n :}
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
      ELSE TK Lparen = IF
         \ a call through any expression (M7b); (*fp)(x) calls fp
         TNEXT  ARGS TO k TO v
         n @ n_deref = IF n >opnd @ TO n THEN
         n_icall 4 CELLS NEW TO nd
         n nd >fn !  v nd >args !  k nd >argn !  nd TO n
      ELSE n EXIT
      THEN THEN THEN THEN THEN THEN
   AGAIN ;

\ Unary operators bind tighter than any infix one, so each parses its
\ operand at the Inc level. Negating a literal folds, as c4 does it --
\ which is why -1 is one IMM and not a multiply.
: UNARY ( -- node )
   TK Not   = IF TNEXT 13 EXPR n_not   N1 EXIT THEN
   TK Tilde = IF TNEXT 13 EXPR n_bnot  N1 EXIT THEN
   TK Sub   = IF TNEXT
                 TK Num = IF TV NEGATE TNEXT t_int NUM-OF EXIT THEN
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
: LOCAL-DECLS ( v -- ) {: v | ct dt t a u sz ag na iv ivn y nd nn bq q fn -- :}
   BEGIN TYPE? TK Static = OR WHILE
      TK Static = IF STATIC-DECL ELSE
      BASE-TYPE TO ct   BQ @ TO bq
      BEGIN
         bq DQ !  ct STARS TO dt   DQ @ TO q
         FNPTR? IF
            FNPTR-DECL TO fn TO u TO a   t_int TO dt   0 TO q
            a 0= IF ." c4fc: bad local declaration" CR ABORT THEN
         ELSE
            TV TO a  TL TO u  Id WANT   -2 TO fn
         THEN
         dt TO t   0 TO sz   0 TO ag   -1 TO na
         fn -2 = IF TK Brak = ELSE fn 0< 0= THEN IF
            fn -2 = IF
               TNEXT
               TK Rbrak = IF -1 TO na ELSE CONST-EXPR TO na THEN
               Rbrak WANT
            ELSE fn TO na THEN
            1 TO ag   dt 2 + TO t   q 2 LSHIFT TO q
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
         q ST-LAST y.q !
         nd 0<> ivn 0<> OR IF
            STN @ 1- ST[] TO y
            n_linit 7 CELLS NEW TO nn
            nd nn >expr !  y nn >isym !  iv nn >ivals !  ivn nn >ivn !
            ag IF na ELSE -1 THEN nn >icount !
            \ how an element is stored: 1 a byte, 2 two bytes, 0 a word
            ag IF dt T-BYTE? IF 1 ELSE dt T-HALF? IF 2 ELSE 0 THEN THEN
            ELSE 0 THEN nn >ibyte !
            nn v V,
         THEN
         TK Comma = WHILE TNEXT
      REPEAT
      Semi WANT
      THEN
   REPEAT ;

\ Every block takes declarations, not just a function's outermost one:
\ C allows them at the top of any block and real code writes them there.
\ The frame only ever grows -- a nested block's slots are not reused
\ once it closes, which is what c4lc does too.
\
\ The statements are gathered in a GROWABLE vector and then copied into
\ the arena at their real size. They used to go straight into a fixed
\ 256-cell array with nothing checking the count, which is fine until a
\ switch with twenty-five arms writes past the end of it.
: COMPOUND ( -- node ) {: | v a n -- :}
   Lbrace WANT
   VEC ALLOT: TO v   v 64 VEC-INIT
   v LOCAL-DECLS
   BEGIN TK Rbrace <> WHILE  STATEMENT v V,  REPEAT
   Rbrace WANT
   v V# TO n
   n 1 MAX CELLS ALLOT: TO a
   n 0 ?DO I v V@  a I CELLS + !  LOOP
   a n n_blk N2 ;

256 CONSTANT CVMAX
CREATE CVAL CVMAX CELLS ALLOT   VARIABLE CVN   0 CVN !
: CASE-VALUE ( -- v )  CONST-EXPR ;

: (STATEMENT) ( -- node ) {: | c b e i s nd lo hi a u -- n :}
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
   \ goto and labels (M7b). `goto` is no keyword, here or in c4lc, so
   \ the statement is known by its shape: goto NAME ;  and  NAME :
   \ A label just before the closing brace labels an empty statement.
   TK Id = IF
      TV TL S" goto" NAME=?  TP @ 1+ TOKS V@ t.kind @ Id = AND IF
         TNEXT  TV TO a  TL TO u  TNEXT  Semi WANT   -1 FN-LABELS !
         n_goto 4 CELLS NEW TO nd
         a nd >lname !  0 nd >body !  u nd >llen !  nd EXIT
      THEN
      TP @ 1+ TOKS V@ t.kind @ Colon = IF
         TV TO a  TL TO u  TNEXT TNEXT   -1 FN-LABELS !
         TK Rbrace = IF n_empty 1 CELLS NEW ELSE STATEMENT THEN TO b
         n_labeled 4 CELLS NEW TO nd
         a nd >lname !  b nd >body !  u nd >llen !  nd EXIT
      THEN
   THEN
   1 EXPR Semi WANT n_expst N1 ;
' (STATEMENT) IS STATEMENT


: BLOCK ( -- node )  COMPOUND ;

\ -- declarations -------------------------------------------------------

\ ... is not a special form: it is one more parameter, unnamed, and the
\ CALL SITE is where the work happens. That is why int vsum(int n, ...)
\ finds n at bp+3 and not bp+2.
\
\ `(void)` is no parameters at all. A parameter may have no name, as in
\ a prototype's `int f(int, char *)`; it gets an empty one, which no
\ lookup can find.
: PARAMS ( -- n va ) {: | n base ct va a u -- n va :}
   Lparen WANT
   STN @ TO base   0 TO n   0 TO va
   TK Void =  TP @ 1+ TOKS V@ t.kind @ Rparen = AND IF TNEXT THEN
   TK Rparen <> IF
      BEGIN
         TK Dot = IF
            TNEXT Dot WANT Dot WANT
            1 TO va   n 1+ TO n
            0
         ELSE
            PARSE-TYPE TO ct
            FNPTR? IF
               FNPTR-DECL DROP TO u TO a   t_int TO ct
               a u c_loc 0 ct 0 0 ST,   0 ST-LAST y.q !   n 1+ TO n
            ELSE
            TK Id = IF TV TL ELSE S" " THEN
            c_loc 0 ct 0 0 ST,   DQ @ ST-LAST y.q !   n 1+ TO n
            TK Id = IF TNEXT ELSE
               TK Comma <> TK Rparen <> AND IF Id WANT THEN
            THEN
            THEN
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
\ The parameters' types and masks, as two arrays of n cells: what a call
\ needs for the const check (M4), and what PREREGISTER carries across
\ the two passes.
: PSIG ( base n -- pt pq ) {: b n | pt pq -- pt pq :}
   n 1 MAX CELLS ALLOT: TO pt   n 1 MAX CELLS ALLOT: TO pq
   n 0 ?DO
      b I + ST[] y.ct @  pt I CELLS + !
      b I + ST[] y.q @   pq I CELLS + !
   LOOP
   pt pq ;
: SIG! ( y rq pt pq -- ) {: y rq pt pq -- :}
   rq y y.rq !  pt y y.pt !  pq y y.pq ! ;

: SKIP-BODY ( -- ) {: | d -- :}
   0 TO d
   BEGIN
      TK Eof = IF ." c4fc: unterminated function body" CR ABORT THEN
      TK Lbrace = IF d 1+ TO d THEN
      TK Rbrace = IF d 1- TO d THEN
      TNEXT
      d 0=
   UNTIL ;

: FUNCTION ( a u ct sc at rq -- )
   {: a u ct sc at rq | base body y n va base0 fi entp inlsv sn bodytp pt pq -- :}
   STN @ TO base0
   a u ST-FIND TO y
   y 0= IF
      a u c_fun -1 ct 0 0 ST,
      STN @ 1- ST[] TO y
   THEN
   STN @ TO base   0 NLOC !
   PARAMS TO va TO n
   va y y.va !   n va - y y.nfix !
   base n va - PSIG TO pq TO pt
   \ Recorded from the PROTOTYPE as well as the definition: a variadic
   \ call site needs __c4cc_make_va, and a unit that only declares it --
   \ every C4IX module does -- still calls it.
   a u S" __c4cc_make_va" NAME=? IF y VA-MAKE ! THEN
   TK Semi = IF                              \ a prototype and nothing more
      TNEXT
      \ a definition's signature wins over a prototype's, as in c4lc
      y y.pt @ 0= IF y rq pt pq SIG! THEN
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
   y rq pt pq SIG!
   a u S" main" NAME=? IF CHERE ENTRY ! THEN
   \ 0x20 marks a variadic function, which is one byte and the last
   \ difference to fall out of the whole of F7.
   a u ct 129 CHERE  sc at OR  va IF 32 OR THEN  SYM,
   at 1 AND IF CHERE CONSN @ CELLS CONS @ + !  1 CONSN +! THEN
   at 2 AND IF CHERE DESN  @ CELLS DESS @ + !  1 DESN  +! THEN
   TP @ TO bodytp                       \ for the inliner's size budget
   0 FN-LABELS !
   a u STATICS-RESET
   BLOCK TO body                        \ parse first: ENT needs the count
   OPTIMIZE @ IF body FOLD TO body THEN
   \ Pass one of -O: who does this function reach, and is it a root?
   COLLECT @ IF
      a u FN-DEF TO fi
      fi ct  n va -  va FN-SIG!
      fi rq pt pq FN-SIG2!
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
   \ Remember where the ENT is. A splice inside the body allocates more
   \ frame words, and by then this instruction is already emitted -- so
   \ its operand is patched afterwards rather than guessed at up front.
   \ OP2, lays down [op][operand], so the operand is the next word.
   CHERE TO entp
   NLOC @ oENT OP2,
   \ A function whose address is taken must not be spliced INTO: C4KE
   \ reads its ENT operand and jumps past it, so the frame that operand
   \ describes is a contract with code elsewhere, and a splice grows it.
   INLINING @ TO inlsv
   a u ATK? IF 0 INLINING ! THEN
   a u FN-NAME!  ct rq FN-RET!
   ULAB-RESET
   body STMT
   ULAB-CHECK
   inlsv INLINING !
   NLOC @ entp 1+ CELLS CODE @ + !
   \ return emits its own LEV, so a function ending in one gets one LEV
   \ -- unless a branch lands here, in which case the LEV that is here
   \ belongs to the arm that took it and the other arm needs its own.
   LAST-OP oLEV <> LABEL-HERE? OR IF oLEV OP, THEN
   \ Worth splicing into its callers?
   \
   \ Size is measured in TOKENS -- the span this body consumed -- which
   \ the parser already knows and which needs no walk over the tree.
   \
   \ The parameters and locals are NOT dropped for a candidate: its body
   \ tree points at those symbol records, and a splice retargets their
   \ y.val to slots in the caller. Blanking the name length instead
   \ keeps them alive but unfindable, because ST-FIND matches on
   \ `y.nlen @ u =` and no identifier has length zero.
   STN @ base - TO sn
   INLINING @ COLLECT @ 0= AND
   va 0= AND  at 3 AND 0= AND  y y.atk @ 0= AND
   a u ATK? 0= AND
   a u S" main" NAME=? 0= AND
   TP @ bodytp - INLMAX @ <= AND
   sn 0> AND
   FN-LABELS @ 0= AND                   \ a label spliced twice is placed twice
   FN-STATICS @ 0= AND                  \ a splice would retarget their records
   IF
      y body base sn n va - IC,
      sn 0 ?DO  0 base I + ST[] y.nlen !  LOOP
      EXIT
   THEN
   base STATICS-KEEP ;                  \ parameters and locals go out of scope

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
: INIT-WRITE ( off elemtype n -- ) {: off et n | v -- :}
   n 0 ?DO
      IVAL I CELLS + @ TO v
      et T-BYTE? IF v off I + ID-C! ELSE
      et T-HALF? IF
         v 255 AND  off I 2* + ID-C!
         v 8 RSHIFT 255 AND  off I 2* 1+ + ID-C!
      ELSE v off I CELLS + ID-! THEN THEN
   LOOP ;

\ a record DECL has retired: no class anything walks, and no name
7 CONSTANT c_gone

\ -- aggregate initializers (M7b) ------------------------------------------
\ c4lc-gen's g:globalaggr, word for word. An initializer is first a tree:
\   kind 0 a number (cell 1), 1 a string (address, length), 2 a name
\   (address, length), 3 a brace list (a vector of nodes), 4 `[I] = X`
\   (I, X), 5 `.M = X` (address, length, X)
\ and then it is walked against the type twice, the same way both times:
\ AMODE 0 puts every string a POINTER takes into region 1, in order,
\ before the global -- as `char *s = "..."` always has -- and AMODE 1
\ writes each value at its offset in the global's zeroed storage, a string
\ pointer or a function address becoming a data patch where it lies. A
\ value is a byte, two bytes or a word by its type, as INIT-WRITE writes
\ them. An aggregate met without braces takes its elements from the
\ enclosing list (C's brace elision).
: INIT-NODE ( kind a b c -- node ) {: k a b c | n -- n :}
   4 CELLS ALLOT: TO n  k n !  a n CELL+ !  b n 2 CELLS + !  c n 3 CELLS + !  n ;
\ a name standing alone that no constant answers: a function, presumably
: INIT-NAME? ( -- f ) {: | y nk -- f :}
   TK Id <> IF 0 EXIT THEN
   TP @ 1+ TOKS V@ t.kind @ TO nk
   nk Comma = nk Rbrace = OR nk Semi = OR 0= IF 0 EXIT THEN
   TV TL ST-FIND TO y
   y 0= IF -1 EXIT THEN
   y y.class @ c_const = y y.cv @ OR 0= ;
DEFER INIT-ITEM
: INIT-BRACES ( -- node ) {: | v -- n :}
   Lbrace WANT   16 NEW-VEC TO v
   BEGIN TK Rbrace <> WHILE
      INIT-ITEM v V,
      TK Comma = IF TNEXT THEN
   REPEAT
   Rbrace WANT
   3 v 0 0 INIT-NODE ;
: INIT-LEAF ( -- node ) {: | a u -- n :}
   TK Lbrace = IF INIT-BRACES EXIT THEN
   TK Str = IF STR-LIT TO u TO a  1 a u 0 INIT-NODE EXIT THEN
   TK And = IF
      TNEXT  TK Id <> IF ." c4fc: & initialiser requires a function name" CR ABORT THEN
      TV TO a  TL TO u  TNEXT  2 a u 0 INIT-NODE EXIT THEN
   INIT-NAME? IF TV TO a  TL TO u  TNEXT  2 a u 0 INIT-NODE EXIT THEN
   0 CONST-EXPR 0 0 INIT-NODE ;
: (INIT-ITEM) ( -- node ) {: | c a u -- n :}
   TK Brak = IF
      TNEXT CONST-EXPR TO c  Rbrak WANT  Assign WANT
      4 c INIT-LEAF 0 INIT-NODE EXIT THEN
   TK Dot =  TP @ 1+ TOKS V@ t.kind @ Id = AND IF
      TNEXT  TV TO a  TL TO u  TNEXT  Assign WANT
      5 a u INIT-LEAF INIT-NODE EXIT THEN
   INIT-LEAF ;
' (INIT-ITEM) IS INIT-ITEM

\ a data word at SLOT holding global Y's address (M7b), c4lc-gen's
\ g:fnpatch: an extern's symbol, an initialised global's offset, or an
\ uninitialised one's number, for PLACE-GLOBALS to place
: GLOBAL-PAT ( slot y -- ) {: o y -- :}
   y y.class @ c_extg = IF o y y.val @ SYMPAT, EXIT THEN
   y y.ini @ IF o y y.val @ STRPAT, ELSE o y y.val @ GLOPAT, THEN ;
: GLOBAL? ( y -- f )  y.class @ DUP c_glo = SWAP c_extg = OR ;
\ a name in a scalar's initializer: -> fr fk gy, where fr is a function's
\ code address (-1 none, -3 an extern's, then fk is its number) and gy a
\ global's record (0 none)
: INIT-ADDR ( a u -- fr fk gy ) {: a u | y -- fr fk gy :}
   a u ST-FIND TO y
   y 0= IF ." c4fc: an initialiser can name only a function or a global" CR ABORT THEN
   y GLOBAL? IF -1 0 y EXIT THEN
   COLLECT @ IF a u ROOT, THEN          \ a global holding &fn is a root
   y y.class @ c_ext = IF -3 y y.val @ 0 EXIT THEN
   COLLECT @ y y.class @ c_fun = AND y y.val @ 0< AND IF 0 0 0 EXIT THEN   \ pass one
   y y.class @ c_fun <> y y.val @ 0< OR IF
      ." c4fc: & initialiser requires a defined function or a global" CR ABORT THEN
   y y.val @ 0 0 ;

\ how many elements a brace list fills: designators move the index
: AGG-COUNT ( node -- n ) {: nd | v i mx j e -- n :}
   nd CELL+ @ TO v   0 TO i   0 TO mx
   v V# 0 ?DO
      I v V@ TO e
      e @ 4 = IF e CELL+ @ ELSE i THEN TO j
      j 1+ TO i   i mx > IF i TO mx THEN
   LOOP mx ;

VARIABLE AMODE   0 AMODE !
4096 CONSTANT SQ-MAX
CREATE SQ SQ-MAX CELLS ALLOT   VARIABLE SQN   VARIABLE SQI
: SQ, ( off -- )
   SQN @ SQ-MAX < 0= IF ." c4fc: too many strings in one initializer" CR ABORT THEN
   SQ SQN @ CELLS + !  1 SQN +! ;
: SQ> ( -- off )  SQ SQI @ CELLS + @  1 SQI +! ;

: AGG-PUT ( v ty off -- ) {: v t o -- :}
   t T-BYTE? IF v o ID-C! EXIT THEN
   t T-HALF? IF v 255 AND o ID-C!  v 8 RSHIFT 255 AND o 1+ ID-C! EXIT THEN
   v o ID-! ;
: AGG-SCALAR ( ty off node -- ) {: t o nd | k y -- :}
   nd @ TO k
   k 0 = IF AMODE @ IF nd CELL+ @ t o AGG-PUT THEN EXIT THEN
   k 1 = IF
      AMODE @ 0= IF nd CELL+ @ nd 2 CELLS + @ ID-STR, SQ,
      ELSE o SQ> STRPAT, THEN EXIT THEN
   k 2 = IF
      AMODE @ 0= IF EXIT THEN
      nd CELL+ @ nd 2 CELLS + @ ST-FIND TO y
      y IF y GLOBAL? IF  o y GLOBAL-PAT  EXIT THEN THEN
      y IF y y.class @ c_ext = IF  o y y.val @ SYMPAT,  EXIT THEN THEN
      \ pass one of -O or -c makes its externs only at its end, and its
      \ output is thrown away: a prototype stands in for now
      COLLECT @ IF y IF y y.class @ c_fun = IF
         nd CELL+ @ nd 2 CELLS + @ ROOT,  EXIT THEN THEN THEN
      y IF y y.class @ c_fun <> y y.val @ 0< OR IF 0 TO y THEN THEN
      y 0= IF ." c4fc: an initializer can name only a function or a global: "
         nd CELL+ @ nd 2 CELLS + @ TYPE CR ABORT THEN
      o y y.val @ FNPAT,
      COLLECT @ IF nd CELL+ @ nd 2 CELLS + @ ROOT, THEN   \ a table is what reaches it
      EXIT THEN
   ." c4fc: unsupported initializer element" CR ABORT ;
\ `{ 5 }` for a scalar is 5
: AGG-UNWRAP ( node -- node' )
   BEGIN DUP @ 3 = WHILE
      CELL+ @  DUP V# 1 <> IF ." c4fc: too many initializers for a scalar" CR ABORT THEN
      0 SWAP V@
   REPEAT ;
: AGG-STRFILL ( cnt off node -- ) {: c o nd -- :}
   nd 2 CELLS + @ c > IF ." c4fc: a string does not fit its array" CR ABORT THEN
   AMODE @ IF nd 2 CELLS + @ 0 ?DO nd CELL+ @ I + C@ o I + ID-C! LOOP THEN ;

DEFER AGG-FILL ( ety cnt off v i -- i' )
\ an array's elements from V at I; BR is false when they come from an
\ enclosing list, which keeps what this array does not take
: AGG-ARR ( ety cnt off v i br -- i' ) {: et c o v i br | idx nd sv j -- i :}
   0 TO idx
   BEGIN i v V# < WHILE
      i v V@ TO nd   0 TO sv
      nd @ 4 = IF
         br 0= IF i EXIT THEN
         nd CELL+ @ TO idx   nd TO sv   nd 2 CELLS + @ i v V!
      THEN
      idx c < 0= IF
         sv IF sv i v V! THEN
         br IF ." c4fc: too many initializers for an array" CR ABORT THEN
         i EXIT THEN
      i TO j
      et -1  o idx et T-SIZE * +  v i AGG-FILL TO i
      sv IF sv j v V! THEN
      idx 1+ TO idx
   REPEAT i ;
\ the member after J of struct K, or -1
: NEXT-MEM ( k j -- j' ) {: k j -- j :}
   #MEMS @ j 1+ ?DO I CELLS MB-OWNER + @ k = IF I UNLOOP EXIT THEN LOOP -1 ;
\ a union lays every member at 0, and takes one initializer
: UNION? ( k -- f ) {: k | m n -- f :}
   0 TO n   k -1 NEXT-MEM TO m
   BEGIN m 0< 0= WHILE
      m CELLS MB-OFF + @ IF 0 EXIT THEN
      n 1+ TO n   k m NEXT-MEM TO m
   REPEAT n 1 > ;
: AGG-STRUCT ( sty off v i br -- i' ) {: st o v i br | k m nd sv uni mt j -- i :}
   st T-INDEX TO k   k -1 NEXT-MEM TO m   k UNION? TO uni
   BEGIN i v V# < WHILE
      i v V@ TO nd   0 TO sv
      nd @ 5 = IF
         br 0= IF i EXIT THEN
         k nd CELL+ @ nd 2 CELLS + @ MEM-FIND TO m
         m 0< IF ." c4fc: no such member in a designator: "
            nd CELL+ @ nd 2 CELLS + @ TYPE CR ABORT THEN
         nd TO sv   nd 3 CELLS + @ i v V!
      THEN
      m 0< IF
         sv IF sv i v V! THEN
         br IF ." c4fc: too many initializers for a struct" CR ABORT THEN
         i EXIT THEN
      m CELLS MB-TYPE + @ TO mt   i TO j
      m CELLS MB-AGG + @ IF
         mt T-DEREF  m CELLS MB-ASZ + @ mt T-DEREF T-SIZE /
      ELSE mt -1 THEN
      o m CELLS MB-OFF + @ +  v i AGG-FILL TO i
      sv IF sv j v V! THEN
      uni IF -1 ELSE k m NEXT-MEM THEN TO m
   REPEAT i ;
: AGG-LIST ( ety cnt off v i br -- i' ) {: et c o v i br -- i :}
   c 0< IF et o v i br AGG-STRUCT ELSE et c o v i br AGG-ARR THEN ;
\ one object of element type ETY (an array of CNT, or -1 for none) at
\ OFF, from the node at I in V
: (AGG-FILL) ( ety cnt off v i -- i' ) {: et c o v i | nd k -- i :}
   i v V@ TO nd   nd @ TO k
   c 0< et T-STRUCT? 0= AND IF  et o nd AGG-UNWRAP AGG-SCALAR  i 1+ EXIT THEN
   k 3 = IF  et c o nd CELL+ @ 0 -1 AGG-LIST DROP  i 1+ EXIT THEN
   k 1 = c 0< 0= AND et T-BYTE? AND IF  c o nd AGG-STRFILL  i 1+ EXIT THEN
   et c o v i 0 AGG-LIST ;
' (AGG-FILL) IS AGG-FILL
\ a global's whole brace list, at OFF
: AGG-TOP ( ety cnt off node -- )  CELL+ @ 0 -1 AGG-LIST DROP ;

: DECL {: | a u ct dt t n sz ag sc at y ivn off es fa fu fr fs fx fk bq q ce fn ai sa su aet acn gy -- :}
   STORAGE TO sc
   TYPE? 0= IF ." c4fc: a declaration was expected" CR ABORT THEN
   BASE-TYPE TO ct   BQ @ TO bq
   ATTRS TO at
   TK Semi = IF TNEXT EXIT THEN         \ a struct definition and nothing more
   BEGIN
      bq DQ !  ct STARS TO dt   DQ @ TO q
      FNPTR? IF
         FNPTR-DECL TO fn TO u TO a   t_int TO dt   0 TO q
         a 0= IF ." c4fc: bad global declaration" CR ABORT THEN
      ELSE
         TV TO a  TL TO u  Id WANT   -2 TO fn
         ST-MANGLE @ 0= IF
            TK Lparen = IF a u dt sc 0 MAX at q FUNCTION EXIT THEN
         THEN
      THEN
      ST-MANGLE @ IF
         TK Lparen = IF ." c4fc: a static local must be a variable" CR ABORT THEN
         a u MANGLE TO u TO a
      THEN
      dt TO t   0 TO ag   1 CELLS TO sz   -1 TO n   0 TO ivn   dt TO es   0 TO ce
      fn -2 = IF TK Brak = ELSE fn 0< 0= THEN IF
         fn -2 = IF
            TNEXT
            TK Rbrak = IF 0 TO n ELSE CONST-EXPR TO n THEN
            Rbrak WANT
         ELSE fn TO n THEN
         dt 2 + TO t   1 TO ag   q 2 LSHIFT TO q
      ELSE
         dt T-STRUCT? IF dt T-SIZE TO sz  1 TO ag THEN
      THEN
      \ the initialiser, if there is one
      -1 TO fr   -1 TO fs   0 TO ai   0 TO gy
      TK Assign = IF
         TNEXT
         ag TK Lbrace = AND IF INIT-BRACES TO ai ELSE   \ an array's or a struct's
         TK And = IF                              \ int *fp = &fn;  int *p = &x;
            TNEXT  TV TO fa  TL TO fu  Id WANT
            fa fu INIT-ADDR TO gy TO fk TO fr
            0 IVAL !  1 TO ivn
         ELSE
         TK Str = IF
            STR-LIT TO su TO sa                   \ "a" "b" is one string
            ag IF
               su 1+ TO ivn                       \ the nul counts
               su 0 ?DO sa I + C@ IVAL I CELLS + ! LOOP
               0 IVAL ivn 1- CELLS + !
            ELSE
               \ char *s = "..." is a POINTER: the bytes go to region 2
               \ and the global's own word is a data-to-data patch.
               sa su ID-STR, TO fs   0 IVAL !  1 TO ivn
            THEN
         ELSE INIT-NAME? IF                       \ int (*fp)(int) = twice;
            TV TO fa  TL TO fu  TNEXT
            fa fu INIT-ADDR TO gy TO fk TO fr
            0 IVAL !  1 TO ivn
         ELSE TK Lbrace = IF
            TNEXT
            BEGIN TK Rbrace <> WHILE
               CONST-EXPR IVAL ivn CELLS + !  ivn 1+ TO ivn
               TK Comma = IF TNEXT THEN
            REPEAT
            Rbrace WANT
         ELSE
            CONST-EXPR IVAL !  1 TO ivn  1 TO ce
         THEN THEN THEN THEN THEN
         ai IF
            \ M7b: the strings the pointers take first, then the storage,
            \ then every value at its offset (c4lc-gen's g:globalaggr)
            t dt <> IF  n 1 < IF ai AGG-COUNT TO n THEN  es TO aet  n TO acn  n es T-SIZE * TO sz
            ELSE dt TO aet  -1 TO acn  dt T-SIZE TO sz THEN
            0 AMODE !  0 SQN !   aet acn 0 ai AGG-TOP
            sz ID-ALLOT TO off
            1 AMODE !  0 SQI !   aet acn off ai AGG-TOP
            0 TO ivn
         ELSE
            ag IF
               n 1 < IF ivn TO n THEN
               n es T-SIZE * TO sz
            THEN
            sz ID-ALLOT TO off
         THEN
         \ a scalar is a whole word, as c4lc writes it: a negative char
         \ or short is sign-extended through all of it
         off  ag IF es ELSE t_int THEN  ivn INIT-WRITE
         fr 0< 0= IF off fr FNPAT, THEN
         fr -3 = IF off fk SYMPAT, THEN             \ an extern function (-c)
         fs 0< 0= IF off fs STRPAT, THEN
         gy IF off gy GLOBAL-PAT THEN               \ a global's address
         \ `extern T x[];` or `int x;` earlier, and this initialises it
         \ (M7b): the code already emitted names the old record's number,
         \ so that number now means this storage; the record retires, and
         \ the definition's symbol lands here, where c4lc puts it. They
         \ used to be two objects, and the code before the definition
         \ read the wrong one.
         a u ST-FIND TO y
         y IF y y.class @ c_glo = y y.ini @ 0= AND IF
            off  y y.val @ CELLS GOFF + !   c_gone y y.class !   0 y y.nlen !
         THEN THEN
         \ an aggregate records its byte size whether or not it was
         \ initialised, because sizeof(name) is answered from it
         a u c_glo off t ag  ag IF sz ELSE 0 THEN  ST,   \ region 1: final
         STN @ 1- ST[] TO y   1 y y.ini !  sc 0 MAX y y.sc !   q y y.q !
         \ a static const scalar with a constant initialiser is a
         \ constant from here on (CPRIMARY); it keeps its storage
         sc 8 = q 1 AND 0<> AND ag 0= AND ce AND dt T-ARITH? AND IF
            1 y y.cv !  IVAL @ dt CONVVAL y y.cval !  dt T-PROMOTE y y.cty !
         THEN
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
               STN @ 1- ST[] TO y   0 y y.sc !   q y y.q !
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
               STN @ 1- ST[] TO y   sc 0 MAX y y.sc !   q y y.q !
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
         y  I CELLS FN-RQ + @  I CELLS FN-PT + @  I CELLS FN-PQ + @  SIG!
      THEN
   LOOP ;

\ Where the three data regions actually land. Region 1 was placed as it
\ was declared; regions 2 and 3 are relative until now, so every patch
\ that names one gets shifted here.
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

' DECL IS DECL-D

: PROGRAM
   BEGIN TK Eof <> WHILE DECL REPEAT
   EMIT-HELPERS                         \ M4: after the last function, as c4lc
   FIX-FORWARDS
   PLACE-GLOBALS
   DB2 @ EMIT-TABPATS ;
