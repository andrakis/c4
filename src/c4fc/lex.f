\ c4fc lex.f -- the C tokenizer (c4lc L0).
\
\ Mirrors c4lc-lex.lisp, which mirrors c4cc's next() byte for byte,
\ quirky escape table included. The oracle is c4lc's own golden dump:
\ src/c4sp/tests/expected/c4lc-tokens.txt, and the two must agree token
\ for token including line numbers.
\
\ Three tables and a loop. Adding a keyword is one KEYWORD row; adding
\ an operator is one OPER row; adding a token kind is one KIND: row --
\ and KIND: derives the printed name from the Forth name, so a kind
\ cannot be spelled one way and printed another.

\ -- token kinds --------------------------------------------------------
\ c4th's dictionary is case-sensitive, so Int, If and Do do not collide
\ with INT, IF and DO. The row is the token's name, and it prints as it
\ is written.

128 CONSTANT KIND-MAX
VARIABLE #KINDS   0 #KINDS !
CREATE KNBUF 2048 ALLOT   VARIABLE KNB   0 KNB !
CREATE KNOFF KIND-MAX CELLS ALLOT
CREATE KNLEN KIND-MAX CELLS ALLOT

: KIND: ( "name" -- )
   >IN @ >R                                  \ read the name, then put it back
   BL WORD COUNT
   DUP #KINDS @ CELLS KNLEN + !
   KNB @ #KINDS @ CELLS KNOFF + !
   DUP >R  KNBUF KNB @ + SWAP MOVE  R> KNB +!
   R> >IN !
   CREATE #KINDS @ , 1 #KINDS +!
   DOES> @ ;
: KNAME ( id -- a u )
   DUP CELLS KNOFF + @ KNBUF +  SWAP CELLS KNLEN + @ ;

KIND: Eof   KIND: Num   KIND: Id    KIND: Str
KIND: Hash  KIND: HashHash
\ EndMac is not a C token: it is the marker the preprocessor pushes
\ after a macro's expansion so it knows when the expansion has been
\ rescanned and the macro may be expanded again. It never reaches the
\ output, and pp.f is the only file that mentions it.
KIND: EndMac
\ keywords
KIND: Char  KIND: Else  KIND: Enum  KIND: If    KIND: Int
KIND: Return KIND: Sizeof KIND: While KIND: Switch KIND: Case
KIND: Default KIND: Break KIND: For  KIND: Continue
KIND: Static KIND: Extern KIND: Attribute
KIND: Constructor KIND: Destructor
KIND: Struct KIND: Union KIND: Typedef KIND: Do
\ operators and punctuation
KIND: Assign KIND: Eq   KIND: Ne    KIND: Lt   KIND: Gt
KIND: Le     KIND: Ge   KIND: Shl   KIND: Shr
KIND: Add    KIND: Sub  KIND: Mul   KIND: Div  KIND: Mod
KIND: Inc    KIND: Dec  KIND: Arrow KIND: Dot
KIND: And    KIND: Or   KIND: Xor   KIND: Tilde KIND: Not
KIND: Lan    KIND: Lor  KIND: Cond  KIND: Colon
KIND: AddA   KIND: SubA KIND: MulA  KIND: DivA KIND: ModA
KIND: ShlA   KIND: ShrA KIND: AndA  KIND: OrA  KIND: XorA
KIND: Semi   KIND: Comma KIND: Lparen KIND: Rparen
KIND: Lbrace KIND: Rbrace KIND: Brak KIND: Rbrak

\ -- the keyword table --------------------------------------------------
\   KEYWORD <text> <kind>       one row per C keyword

256 CONSTANT KW-MAX
VARIABLE #KW   0 #KW !
CREATE KWBUF 1024 ALLOT   VARIABLE KWB   0 KWB !
CREATE KWOFF KW-MAX CELLS ALLOT
CREATE KWLEN KW-MAX CELLS ALLOT
CREATE KWKIND KW-MAX CELLS ALLOT

: KEYWORD ( "text" "kind" -- )
   BL WORD COUNT
   DUP #KW @ CELLS KWLEN + !
   KWB @ #KW @ CELLS KWOFF + !
   DUP >R KWBUF KWB @ + SWAP MOVE R> KWB +!
   BL WORD FIND 0= IF ." lex: KEYWORD: unknown kind" CR ABORT THEN
   EXECUTE #KW @ CELLS KWKIND + !
   1 #KW +! ;

KEYWORD char    Char        KEYWORD else    Else
KEYWORD enum    Enum        KEYWORD if      If
KEYWORD int     Int         KEYWORD return  Return
KEYWORD sizeof  Sizeof      KEYWORD while   While
KEYWORD switch  Switch      KEYWORD case    Case
KEYWORD default Default     KEYWORD break   Break
KEYWORD for     For         KEYWORD continue Continue
KEYWORD static  Static      KEYWORD extern  Extern
KEYWORD __attribute__ Attribute
KEYWORD constructor Constructor
KEYWORD destructor  Destructor
KEYWORD struct  Struct      KEYWORD union   Union
KEYWORD typedef Typedef     KEYWORD do      Do
\ void IS char in c4, as in c4's own symbol seeding.
KEYWORD void    Char

: KW-FIND ( a u -- kind | -1 ) {: a u -- k :}
   #KW @ 0 ?DO
      u I CELLS KWLEN + @ = IF
         a  I CELLS KWOFF + @ KWBUF +  u  BYTES= IF
            I CELLS KWKIND + @ UNLOOP EXIT
         THEN
      THEN
   LOOP -1 ;

\ -- the operator table -------------------------------------------------
\   OPER <text> <kind>          one row per operator
\
\ Matched LONGEST first, computed rather than declared, so the rows can
\ be written in any order. Declaring them longest-first and scanning for
\ the first hit would work too, and would silently mislex >>= the day
\ somebody tidied the list.

128 CONSTANT OP-MAX
VARIABLE #OP   0 #OP !
CREATE OPBUF 512 ALLOT   VARIABLE OPB   0 OPB !
CREATE OPOFF OP-MAX CELLS ALLOT
CREATE OPLEN OP-MAX CELLS ALLOT
CREATE OPKIND OP-MAX CELLS ALLOT

: OPER ( "text" "kind" -- )
   BL WORD COUNT
   DUP #OP @ CELLS OPLEN + !
   OPB @ #OP @ CELLS OPOFF + !
   DUP >R OPBUF OPB @ + SWAP MOVE R> OPB +!
   BL WORD FIND 0= IF ." lex: OPER: unknown kind" CR ABORT THEN
   EXECUTE #OP @ CELLS OPKIND + !
   1 #OP +! ;

OPER <<= ShlA   OPER >>= ShrA
OPER ==  Eq     OPER !=  Ne     OPER <=  Le     OPER >=  Ge
OPER <<  Shl    OPER >>  Shr    OPER ++  Inc    OPER --  Dec
OPER ->  Arrow  OPER &&  Lan    OPER ||  Lor
OPER +=  AddA   OPER -=  SubA   OPER *=  MulA   OPER %=  ModA
OPER &=  AndA   OPER |=  OrA    OPER ^=  XorA
OPER =   Assign OPER +   Add    OPER -   Sub    OPER *   Mul
OPER %   Mod    OPER <   Lt     OPER >   Gt     OPER &   And
OPER |   Or     OPER ^   Xor    OPER ~   Tilde  OPER !   Not
OPER ?   Cond   OPER :   Colon  OPER ;   Semi   OPER ,   Comma
OPER (   Lparen OPER )   Rparen OPER {   Lbrace OPER }   Rbrace
OPER [   Brak   OPER ]   Rbrak  OPER .   Dot

\ -- the source ---------------------------------------------------------

VARIABLE SRC   VARIABLE SLEN   VARIABLE POS   VARIABLE LINE
VARIABLE CONFORMING   0 CONFORMING !

\ Preprocessor mode. In it '#' and '##' become tokens instead of a
\ line to skip, <header> after `include` is one Str token, and an
\ identifier records whether '(' touches it. Everything else -- every
\ escape quirk, every operator -- is the same lexer, which is the
\ point: pp.f works on TOKENS, so there is only ever one tokenizer.
VARIABLE PPMODE    0 PPMODE !
VARIABLE WANT-HDR  0 WANT-HDR !
: BYTES2= ( a1 u1 a2 u2 -- f )
   ROT OVER <> IF DROP 2DROP 0 EXIT THEN  BYTES= ;

: PEEK ( i -- c )  DUP SLEN @ < IF SRC @ + C@ ELSE DROP 0 THEN ;
: CH   ( -- c )    POS @ PEEK ;
: AT-END? ( -- f ) POS @ SLEN @ >= ;

: ID1? ( c -- f ) {: c -- f :}
   c 96 > c 123 < AND   c 64 > c 91 < AND OR   c 95 = OR ;
: ID?  ( c -- f ) DUP ID1? SWAP DUP 47 > SWAP 58 < AND OR ;
: DIG? ( c -- f ) DUP 47 > SWAP 58 < AND ;
: OCT? ( c -- f ) DUP 47 > SWAP 56 < AND ;
: HEXV ( c -- v ) {: c -- v :}
   c DIG? IF c 48 - EXIT THEN
   c 96 > c 103 < AND IF c 87 - EXIT THEN
   c 64 > c 71 < AND IF c 55 - EXIT THEN
   -1 ;

\ -- tokens -------------------------------------------------------------

\ t.adj is set on an identifier that '(' TOUCHES, which is the one bit
\ that separates a function-like #define from an object-like one whose
\ body happens to start with a paren. t.file is the serial of the
\ buffer the token was lexed from: a directive runs to the end of its
\ LINE, and after an #include two different files' line numbers sit
\ next to each other on the stream.
BEGIN-STRUCTURE TOK
   FIELD: t.kind   FIELD: t.val   FIELD: t.len   FIELD: t.line
   FIELD: t.adj    FIELD: t.file
END-STRUCTURE

CREATE TOKS VEC ALLOT
VARIABLE LEXV   TOKS LEXV !               \ where TOK, appends
VARIABLE LEXF   0 LEXF !                  \ serial of the buffer in hand
VARIABLE T-ADJ  0 T-ADJ !                 \ consumed by the next TOK,
: TOK, ( kind val len -- ) {: k v n | t -- :}
   TOK ALLOT: TO t
   k t t.kind !  v t t.val !  n t t.len !  LINE @ t t.line !
   T-ADJ @ t t.adj !  0 T-ADJ !  LEXF @ t t.file !
   t LEXV @ V, ;

\ -- scanners -----------------------------------------------------------

: SKIP-LINE  BEGIN AT-END? 0= CH 10 <> AND WHILE 1 POS +! REPEAT ;
: SKIP-BLOCK
   2 POS +!
   BEGIN AT-END? 0= WHILE
      CH 10 = IF 1 LINE +! THEN
      CH 42 = POS @ 1+ PEEK 47 = AND IF 2 POS +! EXIT THEN
      1 POS +!
   REPEAT ;

: SCAN-DEC ( v -- v )  BEGIN CH DIG? WHILE 10 * CH 48 - + 1 POS +! REPEAT ;
: SCAN-OCT ( v -- v )  BEGIN CH OCT? WHILE  8 * CH 48 - + 1 POS +! REPEAT ;
: SCAN-HEX ( -- v )
   0 BEGIN CH HEXV DUP 0< 0= WHILE SWAP 16 * + 1 POS +! REPEAT DROP ;

\ nonzero -> decimal, 0x/0X -> hex, else octal (0 alone falls out as 0)
: SCAN-NUMBER ( -- v )
   CH 48 <> IF 0 SCAN-DEC EXIT THEN
   1 POS +!
   CH 120 = CH 88 = OR IF 1 POS +! SCAN-HEX EXIT THEN
   0 SCAN-OCT ;

\ c4cc's escape table, quirks preserved: \n->10 \t->8 \r->10 \0->0,
\ anything else escaped is itself. -conforming decodes what C says.
: ESC1 ( c -- b ) {: c -- b :}
   c 110 = IF 10 EXIT THEN
   c 116 = IF  8 EXIT THEN
   c 114 = IF 10 EXIT THEN
   c  48 = IF  0 EXIT THEN
   c ;
: ESC-OCT ( -- v )                      \ C stops an octal escape at three
   0 0 BEGIN DUP 3 < CH OCT? AND WHILE SWAP 8 * CH 48 - + SWAP 1+ 1 POS +! REPEAT DROP ;
: ESCAPE ( -- b )                       \ POS is just past the backslash
   CONFORMING @ 0= IF CH ESC1 1 POS +! EXIT THEN
   CH 110 = IF 1 POS +! 10 EXIT THEN
   CH 116 = IF 1 POS +!  9 EXIT THEN
   CH 114 = IF 1 POS +! 13 EXIT THEN
   CH  97 = IF 1 POS +!  7 EXIT THEN
   CH  98 = IF 1 POS +!  8 EXIT THEN
   CH 102 = IF 1 POS +! 12 EXIT THEN
   CH 118 = IF 1 POS +! 11 EXIT THEN
   CH 120 = CH 88 = OR IF 1 POS +! SCAN-HEX EXIT THEN
   CH OCT? IF ESC-OCT EXIT THEN
   CH 1 POS +! ;

\ index of the closing quote, backslash consuming the next byte
: STR-END ( q -- e ) {: q -- e :}
   POS @
   BEGIN DUP SLEN @ < WHILE
      DUP PEEK DUP q = IF DROP EXIT THEN
      92 = IF 2 + ELSE 1+ THEN
   REPEAT ;

: DECODE ( end dst -- n ) {: end dst -- n :}
   0
   BEGIN POS @ end < WHILE
      CH 92 = IF 1 POS +! ESCAPE ELSE CH 1 POS +! THEN
      OVER dst + C!  1+
   REPEAT ;

\ -- the token makers ---------------------------------------------------

: T-IDENT {: | s a u -- :}
   POS @ TO s
   BEGIN CH ID? WHILE 1 POS +! REPEAT
   SRC @ s + TO a   POS @ s - TO u
   \ In PPMODE every word is an Id: keywords do not exist yet. C
   \ recognises them in a phase AFTER macro expansion, which is exactly
   \ why `#define int long` is legal and why `#ifndef int` asks about a
   \ macro rather than about a type. pp.f classifies them at the end.
   PPMODE @ IF
      a u S" include" BYTES2= IF 1 WANT-HDR ! THEN   \ arm the <...> scan
      CH 40 = IF 1 T-ADJ ! THEN                      \ '(' TOUCHES the name
      Id a u TOK, EXIT
   THEN
   a u KW-FIND DUP 0< 0= IF 0 0 TOK, EXIT THEN
   DROP  Id a u TOK, ;

: T-NUM   Num SCAN-NUMBER 0 TOK, ;

: T-STR {: | end dst n -- :}
   1 POS +!
   34 STR-END TO end
   end POS @ - 1+ ALLOT: TO dst
   end dst DECODE TO n
   end 1+ POS !
   Str dst n TOK, ;

: T-CHAR {: | end dst n -- :}
   1 POS +!
   39 STR-END TO end
   end POS @ - 1+ ALLOT: TO dst
   end dst DECODE TO n
   end 1+ POS !
   Num  n 0> IF dst n 1- + C@ ELSE 0 THEN  0 TOK, ;

: T-SLASH {: | d -- :}
   POS @ 1+ PEEK TO d
   d 47 = IF SKIP-LINE EXIT THEN
   d 42 = IF SKIP-BLOCK EXIT THEN
   d 61 = IF DivA 0 0 TOK, 2 POS +! EXIT THEN
   Div 0 0 TOK, 1 POS +! ;

: OP-MATCH ( -- kind len ) {: | best blen n -- :}
   -1 TO best   0 TO blen
   #OP @ 0 ?DO
      I CELLS OPLEN + @ TO n
      n blen >  POS @ n + SLEN @ <=  AND IF
         SRC @ POS @ +  I CELLS OPOFF + @ OPBUF +  n  BYTES= IF
            I CELLS OPKIND + @ TO best   n TO blen
         THEN
      THEN
   LOOP
   best blen ;

\ -- the loop -----------------------------------------------------------

\ #include <name>: one Str token, not a stream of operators
: T-HEADER {: | s n dst -- :}
   1 POS +!  POS @ TO s
   BEGIN AT-END? 0= CH 62 <> AND CH 10 <> AND WHILE 1 POS +! REPEAT
   POS @ s - TO n
   n 1+ ALLOT: TO dst   SRC @ s + dst n MOVE
   1 POS +!  0 WANT-HDR !
   1 T-ADJ !                              \ t.adj on a Str marks <angle>
   Str dst n TOK, ;

: T-HASH
   PPMODE @ 0= IF SKIP-LINE EXIT THEN
   POS @ 1+ PEEK 35 = IF HashHash 0 0 TOK, 2 POS +! EXIT THEN
   Hash 0 0 TOK, 1 POS +! ;

: LEX-STEP {: | c -- :}
   CH TO c
   c 92 = POS @ 1+ PEEK 10 = AND IF 2 POS +! EXIT THEN   \ splice
   c 10 = IF 1 POS +! 1 LINE +! 0 WANT-HDR ! EXIT THEN
   c 33 < IF 1 POS +! EXIT THEN
   c 35 = IF T-HASH EXIT THEN
   WANT-HDR @ c 60 = AND IF T-HEADER EXIT THEN
   c ID1? IF T-IDENT EXIT THEN
   c DIG? IF T-NUM   EXIT THEN
   c 47  = IF T-SLASH EXIT THEN
   c 34  = IF T-STR  EXIT THEN
   c 39  = IF T-CHAR EXIT THEN
   OP-MATCH OVER 0< IF 2DROP 1 POS +! EXIT THEN
   POS +!  0 0 TOK, ;

CREATE PATHB 1024 ALLOT
: ZPATH ( a u -- z )  DUP >R PATHB SWAP MOVE 0 PATHB R@ + C! R> DROP PATHB ;

\ A file is read into a scratch buffer and then copied into the arena
\ at its real size, because Id and Str tokens point INTO the source and
\ so it has to outlive the scan -- and because #include means several
\ sources are live at once.
\ Grown as the file is read rather than sized for the largest source
\ anyone might ever hand it. It is kept between files, so the cost is
\ the biggest single source, not the sum.
VARIABLE RDMAX   0 RDMAX !
VARIABLE RDBUF   0 RDBUF !
: RD-GROW ( -- ) {: | n new -- :}
   RDMAX @ 2* 65536 MAX TO n
   n ALLOCATE TO new
   new 0= IF ." lex: out of memory reading the source" CR ABORT THEN
   RDBUF @ IF RDBUF @ new RDMAX @ MOVE THEN
   new RDBUF !   n RDMAX ! ;
VARIABLE #FILES  0 #FILES !
: READ-FILE ( a u -- addr len ) {: a u | fd n dst -- addr len :}
   a u ZPATH OPENF TO fd
   fd 0< IF ." lex: cannot open the source" CR ABORT THEN
   0 TO n
   BEGIN
      n 65536 + RDMAX @ > IF RD-GROW THEN
      fd RDBUF @ n + 65536 READF DUP 0>
   WHILE n + TO n REPEAT DROP
   fd CLOSEF
   n 1+ ALLOT: TO dst   RDBUF @ dst n MOVE
   dst n ;

: LEX-BUF ( addr len v -- ) {: addr len v -- :}
   addr SRC !  len SLEN !  0 POS !  1 LINE !  0 WANT-HDR !  0 T-ADJ !
   v LEXV !  1 #FILES +!  #FILES @ LEXF !
   BEGIN AT-END? 0= WHILE LEX-STEP REPEAT
   Eof 0 0 TOK, ;

: NEW-VEC ( n -- v ) {: n | v -- v :}  VEC ALLOT: TO v  v n VEC-INIT  v ;

: LEX-FILE ( a u -- )                     \ the whole file into TOKS
   READ-FILE TOKS DUP 262144 VEC-INIT LEX-BUF ;
: LEX-FILE>V ( a u -- v )                 \ ... into a vector of its own
   READ-FILE 4096 NEW-VEC DUP >R LEX-BUF R> ;
\ The directory a quoted #include is resolved relative to: everything up
\ to the last '/' of the path, empty when there is none.
: DIRNAME ( a u -- a u' ) {: a u | r -- a r :}   \ includes the trailing '/'
   0 TO r
   u 0 ?DO a I + C@ 47 = IF I 1+ TO r THEN LOOP
   a r ;
: LEX-STR>V ( a u -- v ) {: a u | dst -- v :}
   u 1+ ALLOT: TO dst  a dst u MOVE
   dst u 64 NEW-VEC DUP >R LEX-BUF R> ;

\ -- the dump, in c4lc's format -----------------------------------------
\ Rendered into a buffer and then printed AS A C STRING, stopping at the
\ first nul. That is not decoration: c4sp prints a token line that way,
\ so `"tab\there\rcr\0nul..."` loses everything from the \0 onward --
\ the rest of the string, the line number and the closing paren. c4lc's
\ TOKEN is intact; only its dump is lossy. Reproducing it keeps the
\ oracle exact, which is worth more here than a prettier dump: a change
\ in what the lexer does stays a diff instead of hiding among a known
\ one. The faithful bytes are still checked -- by every phase after this
\ one, which reads the token and not the transcript.

CREATE OBUF 4096 ALLOT   VARIABLE OBN
: OB-RESET  0 OBN ! ;
: OB-C  ( c -- )   OBN @ 4096 < IF OBUF OBN @ + C! 1 OBN +! ELSE DROP THEN ;
: OB-T  ( a u -- ) 0 ?DO DUP C@ OB-C 1+ LOOP DROP ;
CREATE NBUF 24 ALLOT   VARIABLE NBP
: OB-N ( n -- )
   DUP 0< IF 45 OB-C NEGATE THEN
   NBUF 24 + NBP !
   BEGIN -1 NBP +!  DUP 10 MOD 48 + NBP @ C!  10 /  DUP 0= UNTIL DROP
   NBP @  NBUF 24 + OVER -  OB-T ;
: OB-FLUSH ( -- )                       \ print it as a C string, then CR
   0                                    \ bytes before the first nul
   BEGIN DUP OBN @ < IF DUP OBUF + C@ 0<> ELSE 0 THEN WHILE 1+ REPEAT
   OBUF SWAP TYPE CR ;

: .TOK ( t -- ) {: t -- :}
   OB-RESET
   [CHAR] ( OB-C
   t t.kind @ KNAME OB-T  32 OB-C
   t t.kind @ Num = IF t t.val @ OB-N ELSE
   t t.kind @ Id  = IF t t.val @ t t.len @ OB-T ELSE
   t t.kind @ Str = IF t t.val @ t t.len @ OB-T ELSE
   48 OB-C THEN THEN THEN
   32 OB-C  t t.line @ OB-N
   [CHAR] ) OB-C
   OB-FLUSH ;
: DUMP-TOKENS  TOKS V# 0 ?DO I TOKS V@ .TOK LOOP ;
: COUNT-TOKENS ( -- ) OB-RESET S" tokens " OB-T TOKS V# OB-N OB-FLUSH ;
