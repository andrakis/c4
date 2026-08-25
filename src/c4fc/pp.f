\ c4fc pp.f -- the C preprocessor (c4lc L9).
\
\ It works on TOKENS, not text, which is the only way macro expansion is
\ correct: lex.f in PPMODE hands back '#' and '##' as tokens, splices
\ backslash-newlines, and returns a <header> name as one Str token. A
\ directive runs from a Hash to the end of its LINE, which is why the
\ splicing matters -- and why every token carries the serial of the
\ buffer it came from as well as its line, since after an #include two
\ files' line numbers sit next to each other on one stream.
\
\ THE STREAM IS A STACK. c4lc splices an expansion onto the front of a
\ cons list and walks the result; here the pending tokens live in a
\ vector used as a stack, top first, and "push back so it is rescanned"
\ is one V,. #include is the same operation with a whole file's tokens.
\
\ Three tables and a walk, as everywhere else in c4fc: DIRECTIVE rows,
\ BINOP rows for the #if grammar, and SPELL rows for the one job that
\ needs a token's text back (# and ##). Adding #warning is one row.
\
\ Deliberate divergences from c4lc's preprocessor, all in the direction
\ of what C says, all documented in docs/c4fc-design.md:
\   * a conditional level records whether a branch has been TAKEN, so
\     #if 1 / #elif 1 / #else does not run the #else arm;
\   * `defined(X)` protects X from expansion even when X is a macro;
\   * a macro is painted blue while its own expansion is rescanned, so
\     a self-referential #define terminates instead of looping;
\   * a function-like macro name without a following '(' stands for
\     itself rather than being an error.

\ -- small helpers ------------------------------------------------------

: PP-STR ( a u -- a' u )  {: a u | d -- a' u :}
   u 1+ ALLOT: TO d  a d u MOVE  d u ;
: V-CLEAR ( v -- )  0 SWAP v.len ! ;
: V-TAIL ( v i -- v' ) {: v i | r -- r :}     \ a fresh vector of v[i..]
   16 NEW-VEC TO r
   v V# i ?DO I v V@ r V, LOOP
   r ;
: V-NOEOF ( v -- v )                          \ drop a trailing Eof
   DUP V# 0> IF
      DUP DUP V# 1- SWAP V@ t.kind @ Eof = IF DUP DUP V# 1- SWAP v.len ! THEN
   THEN ;
: TOK-COPY ( t -- t' ) {: t | n -- n :}
   TOK ALLOT: TO n  t n TOK MOVE  n ;

CREATE NB2 24 ALLOT   VARIABLE NB2P
: N>TEXT ( n -- a u )
   DUP 0< >R  R@ IF NEGATE THEN
   NB2 24 + NB2P !
   BEGIN -1 NB2P +!  DUP 10 MOD 48 + NB2P @ C!  10 /  DUP 0= UNTIL DROP
   R> IF -1 NB2P +! 45 NB2P @ C! THEN
   NB2P @  NB2 24 + OVER - ;

\ -- include search paths -----------------------------------------------

32 CONSTANT PATH-MAX
CREATE PPATHA PATH-MAX CELLS ALLOT
CREATE PPATHU PATH-MAX CELLS ALLOT
VARIABLE #PATHS   0 #PATHS !
: PP-PATH ( a u -- )  {: a u -- :}          \ one -I directory
   #PATHS @ PATH-MAX >= IF ." c4fc: too many -I paths" CR ABORT THEN
   a u PP-STR  #PATHS @ CELLS PPATHU + !  #PATHS @ CELLS PPATHA + !
   1 #PATHS +! ;

\ -- the macro table ----------------------------------------------------
\ m.params is 0 for an object-like macro and a vector of parameter name
\ tokens otherwise -- an EMPTY vector is `#define f() ...`, which is not
\ the same thing and expanded f() to the body with a stray "()" behind
\ it the day the two were conflated.

BEGIN-STRUCTURE MAC
   FIELD: m.name  FIELD: m.nlen  FIELD: m.hash
   FIELD: m.params  FIELD: m.body  FIELD: m.busy
END-STRUCTURE

CREATE MACROS VEC ALLOT
: M-HASH ( a u -- h )  DUP 8 LSHIFT SWAP 0> IF SWAP C@ + ELSE SWAP DROP THEN ;
: MAC-FIND ( a u -- mac|0 ) {: a u | h m -- r :}
   a u M-HASH TO h
   MACROS V# 0 ?DO
      I MACROS V@ TO m
      m m.hash @ h = IF
         a u  m m.name @ m m.nlen @ BYTES2= IF m UNLOOP EXIT THEN
      THEN
   LOOP 0 ;
: MAC-UNDEF ( a u -- ) {: a u | m j -- :}
   0 TO j
   MACROS V# 0 ?DO
      I MACROS V@ TO m
      a u m m.name @ m m.nlen @ BYTES2= 0= IF m j MACROS V!  j 1+ TO j THEN
   LOOP
   j MACROS v.len ! ;
: MAC-DEF ( a u params body -- ) {: a u p b | m -- :}
   a u MAC-UNDEF
   MAC ALLOT: TO m
   a u PP-STR m m.nlen ! m m.name !
   a u M-HASH m m.hash !
   p m m.params !  b m m.body !  0 m m.busy !
   m MACROS V, ;
: MAC-DEFINED? ( a u -- f )  MAC-FIND 0<> ;

\ -- the token stream ---------------------------------------------------

VARIABLE PPIN                            \ pending tokens; top = next
VARIABLE PPOUT                           \ where survivors go
: PP#     ( -- n )  PPIN @ V# ;
: PP-TOP  ( -- t )  PPIN @ DUP V# 1- SWAP V@ ;
: PP-DROP ( -- )    PPIN @ DUP V# 1- SWAP v.len ! ;
: PP-POP  ( -- t )  PP-TOP PP-DROP ;
: PP-PUSH ( t -- )  PPIN @ V, ;
: PP-EMIT ( t -- )  PPOUT @ V, ;
: PP-PUSHV ( v -- ) {: v -- :}           \ reversed, so v[0] pops first
   v V# 0 ?DO  v V# 1- I -  v V@ PP-PUSH  LOOP ;

\ An EndMac marker carries the macro it closes: when it surfaces, that
\ macro's own expansion has been fully rescanned and it may expand again.
: MARK ( m -- t ) {: m | t -- t :}
   TOK ALLOT: TO t  EndMac t t.kind !  m t t.val !
   0 t t.len !  0 t t.line !  0 t t.adj !  0 t t.file !  t ;
: PP-UNMARK ( t -- )  t.val @ ?DUP IF 0 SWAP m.busy ! THEN ;
: PP-SETTLE ( -- )                       \ retire any markers on top
   BEGIN PP# 0> IF PP-TOP t.kind @ EndMac = ELSE 0 THEN WHILE
      PP-POP PP-UNMARK
   REPEAT ;

\ -- spelling a token back into text ------------------------------------
\ Needed by # and ## and by nothing else.  SPELL <kind> <text>.

CREATE SPBUF 1024 ALLOT   VARIABLE SPB   0 SPB !
CREATE SPOFF KIND-MAX CELLS ALLOT
CREATE SPLEN KIND-MAX CELLS ALLOT
: SPELL-ZERO KIND-MAX 0 ?DO 0 I CELLS SPLEN + ! LOOP ; SPELL-ZERO
: SPELL ( "kind" "text" -- ) {: | k a u -- :}
   BL WORD FIND 0= IF ." pp: SPELL: unknown kind" CR ABORT THEN
   EXECUTE TO k
   BL WORD COUNT TO u TO a
   SPB @ k CELLS SPOFF + !   u k CELLS SPLEN + !
   a SPBUF SPB @ + u MOVE   u SPB +! ;

SPELL Lparen (   SPELL Rparen )   SPELL Lbrace {   SPELL Rbrace }
SPELL Brak [     SPELL Rbrak ]    SPELL Semi ;     SPELL Comma ,
SPELL Dot .      SPELL Arrow ->   SPELL Assign =   SPELL Eq ==
SPELL Ne !=      SPELL Lt <       SPELL Gt >       SPELL Le <=
SPELL Ge >=      SPELL Add +      SPELL Sub -      SPELL Mul *
SPELL Div /      SPELL Mod %      SPELL And &      SPELL Or |
SPELL Xor ^      SPELL Not !      SPELL Tilde ~    SPELL Lan &&
SPELL Lor ||     SPELL Shl <<     SPELL Shr >>     SPELL Inc ++
SPELL Dec --     SPELL Cond ?     SPELL Colon :    SPELL Hash #
SPELL HashHash ##
SPELL AddA +=    SPELL SubA -=    SPELL MulA *=    SPELL DivA /=
SPELL ModA %=    SPELL ShlA <<=   SPELL ShrA >>=   SPELL AndA &=
SPELL OrA |=     SPELL XorA ^=
SPELL Int int    SPELL Char char  SPELL If if      SPELL Else else
SPELL While while SPELL For for   SPELL Return return
SPELL Sizeof sizeof   SPELL Struct struct   SPELL Union union
SPELL Enum enum  SPELL Static static  SPELL Extern extern
SPELL Typedef typedef SPELL Break break    SPELL Continue continue
SPELL Switch switch   SPELL Case case      SPELL Default default
SPELL Do do      SPELL Attribute __attribute__
SPELL Constructor constructor  SPELL Destructor destructor

: PP-SPELL ( t -- a u ) {: t | k -- a u :}
   t t.kind @ TO k
   k Id  = IF t t.val @ t t.len @ EXIT THEN
   k Str = IF t t.val @ t t.len @ EXIT THEN
   k Num = IF t t.val @ N>TEXT EXIT THEN
   k CELLS SPOFF + @ SPBUF +  k CELLS SPLEN + @ ;

CREATE SPL 8192 ALLOT   VARIABLE SPLN
: SPL-RESET ( -- )  0 SPLN ! ;
: SPL+ ( a u -- )
   DUP SPLN @ + 8192 > IF ." c4fc: stringize buffer full" CR ABORT THEN
   DUP >R  SPL SPLN @ +  SWAP MOVE  R> SPLN +! ;
\ Space-separated, which is what cpp does for the shapes that occur in
\ a macro argument; its full whitespace rule is subtler and unused here.
: PP-SPELL-LIST ( v -- a u ) {: v -- a u :}
   SPL-RESET
   v V# 0 ?DO  I 0> IF S"  " SPL+ THEN  I v V@ PP-SPELL SPL+  LOOP
   SPL SPLN @ PP-STR ;
: STR-TOK ( a u t -- t' ) {: a u t | n -- n :}   \ a Str token at t's place
   TOK ALLOT: TO n
   Str n t.kind !  a n t.val !  u n t.len !
   t t.line @ n t.line !  0 n t.adj !  t t.file @ n t.file !  n ;
\ a ## b: splice the spellings and lex the result, which is how a paste
\ can make an identifier, a number or an operator without pp knowing
\ which it made.
: PP-PASTE ( ta tb -- v )
   SPL-RESET  SWAP PP-SPELL SPL+  PP-SPELL SPL+
   SPL SPLN @ LEX-STR>V V-NOEOF ;

\ -- the #if grammar ----------------------------------------------------
\   <prec> BINOP <kind> <word>       one row per binary operator

: EBOOL ( f -- n )  1 AND ;
: E<  <  EBOOL ;   : E>  >  EBOOL ;   : E<= <= EBOOL ;   : E>= >= EBOOL ;
: E=  =  EBOOL ;   : E<> <> EBOOL ;
: EAND ( a b -- n )  0<> SWAP 0<> AND EBOOL ;
: EOR  ( a b -- n )  0<> SWAP 0<> OR  EBOOL ;
\ Division by zero yields 0 rather than trapping: it is reachable only
\ in an arm the && to its left has already decided is dead, and cpp is
\ not required to evaluate that arm at all.
: E/ ( a b -- n )  DUP 0= IF 2DROP 0 EXIT THEN / ;
: E% ( a b -- n )  DUP 0= IF 2DROP 0 EXIT THEN MOD ;
: E>> ( a b -- n )  0 MAX 0 ?DO 2/ LOOP ;
: E<< ( a b -- n )  0 MAX LSHIFT ;

64 CONSTANT BOP-MAX
CREATE BOPK BOP-MAX CELLS ALLOT
CREATE BOPP BOP-MAX CELLS ALLOT
CREATE BOPX BOP-MAX CELLS ALLOT
VARIABLE #BOP   0 #BOP !
: BINOP ( prec "kind" "word" -- ) {: p | k x -- :}
   BL WORD FIND 0= IF ." pp: BINOP: unknown kind" CR ABORT THEN
   EXECUTE TO k
   BL WORD FIND 0= IF ." pp: BINOP: unknown word" CR ABORT THEN
   TO x
   k #BOP @ CELLS BOPK + !  p #BOP @ CELLS BOPP + !
   x #BOP @ CELLS BOPX + !  1 #BOP +! ;

 1 BINOP Lor EOR
 2 BINOP Lan EAND
 3 BINOP Or  OR
 4 BINOP Xor XOR
 5 BINOP And AND
 6 BINOP Eq  E=      6 BINOP Ne E<>
 7 BINOP Lt  E<      7 BINOP Gt E>     7 BINOP Le E<=   7 BINOP Ge E>=
 8 BINOP Shl E<<     8 BINOP Shr E>>
 9 BINOP Add +       9 BINOP Sub -
10 BINOP Mul *      10 BINOP Div E/   10 BINOP Mod E%

: BOP-PREC ( kind -- prec|0 ) {: k -- p :}
   #BOP @ 0 ?DO k I CELLS BOPK + @ = IF I CELLS BOPP + @ UNLOOP EXIT THEN LOOP 0 ;
: BOP-XT ( kind -- xt ) {: k -- x :}
   #BOP @ 0 ?DO k I CELLS BOPK + @ = IF I CELLS BOPX + @ UNLOOP EXIT THEN LOOP
   ." pp: no such operator" CR ABORT ;

VARIABLE ETOKS   VARIABLE EPOS
: E-KIND ( -- k )  EPOS @ ETOKS @ V# < IF EPOS @ ETOKS @ V@ t.kind @ ELSE Eof THEN ;
: E-TOK  ( -- t )  EPOS @ ETOKS @ V@  1 EPOS +! ;
: E-EAT  ( k -- )  E-KIND = IF 1 EPOS +! THEN ;

DEFER E-EXPR
: E-DEFINED ( -- v ) {: | paren t -- v :}   \ defined X / defined(X)
   E-KIND Lparen = TO paren
   paren IF 1 EPOS +! THEN
   E-TOK TO t
   paren IF Rparen E-EAT THEN
   t t.kind @ Id = IF t t.val @ t t.len @ MAC-DEFINED? EBOOL EXIT THEN
   0 ;
: E-PRIMARY ( -- v ) {: | k t v -- v :}
   E-KIND TO k
   k Num = IF E-TOK t.val @ EXIT THEN
   k Lparen = IF 1 EPOS +! E-EXPR TO v  Rparen E-EAT  v EXIT THEN
   k Id = IF
      E-TOK TO t
      t t.val @ t t.len @ S" defined" BYTES2= IF E-DEFINED EXIT THEN
      0 EXIT THEN                          \ a surviving name is 0, as C says
   k Eof <> IF 1 EPOS +! THEN
   0 ;
: E-UNARY ( -- v ) {: | k -- v :}
   E-KIND TO k
   k Not   = IF 1 EPOS +! RECURSE 0= EBOOL EXIT THEN
   k Tilde = IF 1 EPOS +! RECURSE INVERT EXIT THEN
   k Sub   = IF 1 EPOS +! RECURSE NEGATE EXIT THEN
   k Add   = IF 1 EPOS +! RECURSE EXIT THEN
   E-PRIMARY ;
: E-BIN ( minprec -- v ) {: mp | v k p r -- v :}
   E-UNARY TO v
   BEGIN
      E-KIND TO k   k BOP-PREC TO p
      p 0<> p mp >= AND
   WHILE
      1 EPOS +!
      p 1+ RECURSE TO r
      v r  k BOP-XT EXECUTE TO v
   REPEAT
   v ;
: E-COND ( -- v ) {: | c a b -- v :}
   1 E-BIN TO c
   E-KIND Cond = IF
      1 EPOS +!  RECURSE TO a  Colon E-EAT  RECURSE TO b
      c 0<> IF a ELSE b THEN EXIT THEN
   c ;
' E-COND IS E-EXPR

\ -- macro expansion ----------------------------------------------------

DEFER PP-APPLY   ( t mac -- )

: PP-ARGFOR ( a u params args -- v|0 ) {: a u ps as | p -- r :}
   ps 0= IF 0 EXIT THEN
   ps V# 0 ?DO
      I ps V@ TO p
      a u  p t.val @ p t.len @ BYTES2= IF
         I as V# < IF I as V@ ELSE 16 NEW-VEC THEN UNLOOP EXIT
      THEN
   LOOP 0 ;

\ Expand every macro in a token list. Used for #if lines and for macro
\ arguments, neither of which has a surrounding stream to push back onto,
\ so it runs the same walk over a stream of its own.
: EXPANDV ( v -- v' ) {: v | si so t k m r -- r :}
   PPIN @ TO si   PPOUT @ TO so
   1024 NEW-VEC PPIN !   16 NEW-VEC DUP TO r PPOUT !
   v PP-PUSHV
   BEGIN PP# 0> WHILE
      PP-POP TO t   t t.kind @ TO k
      k EndMac = IF t PP-UNMARK ELSE
      k Id = IF
         t t.val @ t t.len @ S" defined" BYTES2= IF
            \ `defined X` and `defined(X)`: the operand is NOT expanded
            t PP-EMIT
            PP-SETTLE
            PP# 0> IF
               PP-TOP t.kind @ Lparen = IF
                  PP-POP PP-EMIT  PP-SETTLE
                  PP# 0> IF PP-POP PP-EMIT THEN  PP-SETTLE
                  PP# 0> IF PP-TOP t.kind @ Rparen = IF PP-POP PP-EMIT THEN THEN
               ELSE PP-POP PP-EMIT THEN
            THEN
         ELSE
            t t.val @ t t.len @ MAC-FIND TO m
            m IF m m.busy @ IF 0 TO m THEN THEN
            m IF t m PP-APPLY ELSE t PP-EMIT THEN
         THEN
      ELSE t PP-EMIT THEN THEN
   REPEAT
   si PPIN !   so PPOUT !
   r ;

: EXPAND-ARGS ( args -- args' ) {: as | r -- r :}
   16 NEW-VEC TO r
   as V# 0 ?DO I as V@ EXPANDV r V, LOOP
   r ;

\ Collect comma-separated arguments up to the matching ')'. The '(' has
\ already been taken off the stream.
: PP-ARGS ( -- args ) {: | args cur d t k -- args :}
   16 NEW-VEC TO args   16 NEW-VEC TO cur   0 TO d
   BEGIN
      PP-SETTLE
      PP# 0= IF ." c4fc: unterminated macro arguments" CR ABORT THEN
      PP-POP TO t   t t.kind @ TO k
      k Eof = IF ." c4fc: unterminated macro arguments" CR ABORT THEN
      k Rparen = d 0= AND IF cur args V,  args EXIT THEN
      k Comma = d 0= AND IF cur args V,  16 NEW-VEC TO cur
      ELSE
         k Lparen = IF d 1+ TO d THEN
         k Rparen = IF d 1- TO d THEN
         t cur V,
      THEN
   AGAIN ;

: FIRST-OF ( v t -- t' )  OVER 0= IF NIP EXIT THEN  OVER V# 0= IF NIP EXIT THEN
   DROP 0 SWAP V@ ;
: LAST-OF  ( v t -- t' )  OVER 0= IF NIP EXIT THEN  OVER V# 0= IF NIP EXIT THEN
   DROP DUP V# 1- SWAP V@ ;
: PARAM-OF ( t params args -- v|0 ) {: t ps as -- r :}
   t t.kind @ Id <> IF 0 EXIT THEN
   t t.val @ t t.len @ ps as PP-ARGFOR ;

\ Replace parameters in BODY. An ordinary parameter takes the EXPANDED
\ argument; an operand of # or ## takes the raw one, which is the whole
\ reason STR(V) gives "V" while XSTR(V) gives V's value.
: PP-SUBST ( body params args eargs -- v )
   {: body ps as eas | r i t nx sa lt rt pv -- r :}
   16 NEW-VEC TO r   0 TO i
   BEGIN i body V# < WHILE
      i body V@ TO t
      t t.kind @ Hash = i 1+ body V# < AND IF
         i 1+ body V@ TO nx
         nx ps as PARAM-OF TO sa
         sa IF
            sa PP-SPELL-LIST t STR-TOK r V,   i 2 + TO i
         ELSE t r V,  i 1+ TO i THEN
      ELSE
         i 1+ body V# < IF i 1+ body V@ t.kind @ HashHash = ELSE 0 THEN
         IF
            i 2 + body V# < IF
               t ps as PARAM-OF t LAST-OF TO lt
               i 2 + body V@  DUP ps as PARAM-OF SWAP FIRST-OF TO rt
               lt rt PP-PASTE TO pv  pv V# 0 ?DO I pv V@ r V, LOOP
               i 3 + TO i
            ELSE t r V,  i 1+ TO i THEN
         ELSE
            t ps eas PARAM-OF TO pv
            pv IF pv V# 0 ?DO I pv V@ r V, LOOP ELSE t r V, THEN
            i 1+ TO i
         THEN
      THEN
   REPEAT
   r ;

\ Expanded tokens carry the invocation's line and file, so the directive
\ splitting that runs on line numbers stays sane.
: RELINE ( v t -- v' ) {: v t | r c -- r :}
   16 NEW-VEC TO r
   v V# 0 ?DO
      I v V@ TOK-COPY TO c
      t t.line @ c t.line !   t t.file @ c t.file !
      c r V,
   LOOP r ;

: (APPLY) ( t mac -- ) {: t m | args eargs ex -- :}
   m m.params @ 0= IF
      m m.body @ t RELINE TO ex
      m MARK PP-PUSH   ex PP-PUSHV   1 m m.busy !  EXIT THEN
   PP-SETTLE
   PP# 0= IF t PP-EMIT EXIT THEN
   PP-TOP t.kind @ Lparen <> IF t PP-EMIT EXIT THEN
   PP-DROP
   PP-ARGS TO args
   args EXPAND-ARGS TO eargs
   m m.body @ m m.params @ args eargs PP-SUBST  t RELINE TO ex
   m MARK PP-PUSH   ex PP-PUSHV   1 m m.busy ! ;
' (APPLY) IS PP-APPLY

\ -- the conditional stack ----------------------------------------------
\ Three cells a level: DEAD (an enclosing branch is already dead), TAKEN
\ (some arm of this #if has run) and ACTIVE (this arm is the one).

128 CONSTANT COND-MAX
CREATE CSTK COND-MAX 3 * CELLS ALLOT
VARIABLE CDEPTH   0 CDEPTH !
: C-TOP ( -- a )  CDEPTH @ 1- 3 * CELLS CSTK + ;
: C-PUSH ( dead taken active -- )
   CDEPTH @ COND-MAX >= IF ." c4fc: #if nested too deeply" CR ABORT THEN
   1 CDEPTH +!
   C-TOP 2 CELLS + !   C-TOP CELL+ !   C-TOP ! ;
: C-DEAD?   ( -- f )  C-TOP @ ;
: C-TAKEN?  ( -- f )  C-TOP CELL+ @ ;
: C-ACTIVE! ( f -- )  C-TOP 2 CELLS + ! ;
: C-TAKEN!  ( f -- )  C-TOP CELL+ ! ;
: PP-SKIPPING? ( -- f )
   CDEPTH @ 0= IF 0 EXIT THEN
   C-DEAD? IF -1 EXIT THEN
   C-TOP 2 CELLS + @ 0= ;

\ -- directives ---------------------------------------------------------
\   DIRECTIVE  <name> <word>     runs only in live text
\   CDIRECTIVE <name> <word>     runs even inside a dead branch
\ Each handler takes the directive's body -- the tokens after its name.

32 CONSTANT DIR-MAX
CREATE DNBUF 512 ALLOT   VARIABLE DNB   0 DNB !
CREATE DOFF DIR-MAX CELLS ALLOT
CREATE DLEN DIR-MAX CELLS ALLOT
CREATE DXT  DIR-MAX CELLS ALLOT
CREATE DCND DIR-MAX CELLS ALLOT
VARIABLE #DIR   0 #DIR !
: (DIR) ( cond "name" "word" -- ) {: c | a u x -- :}
   #DIR @ DIR-MAX >= IF ." pp: too many directives" CR ABORT THEN
   BL WORD COUNT TO u TO a
   DNB @ #DIR @ CELLS DOFF + !   u #DIR @ CELLS DLEN + !
   a DNBUF DNB @ + u MOVE  u DNB +!
   BL WORD FIND 0= IF ." pp: DIRECTIVE: unknown word" CR ABORT THEN
   TO x
   x #DIR @ CELLS DXT + !   c #DIR @ CELLS DCND + !
   1 #DIR +! ;
: DIRECTIVE   ( "name" "word" -- )  0 (DIR) ;
: CDIRECTIVE  ( "name" "word" -- ) -1 (DIR) ;
: DIR-FIND ( a u -- i|-1 ) {: a u -- i :}
   #DIR @ 0 ?DO
      a u  I CELLS DOFF + @ DNBUF +  I CELLS DLEN + @  BYTES2=
      IF I UNLOOP EXIT THEN
   LOOP -1 ;

: PP-EVALIF ( v -- f )
   EXPANDV ETOKS !  0 EPOS !  E-EXPR 0<> ;
: D-NOP ( v -- )  DROP ;
: D-ERROR ( v -- )  DROP ." c4fc: #error" CR ABORT ;

: D-IFDEF ( v -- ) {: v | c -- :}
   PP-SKIPPING? IF -1 -1 0 C-PUSH v DROP EXIT THEN
   v V# 0= IF ." c4fc: #ifdef needs a name" CR ABORT THEN
   0 v V@ DUP t.val @ SWAP t.len @ MAC-DEFINED? TO c
   0 c c C-PUSH ;
: D-IFNDEF ( v -- ) {: v | c -- :}
   PP-SKIPPING? IF -1 -1 0 C-PUSH v DROP EXIT THEN
   v V# 0= IF ." c4fc: #ifndef needs a name" CR ABORT THEN
   0 v V@ DUP t.val @ SWAP t.len @ MAC-DEFINED? 0= TO c
   0 c c C-PUSH ;
: D-IF ( v -- ) {: v | c -- :}
   PP-SKIPPING? IF -1 -1 0 C-PUSH v DROP EXIT THEN
   v PP-EVALIF TO c
   0 c c C-PUSH ;
: D-ELIF ( v -- ) {: v | c -- :}
   CDEPTH @ 0= IF ." c4fc: #elif without #if" CR ABORT THEN
   C-DEAD? IF v DROP EXIT THEN
   C-TAKEN? IF 0 C-ACTIVE! v DROP EXIT THEN
   v PP-EVALIF TO c
   c C-ACTIVE!  c C-TAKEN! ;
: D-ELSE ( v -- )
   DROP
   CDEPTH @ 0= IF ." c4fc: #else without #if" CR ABORT THEN
   C-DEAD? IF EXIT THEN
   C-TAKEN? 0= C-ACTIVE!  -1 C-TAKEN! ;
: D-ENDIF ( v -- )
   DROP
   CDEPTH @ 0= IF ." c4fc: #endif without #if" CR ABORT THEN
   -1 CDEPTH +! ;

\ Function-like only when the '(' TOUCHED the name: "#define A (x)"
\ defines A as the token sequence "(x)".
: PP-PARAMS ( v i -- params i' ) {: v i | ps t k -- ps i :}
   16 NEW-VEC TO ps
   BEGIN i v V# < WHILE
      i v V@ TO t   t t.kind @ TO k   i 1+ TO i
      k Rparen = IF ps i EXIT THEN
      k Comma = 0= IF
         k Id <> IF ." c4fc: bad macro parameter" CR ABORT THEN
         t ps V, THEN
   REPEAT
   ." c4fc: unterminated macro parameter list" CR ABORT ;
: D-DEFINE ( v -- ) {: v | n ps i -- :}
   v V# 0= IF ." c4fc: #define needs a name" CR ABORT THEN
   0 v V@ TO n
   n t.kind @ Id <> IF ." c4fc: #define needs a name" CR ABORT THEN
   n t.adj @ IF
      v 2 PP-PARAMS TO i TO ps
      n t.val @ n t.len @ ps  v i V-TAIL MAC-DEF
   ELSE
      n t.val @ n t.len @ 0  v 1 V-TAIL MAC-DEF
   THEN ;
: D-UNDEF ( v -- ) {: v | n -- :}
   v V# 0= IF EXIT THEN
   0 v V@ TO n  n t.val @ n t.len @ MAC-UNDEF ;

\ Where a file's own directory is remembered, so that "quoted" includes
\ resolve against it -- which is what C says and what gcc does, and is
\ not what a search of the CURRENT directory does. The two agree for
\ every module in this tree that is compiled from the root, and disagree
\ the moment two directories both hold a c4.h, which this one does.
1024 CONSTANT FDIR-MAX
CREATE FDIRA FDIR-MAX CELLS ALLOT
CREATE FDIRU FDIR-MAX CELLS ALLOT
VARIABLE CURFILE   0 CURFILE !
: FDIR! ( a u serial -- ) {: a u s -- :}
   s FDIR-MAX < IF a s CELLS FDIRA + !  u s CELLS FDIRU + ! THEN ;
: FDIR@ ( serial -- a u ) {: s -- a u :}
   s FDIR-MAX < IF s CELLS FDIRA + @  s CELLS FDIRU + @ ELSE 0 0 THEN ;

VARIABLE #INCL   0 #INCL !
CREATE IPATH 1024 ALLOT
: JOIN-PATH ( dir du name nu -- len ) {: d du n nu -- len :}
   du nu + 1+ 1024 > IF ." c4fc: include path too long" CR ABORT THEN
   d IPATH du MOVE  n IPATH du + nu MOVE  0 IPATH du + nu + C!
   du nu + ;
: TRY-PATH ( dir du name nu -- len ) {: d du n nu -- len :}
   d IPATH du MOVE  47 IPATH du + C!
   n IPATH du + 1+ nu MOVE  0 IPATH du + 1+ nu + C!
   du 1+ nu + ;
: FILE-THERE? ( a u -- f ) {: a u | fd -- f :}
   a u ZPATH OPENF TO fd
   fd 0< IF 0 EXIT THEN  fd CLOSEF  -1 ;
\ ANGLE is the <...> form: the -I list only, never the including file's
\ own directory. The lexer marks it in t.adj, which a Str token has
\ spare.
: PP-RESOLVE ( a u angle -- a' u' ) {: a u ang | r -- a u :}
   u 0> IF a C@ 47 = IF a u FILE-THERE? IF a u EXIT THEN THEN THEN  \ absolute
   ang 0= IF
      CURFILE @ FDIR@ a u JOIN-PATH TO r
      IPATH r FILE-THERE? IF IPATH r EXIT THEN
   THEN
   #PATHS @ 0 ?DO
      I CELLS PPATHA + @  I CELLS PPATHU + @  a u TRY-PATH TO r
      IPATH r FILE-THERE? IF IPATH r UNLOOP EXIT THEN
   LOOP
   ." c4fc: cannot find include: " a u TYPE CR ABORT ;
: PP-LEX-INCLUDE ( a u -- v ) {: a u | v -- v :}
   a u PP-STR LEX-FILE>V TO v                 \ the path must outlive the read
   a u DIRNAME PP-STR LEXF @ FDIR!
   v ;
: D-INCLUDE ( v -- ) {: v | ex n -- :}
   1 #INCL +!
   #INCL @ 4096 > IF ." c4fc: #include runaway" CR ABORT THEN
   v EXPANDV TO ex
   ex V# 0= IF ." c4fc: #include needs a file" CR ABORT THEN
   0 ex V@ TO n
   n t.kind @ Str <> IF ." c4fc: #include needs a quoted name or a bracketed one" CR ABORT THEN
   n t.val @ n t.len @ n t.adj @ PP-RESOLVE PP-LEX-INCLUDE V-NOEOF PP-PUSHV ;

CDIRECTIVE ifdef   D-IFDEF
CDIRECTIVE ifndef  D-IFNDEF
CDIRECTIVE if      D-IF
CDIRECTIVE elif    D-ELIF
CDIRECTIVE else    D-ELSE
CDIRECTIVE endif   D-ENDIF
DIRECTIVE  define  D-DEFINE
DIRECTIVE  undef   D-UNDEF
DIRECTIVE  include D-INCLUDE
DIRECTIVE  pragma  D-NOP
DIRECTIVE  line    D-NOP
DIRECTIVE  warning D-NOP
DIRECTIVE  error   D-ERROR

\ -- the walk -----------------------------------------------------------

: TAKE-LINE ( line file -- v ) {: ln fl | v t -- v :}
   16 NEW-VEC TO v
   BEGIN
      PP-SETTLE
      PP# 0= IF v EXIT THEN
      PP-TOP TO t
      t t.kind @ Eof = IF v EXIT THEN
      t t.line @ ln <> IF v EXIT THEN
      t t.file @ fl <> IF v EXIT THEN
      PP-DROP  t v V,
   AGAIN ;

: PP-DIRECTIVE ( t -- ) {: t | v n a u i -- :}
   t t.line @ t t.file @ TAKE-LINE TO v
   v V# 0= IF EXIT THEN                   \ a bare '#' is a null directive
   0 v V@ TO n
   n t.kind @ Num = IF EXIT THEN          \ gcc's `# 12 "file"` line marker
   t t.file @ CURFILE !
   n PP-SPELL TO u TO a
   a u DIR-FIND TO i
   i 0< IF
      PP-SKIPPING? IF EXIT THEN
      ." c4fc: unknown directive: #" a u TYPE CR ABORT THEN
   i CELLS DCND + @ 0= PP-SKIPPING? AND IF EXIT THEN
   v 1 V-TAIL  i CELLS DXT + @ EXECUTE ;

: PP-GO ( -- ) {: | t k m -- :}
   BEGIN PP# 0> WHILE
      PP-POP TO t   t t.kind @ TO k
      k EndMac = IF t PP-UNMARK ELSE
      k Eof = IF
         CDEPTH @ IF ." c4fc: unterminated #if" CR ABORT THEN
         t PP-EMIT  EXIT
      ELSE
      k Hash = IF t PP-DIRECTIVE
      ELSE
      PP-SKIPPING? IF                     \ inside a dead branch
      ELSE
      k Id = IF
         t t.val @ t t.len @ MAC-FIND TO m
         m IF m m.busy @ IF 0 TO m THEN THEN
         m IF t m PP-APPLY ELSE t PP-EMIT THEN
      ELSE t PP-EMIT THEN
      THEN THEN THEN THEN
   REPEAT ;

\ -- entry points -------------------------------------------------------

: PP-RESET ( -- )
   MACROS 256 VEC-INIT  0 CDEPTH !  0 #INCL !  0 #PATHS !  0 SPLN ! ;

\ -D NAME   or   -D NAME=VALUE
: PP-DEFINE ( a u -- ) {: a u | e -- :}
   0 TO e
   BEGIN e u < IF a e + C@ 61 <> ELSE 0 THEN WHILE e 1+ TO e REPEAT
   e u >= IF
      a u 0  S" 1" LEX-STR>V V-NOEOF MAC-DEF
   ELSE
      a e 0  a e + 1+  u e - 1-  LEX-STR>V V-NOEOF MAC-DEF
   THEN ;

: PP-FILE ( a u -- )                      \ preprocess that file into TOKS
   1 PPMODE !
   TOKS 262144 VEC-INIT   TOKS PPOUT !
   65536 NEW-VEC PPIN !
   PP-LEX-INCLUDE PP-PUSHV
   PP-GO ;

\ The dump the differential compares: kind and value only. gcc -E and
\ c4fc's own preprocessor agree on the TOKENS and cannot agree on the
\ line numbers, because one of them emits `# 12 "file"` markers and the
\ other consumes them.
: DUMP-PPTOKENS ( -- ) {: | t -- :}
   TOKS V# 0 ?DO
      I TOKS V@ TO t
      OB-RESET
      t t.kind @ KNAME OB-T  32 OB-C
      t t.kind @ Num = IF t t.val @ OB-N ELSE
      t t.kind @ Id  = IF t t.val @ t t.len @ OB-T ELSE
      t t.kind @ Str = IF t t.val @ t t.len @ OB-T ELSE
      48 OB-C THEN THEN THEN
      OB-FLUSH
   LOOP ;
