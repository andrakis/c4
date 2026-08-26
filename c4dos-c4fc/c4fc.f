\ c4th core.f -- the part of Forth-2012 CORE that is written in Forth.
\
\ Everything here is built from the primitives and from IMMEDIATE, which
\ is the point: control structures are ordinary definitions that run at
\ compile time and lay down branches, not syntax wired into the C.

\ -- conditionals ------------------------------------------------------
\ IF leaves the address of the branch operand for THEN to fill in. The
\ operand cell is written as 0 and patched later; nothing ever reads it
\ before then.

: IF     POSTPONE 0BRANCH HERE 0 , ; IMMEDIATE
: THEN   HERE SWAP ! ; IMMEDIATE
: ELSE   POSTPONE BRANCH HERE 0 ,  SWAP  HERE SWAP ! ; IMMEDIATE

\ -- indefinite loops --------------------------------------------------

: BEGIN  HERE ; IMMEDIATE
: AGAIN  POSTPONE BRANCH , ; IMMEDIATE
: UNTIL  POSTPONE 0BRANCH , ; IMMEDIATE
\ WHILE leaves ( orig dest ) -- the SWAP is load-bearing and belongs
\ HERE, not in REPEAT. It keeps BEGIN's destination on top, so a second
\ WHILE can stack another orig underneath it and REPEAT still finds the
\ pair it needs. Put the SWAP in REPEAT instead and the single-WHILE case
\ still works while BEGIN .. WHILE .. WHILE .. REPEAT .. ELSE .. THEN
\ compiles a branch to the wrong address -- which is a segfault, not a
\ failed test. The Forth-2012 suite's GI5 is exactly that shape.
: WHILE  POSTPONE 0BRANCH HERE 0 , SWAP ; IMMEDIATE
: REPEAT POSTPONE BRANCH , HERE SWAP ! ; IMMEDIATE

\ -- defining words ----------------------------------------------------

: VARIABLE  CREATE 1 CELLS ALLOT ;
: CONSTANT  CREATE , DOES> @ ;

\ -- counted loops -----------------------------------------------------
\ LEAVE needs somewhere to record the branches it lays down, and there
\ may be several per loop, so they go on their own stack rather than on
\ the data stack where DO's loop-start address is already sitting. DO
\ records the depth it started at; LOOP resolves everything above it.

CREATE LVSTK 64 CELLS ALLOT
VARIABLE LVSP   0 LVSP !

: LV>    ( a -- )  LVSP @ CELLS LVSTK + !  1 LVSP +! ;
: >LV    ( -- a )  -1 LVSP +!  LVSP @ CELLS LVSTK + @ ;

: DO     POSTPONE (DO)  LVSP @  HERE ; IMMEDIATE
\ ?DO's skip is just another leave: if the limits are equal it branches to
\ wherever LOOP ends up, which is exactly what the leave stack resolves.
\ The depth is captured before that branch is recorded, so LOOP resolves
\ it along with any real LEAVEs. No UNLOOP on this path -- no loop was
\ ever entered.
: ?DO    LVSP @ >R
         POSTPONE 2DUP POSTPONE =
         POSTPONE 0BRANCH HERE 0 ,
         POSTPONE 2DROP
         POSTPONE BRANCH HERE 0 , LV>
         HERE SWAP !
         POSTPONE (DO) R> HERE ; IMMEDIATE
: LEAVE  POSTPONE UNLOOP POSTPONE BRANCH HERE 0 , LV> ; IMMEDIATE

: (RESOLVE-LEAVES)  ( depth -- )
         BEGIN  DUP LVSP @ <  WHILE  >LV HERE SWAP !  REPEAT  DROP ;

: LOOP   POSTPONE (LOOP) ,  (RESOLVE-LEAVES) ; IMMEDIATE
: +LOOP  POSTPONE (+LOOP) , (RESOLVE-LEAVES) ; IMMEDIATE

\ -- scaling -----------------------------------------------------------
\ */ and */MOD keep the intermediate product double wide, which is the
\ whole reason they exist: 3 * MAX-INT / MAX-INT is 3, not an overflow.

: */MOD  ( a b c -- rem quot )  >R M* R> SM/REM ;
: */     ( a b c -- q )  */MOD SWAP DROP ;

\ -- shorthands --------------------------------------------------------

: SPACES ( n -- )  0 MAX  0 ?DO SPACE LOOP ;
: ?      ( a -- )  @ . ;
: WITHIN ( n lo hi -- f )  OVER - >R - R> U< ;
: TRUE   -1 ;
: FALSE  0 ;
: NOT    0= ;
: 2!     ( x y a -- )  SWAP OVER ! CELL+ ! ;
: 2@     ( a -- x y )  DUP CELL+ @ SWAP @ ;
: ERASE  ( a n -- )  0 FILL ;
: CMOVE  ( s d n -- )  MOVE ;
\ c4th ext.f -- the Forth-2012 word sets c4th's kernel does not carry.
\
\ Nothing here is invented. CASE/OF/ENDOF/ENDCASE and DEFER/IS are CORE
\ EXT; BEGIN-STRUCTURE and friends are the Structures word set; VALUE and
\ TO are CORE EXT. They are written in Forth because they can be, which
\ is the same reason core.f exists -- and because a DSL built on standard
\ syntax has a standard test suite waiting for it, which one built on
\ invented syntax never does.

\ -- selection ----------------------------------------------------------
\ The classic implementation: CASE leaves a count on the compile-time
\ stack, each OF adds one, and ENDCASE resolves that many THENs.

: CASE     0 ; IMMEDIATE
: OF       1+ >R POSTPONE OVER POSTPONE = POSTPONE IF POSTPONE DROP R> ; IMMEDIATE
: ENDOF    >R POSTPONE ELSE R> ; IMMEDIATE
: ENDCASE  POSTPONE DROP 0 ?DO POSTPONE THEN LOOP ; IMMEDIATE

\ -- deferred words -----------------------------------------------------
\ Mutual recursion needs these. A recursive-descent parser is one big
\ mutually recursive family and Forth insists on definition before use,
\ so the family is DEFERred first and filled in as each member is
\ written. That is not a workaround -- it is a table of what the parser
\ consists of, written down before the code.

: DEFER      CREATE ['] ABORT , DOES> @ EXECUTE ;
: DEFER@     >BODY @ ;
: DEFER!     >BODY ! ;
: IS         STATE @ IF POSTPONE ['] POSTPONE DEFER! ELSE ' DEFER! THEN ; IMMEDIATE
: ACTION-OF  STATE @ IF POSTPONE ['] POSTPONE DEFER@ ELSE ' DEFER@ THEN ; IMMEDIATE

\ -- values -------------------------------------------------------------

: VALUE  CREATE , DOES> @ ;
: (TO)   ' >BODY STATE @ IF POSTPONE LITERAL POSTPONE ! ELSE ! THEN ;
: TO     (TO) ; IMMEDIATE

\ -- structures ---------------------------------------------------------
\ Records, so that an AST node is  node .op @  and not  2 CELLS + @ .
\ self.f has twenty sites doing the second thing; that is the argument
\ for this file in one line.

: BEGIN-STRUCTURE  CREATE HERE 0 0 , DOES> @ ;
: END-STRUCTURE    SWAP ! ;
: +FIELD   ( n1 n2 "name" -- n3 )  CREATE OVER , + DOES> @ + ;
: FIELD:   ( n1 "name" -- n2 )     ALIGNED 1 CELLS +FIELD ;
: CFIELD:  ( n1 "name" -- n2 )     1 CHARS +FIELD ;
\ c4th locals.f -- named arguments and locals, Forth-2012 syntax.
\
\   : ATOI {: a u -- n :}  0  u 0 ?DO 10 * a I + C@ 48 - + LOOP ;
\
\ This is the word that makes a compiler writable. self.f has twenty-one
\ VARIABLEs whose only job is to hold a value across three lines because
\ there was nowhere else to put it -- TQ PQ NQ EQ HQ HK NA NN2 STA STN
\ STO WI TA TN and the rest. Every one of them is a local that could not
\ be spelled, and every one of them is also a re-entrancy bug waiting for
\ the day the word is called recursively. A parser is recursive.
\
\ HOW IT WORKS, in three parts and no magic.
\
\ 1. Names. {: parses the names into a compile-time table. They are NOT
\    dictionary entries -- c4th's dictionary and its code share one
\    space, so a header created here would land in the middle of the
\    body being compiled. Instead they are found through NOTFOUND, the
\    outer interpreter's one extension point: a word that is neither
\    defined nor a number is offered to a handler before the error. A
\    local name arrives there and compiles a frame access.
\
\ 2. The frame. Locals live on a stack of their own, not on the return
\    stack, because c4th's DO/LOOP keeps its parameters there and I
\    inside a loop would read the wrong thing. Each frame stores the
\    previous frame pointer, so recursion works.
\
\ 3. The end. ; and EXIT are redefined to drop the frame first. That is
\    safe rather than clever: c4th hides a definition while compiling
\    it, so the ; that ends the new ; finds the old one.

\ -- the frame, at run time --------------------------------------------

512 CONSTANT LSMAX
CREATE LSTK LSMAX CELLS ALLOT
VARIABLE LFP   VARIABLE LSP
0 LFP !  1 LSP !

: LA ( i -- a )  CELLS LSTK + ;

: (LFRAME) ( x1..xn n m -- )            \ n from the stack, m slots in all
   LSP @ >R
   LFP @ R@ LA !                        \ the frame remembers its parent
   R@ 1+ + LSP !
   R@ LFP !
   BEGIN DUP 0> WHILE
      1-  DUP LFP @ + 1+ LA  SWAP >R ! R>
   REPEAT
   DROP R> DROP ;

: (LDROP)  ( -- )   LFP @ LSP !  LFP @ LA @ LFP ! ;
: (LOCAL@) ( i -- x )    LFP @ + 1+ LA @ ;
: (LOCAL!) ( x i -- )    LFP @ + 1+ LA ! ;

\ -- the names, at compile time ----------------------------------------

32 CONSTANT LMAX
CREATE LNBUF 1024 ALLOT
CREATE LNOFF LMAX CELLS ALLOT
CREATE LNLEN LMAX CELLS ALLOT
VARIABLE LN     VARIABLE LNB
VARIABLE LARGS  VARIABLE LON
VARIABLE LPH    VARIABLE LDONE

: BYTES= ( a1 a2 u -- f )
   0 ?DO
      OVER C@ OVER C@ <> IF 2DROP 0 UNLOOP EXIT THEN
      1+ SWAP 1+ SWAP
   LOOP 2DROP -1 ;
: SAME? ( a1 u1 a2 u2 -- f )
   ROT OVER <> IF DROP 2DROP 0 EXIT THEN
   BYTES= ;

: LNAME ( i -- a u )  DUP CELLS LNOFF + @ LNBUF +  SWAP CELLS LNLEN + @ ;
: LADD  ( a u -- )
   LN @ LMAX < 0= IF ." locals.f: too many locals" CR ABORT THEN
   LNB @ LN @ CELLS LNOFF + !
   DUP LN @ CELLS LNLEN + !
   >R LNBUF LNB @ + R@ MOVE
   R> LNB +!
   1 LN +! ;
: LFIND ( a u -- i | -1 )
   LON @ 0= IF 2DROP -1 EXIT THEN
   LN @ 0 ?DO 2DUP I LNAME SAME? IF 2DROP I UNLOOP EXIT THEN LOOP
   2DROP -1 ;

\ The handler. Chaining is deliberate: a later layer can save this one
\ and add syntax of its own without editing this file.
VARIABLE LPREV
: L-NOTFOUND ( a u -- f )
   2DUP LFIND DUP 0< 0= IF
      >R 2DROP R> POSTPONE LITERAL POSTPONE (LOCAL@) -1 EXIT
   THEN DROP
   LPREV @ ?DUP IF EXECUTE ELSE 2DROP 0 THEN ;

\ -- the syntax ---------------------------------------------------------

\ Forth-2012 allows one {: per definition, and a second one used to
\ silently discard the first one's names -- which shows up much later as
\ "TO: not found" on a name that is plainly right there. Say so instead.
: {:
   LON @ IF ." locals.f: a second {: in one definition" CR ABORT THEN
   0 LN ! 0 LNB ! 1 LON ! 0 LARGS ! 0 LPH ! 0 LDONE !
   BEGIN
      BL WORD COUNT DUP 0= IF 2DROP ." locals.f: {: without :}" CR ABORT THEN
      2DUP S" :}" SAME? IF 1 LDONE ! THEN
      LDONE @ 0= IF
         2DUP S" |"  SAME? IF 1 LPH ! ELSE
         2DUP S" --" SAME? IF 2 LPH ! ELSE
            LPH @ 2 < IF 2DUP LADD  LPH @ 0= IF 1 LARGS +! THEN THEN
         THEN THEN
      THEN
      2DROP
      LDONE @
   UNTIL
   LARGS @ POSTPONE LITERAL
   LN @ POSTPONE LITERAL
   POSTPONE (LFRAME) ; IMMEDIATE

' ; CONSTANT (OLD;)
: ;  LON @ IF POSTPONE (LDROP) 0 LON ! 0 LN ! THEN (OLD;) EXECUTE ; IMMEDIATE

' EXIT CONSTANT (OLD-EXIT)
: EXIT  LON @ IF POSTPONE (LDROP) THEN (OLD-EXIT) , ; IMMEDIATE

: TO ( "name" -- )
   BL WORD DUP COUNT LFIND DUP 0< 0= IF
      NIP
      STATE @ IF POSTPONE LITERAL POSTPONE (LOCAL!)
              ELSE DROP ." locals.f: TO a local outside a definition" CR THEN
   ELSE
      DROP FIND 0= IF ." locals.f: TO: not found" CR ABORT THEN
      >BODY STATE @ IF POSTPONE LITERAL POSTPONE ! ELSE ! THEN
   THEN ; IMMEDIATE

NOTFOUND @ LPREV !
' L-NOTFOUND NOTFOUND !
\ dos.f -- producing a file on C4DOS.
\
\ The C4 VM has no write syscall. Natively c4th has SAVE-FILE, which
\ opens and writes with the host's own calls; under c4m, c4mp or c4bb
\ there is no such thing, and the ONLY way a program running there can
\ put bytes on a disk is to ask C4DOS to do it.
\
\ C4DOS's loader patches the address of its API table into any image
\ carrying the symbol __c4dos_api (include/c4dos.h), and c4th carries
\ it, so C4DOS-API is that address or zero. The table's slots hold
\ ordinary function addresses, so INVOKE1/2/3 call them directly -- the
\ invoke stub c4dos.h describes is for C, which cannot call through a
\ variable; Forth can.
\
\ Slot numbers are include/c4dos.h's and are v1, so every DOS that has
\ an API table at all has these three.

2 CONSTANT DOS-CREATE   3 CONSTANT DOS-WRITE   4 CONSTANT DOS-CLOSE

: DOS? ( -- f )  C4DOS-API 0<> ;
: DOS-SLOT ( n -- addr )  CELLS C4DOS-API + @ ;

\ The name has to be NUL-terminated for DOS, which takes char *; Forth
\ strings are address and count, so this is where the two meet.
CREATE DOSNAME 128 ALLOT
: >DOSZ ( a u -- z )
   DUP 127 > IF ." dos.f: name too long" CR ABORT THEN
   DUP >R  DOSNAME SWAP MOVE  0 DOSNAME R@ + C!  R> DROP  DOSNAME ;

\ ( addr len a u -- ok )  Write a block to a named file on the RAM disk.
: DOS-SAVE {: a n na nu | h w -- ok :}
   DOS? 0= IF 0 EXIT THEN
   na nu >DOSZ  DOS-CREATE DOS-SLOT INVOKE1 TO h
   h 0< IF 0 EXIT THEN
   \ write(handle, buf, len) -- c4dos.h's order, and INVOKE3 passes the
   \ three in the order they are on the stack.
   h a n  DOS-WRITE DOS-SLOT INVOKE3 TO w
   h DOS-CLOSE DOS-SLOT INVOKE1 DROP
   w n = ;

\ One name for "put this block in this file", whichever machine we are
\ on: DOS if there is a DOS, the host's own open/write if we are native,
\ and an honest failure on a bare VM where neither exists.
: SAVE-BLOCK ( addr len a u -- ok )
   DOS? IF DOS-SAVE EXIT THEN
   SAVE-FILE ;
\ c4fc dsl.f -- the vocabulary a C compiler is written in.
\
\ Three ideas, and every C feature added later is one row in each.
\
\ NODES. A node is a record whose first cell is a TAG. NODE: allocates
\ the tag and opens a field list; field names are shared across kinds on
\ purpose -- two kinds that both have >lhs at the same offset SHOULD use
\ the same accessor, and Forth's redefinition makes that free rather than
\ a collision.
\
\ GENERICS. A generic is a table of execution tokens indexed by tag, so
\ dispatch is one indexed fetch, not a chain of comparisons. self.f's
\ DODIR is a twenty-arm  DUP n = IF ... EXIT THEN  chain: linear in the
\ number of C constructs, and every new construct edits it. A generic is
\ O(1) and nothing existing is touched.
\
\ TABLES. Keywords, operator precedence, type rules -- each is a list of
\ rows, one per feature, declared next to nothing else.

\ -- arena --------------------------------------------------------------
\ A compiler is a batch process: bump-allocate and let go of everything
\ at the end. No free, no GC, no ownership.

\ It is a LIST OF BLOCKS, because a bump allocator that never frees can
\ simply start another one -- nothing moves, so every pointer already
\ handed out stays good. That matters more than it sounds: the arena
\ used to be one 64 MB malloc, which is larger than the whole of c4bb.
\ A compiler that only fits on the machine that is compiling FOR the
\ small machine is the wrong shape.
VARIABLE ARENA-TOP   VARIABLE ARENA-END   VARIABLE ARENA-BLK
: ARENA-BLOCK ( n -- ) {: n | b -- :}
   n ARENA-BLK @ MAX TO n
   n ALLOCATE TO b
   b 0= IF ." c4fc: out of memory for the arena" CR ABORT THEN
   b ARENA-TOP !   b n + ARENA-END !
   ARENA-BLK @ 2* ARENA-BLK ! ;          \ later blocks are bigger
: ARENA-INIT ( n -- )  DUP ARENA-BLK !  ARENA-BLOCK ;
: ALLOT: ( n -- a )                       \ cell-aligned: cells are what go in it
   {: n | a -- a :}
   n 1 CELLS 1- + 1 CELLS 1- INVERT AND TO n
   ARENA-TOP @ n + ARENA-END @ > IF n ARENA-BLOCK THEN
   ARENA-TOP @ TO a
   a n + ARENA-TOP !
   a ;

\ -- growable vectors ---------------------------------------------------
\ Token lists, instruction lists, symbol tables. Arrays, not cons cells.

BEGIN-STRUCTURE VEC
   FIELD: v.data
   FIELD: v.len
   FIELD: v.cap
END-STRUCTURE

: VEC-INIT ( v n -- )  {: v n -- :}  n CELLS ALLOCATE v v.data !  0 v v.len !  n v v.cap ! ;
\ Doubling, and the old block is simply let go of: there is no FREE in
\ c4th and a compiler is a batch process, so the peak is what matters
\ and it is bounded by twice the final size.
\
\ GROW is the same idea for the buffers that are NOT vectors -- the code
\ stream, the patch table, the symbol section. Each is a raw block with
\ its own cursor, so the pattern is: notice it is full where the bounds
\ check already was, and double it there.
: GROW ( addrvar capvar elembytes -- ) {: av cv esz | n new -- :}
   cv @ 2* TO n
   n esz * ALLOCATE TO new
   new 0= IF ." c4fc: out of memory growing a buffer" CR ABORT THEN
   av @ new  cv @ esz *  MOVE
   new av !   n cv ! ;
: VEC-GROW ( v -- ) {: v | nc nd -- :}
   v v.cap @ 2* DUP 0= IF DROP 16 THEN TO nc
   nc CELLS ALLOCATE TO nd
   nd 0= IF ." c4fc: out of memory growing a vector" CR ABORT THEN
   v v.data @ nd v v.len @ CELLS MOVE
   nd v v.data !  nc v v.cap ! ;
: V, ( x v -- )
   {: x v -- :}
   v v.len @ v v.cap @ >= IF v VEC-GROW THEN
   x  v v.data @ v v.len @ CELLS + !
   1 v v.len +! ;
: V@ ( i v -- x )  v.data @ SWAP CELLS + @ ;
: V! ( x i v -- )  v.data @ SWAP CELLS + ! ;
: V# ( v -- n )    v.len @ ;

\ -- diagnostics --------------------------------------------------------
\ A number with no trailing space, because c4th's . prints one and a
\ message reads better without it.

CREATE .NBUF 24 ALLOT   VARIABLE .NBP
: .N ( n -- )
   DUP 0< IF 45 EMIT NEGATE THEN
   .NBUF 24 + .NBP !
   BEGIN -1 .NBP +!  DUP 10 MOD 48 + .NBP @ C!  10 /  DUP 0= UNTIL DROP
   .NBP @  .NBUF 24 + OVER -  TYPE ;

\ -- nodes --------------------------------------------------------------

64 CONSTANT TAG-MAX
VARIABLE #TAGS       0 #TAGS !
VARIABLE NODE-SIZE                       \ field cursor for the kind in hand

: NODE: ( "name" -- )                    \ allocate a tag, open a field list
   #TAGS @ TAG-MAX < 0= IF ." c4fc: too many node kinds" CR ABORT THEN
   #TAGS @ CONSTANT
   1 #TAGS +!
   1 CELLS NODE-SIZE ! ;                 \ cell 0 is the tag
: NFIELD: ( "name" -- )                  \ a slot in the node in hand
   CREATE NODE-SIZE @ ,  1 CELLS NODE-SIZE +!
   DOES> @ + ;
: ;NODE ( -- ) ;

: >TAG ( node -- tag )  @ ;
: NEW ( tag size -- node )  ALLOT: TUCK ! ;

\ -- generics -----------------------------------------------------------

: NO-METHOD ( node -- )
   ." c4fc: no method for tag " >TAG . CR ABORT ;

: GENERIC: ( "name" -- )
   CREATE  TAG-MAX 0 ?DO ['] NO-METHOD , LOOP
   DOES> ( node body -- )  OVER >TAG CELLS + @ EXECUTE ;

VARIABLE M-TABLE   VARIABLE M-TAG
CREATE M-NAME 96 ALLOT   VARIABLE M-LEN
: M-CAT ( a u -- )                       \ append to the method's name
   DUP >R  M-NAME M-LEN @ +  SWAP MOVE  R> M-LEN +! ;
: :M ( "generic" "tag" -- )              \ open a method on a generic
   S" : " M-LEN ! DROP  2 M-LEN !  S" : " DROP M-NAME 2 MOVE
   BL WORD DUP COUNT M-CAT
   FIND 0= IF ." c4fc: :M unknown generic" CR ABORT THEN
   >BODY M-TABLE !
   S" /" M-CAT
   BL WORD DUP COUNT M-CAT
   FIND 0= IF ." c4fc: :M unknown tag" CR ABORT THEN
   EXECUTE M-TAG !
   S"  " M-CAT
   M-NAME M-LEN @ EVALUATE ;             \ ": EVAL/n_add " -- a real name
: ;M ( -- )
   POSTPONE ;
   LATEST M-TAG @ CELLS M-TABLE @ + ! ; IMMEDIATE
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
CREATE FNAMEA FDIR-MAX CELLS ALLOT
CREATE FNAMEU FDIR-MAX CELLS ALLOT
VARIABLE CURFILE   0 CURFILE !
: FDIR! ( a u serial -- ) {: a u s -- :}
   s FDIR-MAX < IF a s CELLS FDIRA + !  u s CELLS FDIRU + ! THEN ;
: FDIR@ ( serial -- a u ) {: s -- a u :}
   s FDIR-MAX < IF s CELLS FDIRA + @  s CELLS FDIRU + @ ELSE 0 0 THEN ;
: FNAME! ( a u serial -- ) {: a u s -- :}
   s FDIR-MAX < IF a s CELLS FNAMEA + !  u s CELLS FNAMEU + ! THEN ;
\ Which file a token came from, so a diagnostic can say so: a parse
\ error thirty thousand tokens into an #include chain that names only a
\ line number names almost nothing.
: FNAME@ ( serial -- a u ) {: s -- a u :}
   s FDIR-MAX < IF s CELLS FNAMEA + @  s CELLS FNAMEU + @ ELSE 0 0 THEN ;
: .WHERE ( t -- ) {: t | a u -- :}
   t t.file @ FNAME@ TO u TO a
   u IF a u TYPE ELSE ." (source)" THEN
   [CHAR] : EMIT  t t.line @ .N ;

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
   a u PP-STR TO u TO a                       \ the path must outlive the read
   a u LEX-FILE>V TO v
   a u DIRNAME LEXF @ FDIR!
   a u LEXF @ FNAME!
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

\ Keywords, at last. Every word came out of the lexer as an Id because
\ that is the order C puts the phases in; now that no more expansion can
\ happen, the ones that name a keyword become one.
: RECLASSIFY ( -- ) {: | t k -- :}
   TOKS V# 0 ?DO
      I TOKS V@ TO t
      t t.kind @ Id = IF
         t t.val @ t t.len @ KW-FIND TO k
         k 0< 0= IF k t t.kind !  0 t t.val !  0 t t.len ! THEN
      THEN
   LOOP ;

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
   PP-GO
   RECLASSIFY ;

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
\ c4fc types.f -- the type algebra.
\
\ A type is ONE INTEGER, and the encoding is c4lc's rather than one of my
\ own, because the .c4r symbol record carries it and byte-identity means
\ carrying the same number:
\
\    char 0    int 1    and every * adds 2
\    char *    is 2     int *      is 3     int ** is 5
\    struct k  is 1024 + 64k, so struct A is 1024 and struct A * is 1026
\
\ Sixty-four apart is what leaves room for thirty-one levels of
\ indirection before two structs could collide, and it is what c4lc
\ does -- read off three structs in one file rather than guessed.
\
\ Everything a compiler asks of a type is here: how big is it, what does
\ a pointer to it step by, and what is it a pointer TO.

1024 CONSTANT T-STRUCT0
  64 CONSTANT T-STRIDE

256 CONSTANT SMAX-STRUCT
CREATE ST-NAME  SMAX-STRUCT CELLS ALLOT
CREATE ST-NLEN  SMAX-STRUCT CELLS ALLOT
CREATE ST-SIZE  SMAX-STRUCT CELLS ALLOT
VARIABLE #STRUCTS   0 #STRUCTS !

512 CONSTANT MMAX-MEM
CREATE MB-OWNER MMAX-MEM CELLS ALLOT     \ which struct
CREATE MB-NAME  MMAX-MEM CELLS ALLOT
CREATE MB-NLEN  MMAX-MEM CELLS ALLOT
CREATE MB-TYPE  MMAX-MEM CELLS ALLOT
CREATE MB-OFF   MMAX-MEM CELLS ALLOT
CREATE MB-AGG   MMAX-MEM CELLS ALLOT     \ an array member is its own address
VARIABLE #MEMS   0 #MEMS !

: T-BASE ( t -- b )
   DUP T-STRUCT0 < IF 1 AND EXIT THEN
   T-STRUCT0 - T-STRIDE / T-STRIDE * T-STRUCT0 + ;
: T-STARS ( t -- n )  DUP T-BASE - 2 / ;
: T-PTR?  ( t -- f )  T-STARS 0> ;
: T-STRUCT? ( t -- f ) DUP T-STRUCT0 >= SWAP T-STARS 0= AND ;
: T-INDEX ( t -- k )  T-BASE T-STRUCT0 - T-STRIDE / ;
: T-DEREF ( t -- t' ) 2 - ;
: T-ADDR  ( t -- t' ) 2 + ;

: T-SIZE ( t -- n )
   DUP T-PTR? IF DROP 1 CELLS EXIT THEN
   DUP t_char = IF DROP 1 EXIT THEN
   DUP t_int  = IF DROP 1 CELLS EXIT THEN
   T-INDEX CELLS ST-SIZE + @ ;
\ What a pointer of this type steps by. One means "emit no multiply at
\ all", which is not an optimisation but what c4lc does -- char * walks
\ byte by byte with no MUL in sight.
: T-STEP ( t -- n )   T-DEREF T-SIZE ;

: ST-FINDS ( a u -- k|-1 ) {: a u -- k :}
   #STRUCTS @ 0 ?DO
      I CELLS ST-NLEN + @ u = IF
         I CELLS ST-NAME + @ a u BYTES= IF I UNLOOP EXIT THEN
      THEN
   LOOP -1 ;
: ST-NEW ( a u -- k ) {: a u | k -- k :}
   #STRUCTS @ TO k
   k SMAX-STRUCT < 0= IF ." c4fc: too many structs" CR ABORT THEN
   a k CELLS ST-NAME + !  u k CELLS ST-NLEN + !  0 k CELLS ST-SIZE + !
   1 #STRUCTS +!
   k ;
: ST-TYPE ( k -- t )  T-STRIDE * T-STRUCT0 + ;

: MEM, ( struct a u type off agg -- ) {: k a u t o ag -- :}
   #MEMS @ MMAX-MEM < 0= IF ." c4fc: too many struct members" CR ABORT THEN
   k #MEMS @ CELLS MB-OWNER + !
   a #MEMS @ CELLS MB-NAME  + !
   u #MEMS @ CELLS MB-NLEN  + !
   t #MEMS @ CELLS MB-TYPE  + !
   o #MEMS @ CELLS MB-OFF   + !
   ag #MEMS @ CELLS MB-AGG  + !
   1 #MEMS +! ;
: MEM-FIND ( struct a u -- i|-1 ) {: k a u -- i :}
   #MEMS @ 0 ?DO
      I CELLS MB-OWNER + @ k = IF
         I CELLS MB-NLEN + @ u = IF
            I CELLS MB-NAME + @ a u BYTES= IF I UNLOOP EXIT THEN
         THEN
      THEN
   LOOP -1 ;
\ c4fc emit.f -- the code, data, patch and symbol segments, and the
\ .c4r they are written as.
\
\ Matching c4lc byte for byte turns out to mean matching four things
\ that are not obvious from the format alone, and every one of them was
\ found by decoding an image c4lc produced rather than by reading a
\ document:
\
\   1. The code stream is ZERO-based. c4cc emits through *++e so its
\      images never use word 0; c4lc puts its first instruction there.
\      Both are loadable and the two conventions coexist in the tree.
\   2. Version 3, and the eight header bytes that are padding in v2 hold
\      the data segment's MEMSZ -- its size in memory, which is larger
\      than the bytes written.
\   3. The data segment is written with trailing zeros TRIMMED. The
\      loader zero-fills up to memsz, so a string's terminating nul at
\      the very end of the segment is simply not stored, and neither is
\      an uninitialised global.
\   4. LEV ends a function, and `return` emits its own -- so a function
\      whose last statement is a return gets one LEV, not two.

\ Starting sizes, not limits: each of these buffers doubles where its
\ bounds check used to abort. Small on purpose -- most compiles are
\ small, and the one that is not pays for what it uses.
   4096 CONSTANT CMAX0
   8192 CONSTANT DMAX0
   1024 CONSTANT PMAX0
    512 CONSTANT SMAX0
VARIABLE CMAX   VARIABLE DMAX   VARIABLE IMAX   VARIABLE PMAX   VARIABLE SMAX

\ THE DATA SEGMENT IS THREE REGIONS, in this order:
\
\   1. globals WITH an initialiser, in declaration order
\   2. string literals and jump tables, in the order they are met
\   3. globals WITHOUT one
\
\ which is c4lc's layout and is not the obvious one -- a global
\ initialised in a file whose first function contains a string still
\ comes first. Only region 1's addresses are known as they are handed
\ out; regions 2 and 3 are relative until every declaration has been
\ seen, so their patches are revisited at the end.
VARIABLE CODE   VARIABLE CN
VARIABLE IDATA  VARIABLE IDN          \ region 1: initialised globals
VARIABLE DATA   VARIABLE DN           \ region 2: strings and tables
VARIABLE UDN                          \ region 3: the rest, size only
VARIABLE DB2    VARIABLE DB3          \ where regions 2 and 3 begin
VARIABLE PATCH  VARIABLE PN
VARIABLE SYMS   VARIABLE SN
VARIABLE ENTRY
VARIABLE CONS   VARIABLE CONSN        \ constructors, as code indices
VARIABLE DESS   VARIABLE DESN

 0 CONSTANT oLEA   1 CONSTANT oIMM   2 CONSTANT oJMP   3 CONSTANT oJSR
 4 CONSTANT oBZ    5 CONSTANT oBNZ   6 CONSTANT oENT   7 CONSTANT oADJ
 8 CONSTANT oLEV   9 CONSTANT oLI   10 CONSTANT oLC   11 CONSTANT oSI
12 CONSTANT oSC   13 CONSTANT oPSH  14 CONSTANT oOR   15 CONSTANT oXOR
16 CONSTANT oAND  17 CONSTANT oEQ   18 CONSTANT oNE   19 CONSTANT oLT
20 CONSTANT oGT   21 CONSTANT oLE   22 CONSTANT oGE   23 CONSTANT oSHL
24 CONSTANT oSHR  25 CONSTANT oADD  26 CONSTANT oSUB  27 CONSTANT oMUL
28 CONSTANT oDIV  29 CONSTANT oMOD
30 CONSTANT oOPEN 31 CONSTANT oREAD 32 CONSTANT oCLOS 33 CONSTANT oPRTF
34 CONSTANT oMALC 35 CONSTANT oFREE 36 CONSTANT oMSET 37 CONSTANT oMCMP
38 CONSTANT oEXIT
\ The rest of c4's library and intrinsic opcodes. The list index IS the
\ opcode number and it is mirrored in c4m.c, load-c4r.c, oisc4.c, c4cc.c
\ and c4r.lisp, so these are appended and never inserted.
39 CONSTANT oPUTC 40 CONSTANT oPUTS 41 CONSTANT oRALC 42 CONSTANT oMCPY
43 CONSTANT oSTRC 44 CONSTANT oITH  45 CONSTANT o_OPC 46 CONSTANT o_BLT
47 CONSTANT o_TRP 48 CONSTANT oOPCD 49 CONSTANT o_JMP 50 CONSTANT o_ADJ
51 CONSTANT oC4CF 52 CONSTANT oC4CY 53 CONSTANT oTIME 54 CONSTANT oSIGH
55 CONSTANT oSIGI 56 CONSTANT oUSLP 57 CONSTANT oINFO 58 CONSTANT oOPSL
59 CONSTANT oC4IV 60 CONSTANT oFLT
61 CONSTANT oJSRI 62 CONSTANT oJSRS 63 CONSTANT oJMPA
\ c4mp only: the processor opcodes. c4m does not have these; a program
\ that calls them must test __c4_info() & C4I_SMP first.
66 CONSTANT oCPUI 67 CONSTANT oCPUN 68 CONSTANT oCPUS 69 CONSTANT oCPUH
70 CONSTANT oCAS  71 CONSTANT oXCHG 72 CONSTANT oFADD 73 CONSTANT oCWAI
74 CONSTANT oCWAK 75 CONSTANT oIPI  78 CONSTANT oTRAW
\ c4mp's fused array-element opcodes, emitted only under -mcisc.
76 CONSTANT oLXI  77 CONSTANT oSXI
\ The fused opcodes (docs/fused-opcodes.md), emitted only under -mfuse:
\ two- and three-instruction sequences a third of the instructions real
\ workloads execute are made of. LDL..IMMP carry the operand of the
\ instruction they start with, LIP/ADDL/POPA carry none.
79 CONSTANT oLDL  80 CONSTANT oLDG  81 CONSTANT oPSHL 82 CONSTANT oPSHG
83 CONSTANT oLEAP 84 CONSTANT oIMMP 85 CONSTANT oLIP  86 CONSTANT oADDL
87 CONSTANT oSTL  88 CONSTANT oPOPA

BEGIN-STRUCTURE SYMR
   FIELD: y.name  FIELD: y.nlen  FIELD: y.type  FIELD: y.class  FIELD: y.val
   FIELD: y.ct    FIELD: y.agg   FIELD: y.sz
   FIELD: y.va    FIELD: y.nfix  FIELD: y.ini   FIELD: y.sc
END-STRUCTURE
VARIABLE VA-MAKE   0 VA-MAKE !          \ code index of __c4cc_make_va
: SYM[] ( i -- a )  SYMR * SYMS @ + ;

: EMIT-INIT
   CMAX0 CMAX !  DMAX0 DMAX !  DMAX0 IMAX !  PMAX0 PMAX !  SMAX0 SMAX !
   CMAX @ CELLS ALLOCATE CODE !    0 CN !
   DMAX @ ALLOCATE DATA !          0 DN !
   IMAX @ ALLOCATE IDATA !         0 IDN !
   0 UDN !  0 DB2 !  0 DB3 !
   256 CELLS ALLOCATE CONS !       0 CONSN !
   256 CELLS ALLOCATE DESS !       0 DESN !
   IDATA @ IMAX @ 0 FILL
   PMAX @ 3 * CELLS ALLOCATE PATCH ! 0 PN !
   SMAX @ SYMR * ALLOCATE SYMS !  0 SN !
   DATA @ DMAX @ 0 FILL
   -1 ENTRY ! ;

\ -- code ---------------------------------------------------------------

: C, ( w -- )
   CN @ CMAX @ >= IF CODE CMAX 1 CELLS GROW THEN
   CODE @ CN @ CELLS + !  1 CN +! ;
: OP,   ( op -- )    C, ;
: OP2,  ( n op -- )  C, C, ;
: CHERE ( -- n )     CN @ ;

\ The highest code address anything has branched TO in the function
\ being compiled. It answers one question, at the end: is there a LABEL
\ here? A function whose last statement is `if (x) return 1;` ends with
\ a LEV that only the TAKEN arm reaches, and the false arm branches
\ past it -- so it still needs a LEV of its own. Judging that by "the
\ last thing emitted was a LEV" runs off the end of the function into
\ whatever was compiled next; c4lc gets it right because its labels sit
\ in the instruction list and break the chain.
VARIABLE LABMAX   -1 LABMAX !
: LABEL@ ( addr -- )  LABMAX @ MAX LABMAX ! ;
: LABEL-RESET ( -- )  -1 LABMAX ! ;
: LABEL-HERE? ( -- f )  LABMAX @ CHERE >= ;

: LAST-OP ( -- w )   CN @ 0= IF -1 EXIT THEN CODE @ CN @ 1- CELLS + @ ;

: PAT, ( type addr value -- ) {: t a v | p -- :}
   PN @ PMAX @ >= IF PATCH PMAX 3 CELLS GROW THEN
   PN @ 3 * CELLS PATCH @ + TO p
   t p !   a p CELL+ !   v p 2 CELLS + !
   1 PN +! ;

32768 CONSTANT DSMAX
CREATE DSL DSMAX CELLS ALLOT   VARIABLE DSN   0 DSN !

\ A patched operand carries its value in the patch, and what the CODE
\ word holds depends on which kind it is: a data reference writes ZERO
\ there, a code reference writes the target. The asymmetry is real and
\ neither half is visible in a small program -- the data rule shows up
\ the moment there are two string literals (the second's IMM is 0 in the
\ code and 3 in the patch), the code rule the moment a call target is
\ not function zero.
: IMMD, ( off -- ) {: off -- :}         \ IMM of a REGION 2 address
   oIMM OP,  -2 CHERE off PAT,  0 C,
   PN @ 1- DSL DSN @ CELLS + !  1 DSN +! ;
\ The operand form is shared by IMM and by JSRI: calling through a
\ global function pointer takes the same data reference as loading it.
: DOP, ( op off -- ) {: op off -- :}    \ a REGION 1 address, final
   op OP,  -2 CHERE off PAT,  0 C, ;
: IMMI, ( off -- )  oIMM SWAP DOP, ;
: JSRC, ( target -- ) {: t -- :}        \ a call to a known function
   oJSR OP,  -1 CHERE t PAT,  t C, ;

\ A jump table's entries are code addresses living in DATA, which is
\ patch type -3. They are collected rather than emitted as they are
\ found, because c4lc writes every one of them after every code patch
\ and c4r.lisp walks the patch list against the instruction stream.
8192 CONSTANT TABMAX
CREATE TABD TABMAX CELLS ALLOT
CREATE TABC TABMAX CELLS ALLOT          \ the value
CREATE TABB TABMAX CELLS ALLOT          \ 1 = the SLOT is in region 2
CREATE TABT TABMAX CELLS ALLOT          \   2 = the VALUE is; +patch type
VARIABLE TABN   0 TABN !
\ Everything a DATA word can refer to. All three live in one list and in
\ the order they were met, because that is one list in c4lc and the
\ order is visible in the image.
: (TABPAT) ( slot value flags type -- )
   TABN @ TABMAX < 0= IF ." c4fc: too many data patches" CR ABORT THEN
   TABT TABN @ CELLS + !   TABB TABN @ CELLS + !
   TABC TABN @ CELLS + !   TABD TABN @ CELLS + !   1 TABN +! ;
\ a switch jump table: a region 2 slot holding a code address
: TABPAT, ( slot code -- )   DUP LABEL@  1 -3 (TABPAT) ;
\ `int *fp = &fn;`: a region 1 slot holding a code address
: FNPAT,  ( slot code -- )   0 -3 (TABPAT) ;
\ `char *s = "...";`: a region 1 slot holding the address of a string
\ that is ALSO in region 1 -- see ID-STR, below for why.
: STRPAT, ( slot off -- )    0 -4 (TABPAT) ;
\ The optimizer moves code, so a data word holding a CODE address has to
\ be remapped when it does. It sets TAB-MAP; nothing else does, and the
\ two paths are otherwise the same loop rather than two copies of it.
: TAB-SAME ( a -- a ) ;
DEFER TAB-MAP ( codeaddr -- codeaddr' )
' TAB-SAME IS TAB-MAP
: EMIT-TABPATS ( base -- ) {: b | f ty -- :}
   TABN @ 0 ?DO
      TABB I CELLS + @ TO f   TABT I CELLS + @ TO ty
      TABD I CELLS + @  f 1 AND IF b + THEN
      TABC I CELLS + @  ty -3 = IF TAB-MAP ELSE f 2 AND IF b + THEN THEN
      ty  ROT ROT PAT,
   LOOP ;

\ A call to a function that has only been PROTOTYPED does not know where
\ it will land, so every call to a user function is recorded and fixed
\ when the whole program has been read. Both halves need fixing: the
\ patch's value and the code word, because a code reference carries its
\ target in both.
32768 CONSTANT FWMAX
CREATE FWP FWMAX CELLS ALLOT
CREATE FWS FWMAX CELLS ALLOT
VARIABLE FWN   0 FWN !
\ A reference to a function by name, which may not have been compiled
\ yet: the patch is recorded now with a placeholder and FIX-FORWARDS
\ fills in the address once the whole unit is parsed. JSR calls it; IMM
\ takes its address, and a threaded Forth's dictionary is nothing but a
\ table of those.
: FREF, ( op sym -- ) {: op y -- :}
   op OP,  -1 CHERE 0 PAT,  0 C,
   PN @ 1- FWP FWN @ CELLS + !   y FWS FWN @ CELLS + !   1 FWN +! ;
: JSRF, ( sym -- )  oJSR SWAP FREF, ;
\ Branches carry a code patch exactly as calls do, so a forward branch
\ has two things to fill in later -- the patch's value and the code word
\ -- and what it carries around meanwhile is its PATCH index.
: BR, ( op -- mark ) {: op -- m :}
   op OP,  -1 CHERE 0 PAT,  0 C,  PN @ 1- ;
: RESTO ( mark target -- ) {: m t | p -- :}
   m 3 * CELLS PATCH @ + TO p
   t p 2 CELLS + !
   t p CELL+ @ CELLS CODE @ + !
   t LABEL@ ;
: >RES ( mark -- )  CHERE RESTO ;
: BACK, ( op target -- ) {: op t -- :}
   op OP,  -1 CHERE t PAT,  t C,  t LABEL@ ;

\ Region 1: an initialised global's address IS its offset, because that
\ region starts at zero.
\ The data buffers grow ZEROED, because a partially-initialised array
\ leaves the rest of itself to be read as zeros. Neither of these two
\ had a bounds check at all before -- they simply bumped the cursor past
\ the end of the block and wrote there.
: DGROW ( addrvar capvar -- ) {: av cv | old -- :}
   cv @ TO old
   av cv 1 GROW
   av @ old +   cv @ old -   0 FILL ;
: ID-ALLOT ( n -- off ) {: n | off -- off :}
   IDN @ 1 CELLS 1- + 1 CELLS 1- INVERT AND IDN !
   IDN @ TO off
   BEGIN off n + IMAX @ > WHILE IDATA IMAX DGROW REPEAT
   n IDN +!  off ;
\ A string a GLOBAL INITIALISER needs is allocated here rather than with
\ the other literals, because c4lc lays the data out in one pass over
\ the declarations and the string is met while that global is: the
\ pointer's own word comes AFTER the bytes it points at.
: ID-STR, ( a u -- off ) {: a u | off -- off :}
   u 1+ ID-ALLOT TO off
   a  IDATA @ off +  u MOVE
   off ;
: ID-C! ( c off -- )  IDATA @ + C! ;
: ID-! ( v off -- )   IDATA @ + ! ;

\ A global's address is not known until every string literal has been
\ seen, because c4lc lays the globals out AFTER them -- so the patch is
\ recorded with the global's number and revisited at the end.
8192 CONSTANT GPMAX
CREATE GPL GPMAX CELLS ALLOT   VARIABLE GPN   0 GPN !
: FIX-REGION2 ( base -- ) {: b | p -- :}   \ shift every region 2 reference
   DSN @ 0 ?DO
      DSL I CELLS + @ 3 * CELLS PATCH @ + 2 CELLS + TO p
      p @ b + p !
   LOOP ;
: GOP, ( op slot -- ) {: op slot -- :}
   op OP,  -2 CHERE slot PAT,  0 C,
   PN @ 1- GPL GPN @ CELLS + !  1 GPN +! ;
: IMMG, ( slot -- )  oIMM SWAP GOP, ;

\ -- externs, and object mode -------------------------------------------
\ A .c4o names what it could not resolve. An unresolved reference is an
\ opcode whose operand word is zero and whose patch carries the SYMBOL
\ ID in the type field -- where a whole-program image would carry -1 or
\ -2 -- and the symbol itself sits at the end of the section with
\ ATTR_EXTERN and a value of zero, for c4rlink to fill in.
\
\ The id is not known until every DEFINED symbol has been numbered, so
\ the patch is written with the type -1000-k and rewritten at the end.
\ That placeholder is out of range of every real type, which is what
\ lets the peephole passes carry it through unharmed.

VARIABLE OBJECT   0 OBJECT !

512 CONSTANT EXTMAX
CREATE EXT-A EXTMAX CELLS ALLOT         \ name
CREATE EXT-U EXTMAX CELLS ALLOT
CREATE EXT-T EXTMAX CELLS ALLOT         \ C type
CREATE EXT-C EXTMAX CELLS ALLOT         \ 129 a function, 131 data
CREATE EXT-V EXTMAX CELLS ALLOT         \ variadic
CREATE EXT-G EXTMAX CELLS ALLOT         \ array bytes, 0 if not an array
VARIABLE EXTN   0 EXTN !

: EXT-FIND ( a u -- k|-1 ) {: a u -- k :}
   EXTN @ 0 ?DO
      u I CELLS EXT-U + @ = IF
         a  I CELLS EXT-A + @  u BYTES= IF I UNLOOP EXIT THEN
      THEN
   LOOP -1 ;
: EXT-NEW ( a u ct class va agg -- k ) {: a u ct c va ag | k -- k :}
   EXTN @ EXTMAX < 0= IF ." c4fc: too many externs" CR ABORT THEN
   EXTN @ TO k
   a k CELLS EXT-A + !   u k CELLS EXT-U + !   ct k CELLS EXT-T + !
   c k CELLS EXT-C + !   va k CELLS EXT-V + !  ag k CELLS EXT-G + !
   1 EXTN +!  k ;

: EXT-TYPE ( k -- t )  -1000 SWAP - ;    \ the placeholder patch type
: EXTREF, ( op k -- ) {: op k -- :}
   op OP,   k EXT-TYPE CHERE 0 PAT,   0 C, ;
: FIX-EXTERNS ( base -- ) {: b | p -- :}
   PN @ 0 ?DO
      I 3 * CELLS PATCH @ + TO p
      p @ -1000 <= IF  b -1000 p @ - +  p !  0 p 2 CELLS + !  THEN
   LOOP ;

\ -- constant arithmetic ------------------------------------------------
\ One table, used twice: the TREE pass folds `3 * 4` in the AST and the
\ peephole pass folds `IMM 3; PSH; IMM 4; MUL` in the instruction
\ stream. They must agree, and the way to make them agree is to have
\ one of them. Division by zero never folds -- it is left to fail at
\ runtime, where C says it may.
: ASHR ( a b -- v )  {: a b -- v :}
   a 0< IF a INVERT b RSHIFT INVERT ELSE a b RSHIFT THEN ;
: FOLD1 ( a b op -- v ok ) {: a b o -- v ok :}
   o oADD = IF a b +      1 EXIT THEN
   o oSUB = IF a b -      1 EXIT THEN
   o oMUL = IF a b *      1 EXIT THEN
   o oDIV = IF b 0= IF 0 0 EXIT THEN a b /   1 EXIT THEN
   o oMOD = IF b 0= IF 0 0 EXIT THEN a b MOD 1 EXIT THEN
   o oAND = IF a b AND    1 EXIT THEN
   o oOR  = IF a b OR     1 EXIT THEN
   o oXOR = IF a b XOR    1 EXIT THEN
   o oSHL = IF a b LSHIFT 1 EXIT THEN
   o oSHR = IF a b ASHR   1 EXIT THEN
   o oEQ  = IF a b =  1 AND 1 EXIT THEN
   o oNE  = IF a b <> 1 AND 1 EXIT THEN
   o oLT  = IF a b <  1 AND 1 EXIT THEN
   o oGT  = IF a b >  1 AND 1 EXIT THEN
   o oLE  = IF a b <= 1 AND 1 EXIT THEN
   o oGE  = IF a b >= 1 AND 1 EXIT THEN
   0 0 ;

\ -- data ---------------------------------------------------------------

: D-ALIGN  DN @ 1 CELLS 1- + 1 CELLS 1- INVERT AND DN ! ;
: D-ALLOT ( n -- off ) {: n | off -- off :}
   DN @ TO off
   BEGIN off n + DMAX @ > WHILE DATA DMAX DGROW REPEAT
   n DN +!  off ;
: D-STR, ( a u -- off ) {: a u | off -- off :}
   DN @ TO off
   a  DATA @ off +  u MOVE
   u 1+ DN +!                           \ the nul is part of the string
   off ;

: FIX-GLOBALS ( map -- ) {: map | p -- :}
   GPN @ 0 ?DO
      GPL I CELLS + @ 3 * CELLS PATCH @ + 2 CELLS + TO p
      p @ CELLS map + @  p !
   LOOP ;

\ -- symbols ------------------------------------------------------------

\ y.type and y.class are the .c4r symbol record's fields; y.ct is the C
\ TYPE, which the image format has no room for and the compiler cannot
\ do without -- char and int differ by one opcode at every load and
\ every store.
\ attrs 0x40 marks an AGGREGATE -- an array, or a struct variable. Both
\ are names that stand for an address rather than a value, and c4lc
\ flags them the same way.
64 CONSTANT ATTR-ARRAY
: SYM, ( a u type class val attrs -- ) {: a u t c v at | y -- :}
   SN @ SMAX @ >= IF SYMS SMAX SYMR GROW THEN
   SN @ SYM[] TO y
   a y y.name !  u y y.nlen !  t y y.type !  c y y.class !  v y y.val !
   at y y.agg !
   1 SN +! ;

\ ATTR_EXTERN is 16; 32 marks a variadic function and 64 an aggregate,
\ exactly as a defined symbol carries them.
: EXT-SYMS ( -- ) {: | at -- :}
   EXTN @ 0 ?DO
      16 TO at
      I CELLS EXT-V + @ IF at 32 OR TO at THEN
      I CELLS EXT-G + @ IF at 64 OR TO at THEN
      I CELLS EXT-A + @  I CELLS EXT-U + @
      I CELLS EXT-T + @  I CELLS EXT-C + @  0  at  SYM, 
   LOOP ;

: FIX-FORWARDS {: | p v -- :}
   FWN @ 0 ?DO
      FWP I CELLS + @ 3 * CELLS PATCH @ + TO p
      FWS I CELLS + @ y.val @ TO v
      \ In object mode an undefined callee is an EXTERN and its call site
      \ never got here -- except during the discovery pass, which runs
      \ before the externs exist and whose output is thrown away.
      v 0< IF
         OBJECT @ IF 0 TO v
         ELSE ." c4fc: a function was called but never defined" CR ABORT THEN
      THEN
      v p 2 CELLS + !
      v p CELL+ @ CELLS CODE @ + !
   LOOP ;


\ -- writing it out -----------------------------------------------------

\ Where the image goes. c4fc wrote it to stdout, which is fine for a
\ Makefile with a redirect and useless on C4DOS, where there is no '>'
\ by decision and a tool is expected to write the file itself. OUT-C is
\ the one place that decides; OUT>FILE collects into a buffer and
\ SAVE-BLOCK puts it where it belongs.
VARIABLE OUTBUF   0 OUTBUF !
VARIABLE OUTN     0 OUTN !
VARIABLE OUTMAX   0 OUTMAX !
VARIABLE OUTNAME  0 OUTNAME !   VARIABLE OUTNLEN  0 OUTNLEN !

: OUT>STDOUT ( -- )  0 OUTNAME !  0 OUTNLEN ! ;
: OUT>FILE ( a u -- )  OUTNLEN !  OUTNAME ! ;
: OUT-RESET ( -- )
   0 OUTN !
   OUTNAME @ IF
      OUTBUF @ 0= IF 65536 DUP OUTMAX ! ALLOCATE OUTBUF ! THEN
   THEN ;
: OUT-C ( c -- )
   OUTNAME @ 0= IF EMIT EXIT THEN
   OUTN @ OUTMAX @ >= IF OUTBUF OUTMAX 1 GROW THEN
   OUTBUF @ OUTN @ + C!  1 OUTN +! ;
: OUT-FLUSH ( -- )
   OUTNAME @ 0= IF EXIT THEN
   OUTBUF @ OUTN @  OUTNAME @ OUTNLEN @  SAVE-BLOCK
   0= IF ." c4fc: cannot write " OUTNAME @ OUTNLEN @ TYPE CR ABORT THEN ;

CREATE WSCR 1 CELLS ALLOT
: FW  ( w -- )  WSCR !  1 CELLS 0 ?DO WSCR I + C@ OUT-C LOOP ;
: FMK ( c -- )  OUT-C  1 CELLS 1- 0 ?DO 0 OUT-C LOOP ;
: ALIGNUP ( n -- n )  1 CELLS 1- + 1 CELLS 1- INVERT AND ;
: MEMSZ ( -- n )  DB3 @ UDN @ + ALIGNUP ;
\ The whole segment, assembled: region 1's bytes, then region 2's, then
\ nothing at all -- region 3 is zeros and the loader supplies them.
: DBYTE ( i -- c )
   DUP IDN @ < IF IDATA @ + C@ EXIT THEN
   DUP DB2 @ >= OVER DB2 @ DN @ + < AND IF DB2 @ - DATA @ + C@ EXIT THEN
   DROP 0 ;
: DTRIM ( -- n )
   MEMSZ BEGIN DUP 0> IF DUP 1- DBYTE 0= ELSE 0 THEN WHILE 1- REPEAT ;

: WRITE-IMAGE {: | dl y -- :}
   DTRIM TO dl
   OUT-RESET
   67 OUT-C 52 OUT-C 82 OUT-C            \ "C4R"
   3 OUT-C                               \ version 3, as c4lc writes
   1 CELLS 8 * OUT-C
   \ v3 puts memsz in v2's eight padding bytes -- and the field is EIGHT
   \ BYTES whatever the word size is, so a 32-bit image writes the word
   \ and then four zeros. Writing one word and stopping made a header
   \ four bytes short, which nothing noticed until c4fc ran on a 32-bit
   \ machine: at 64 bits one word IS the field.
   MEMSZ FW   8 1 CELLS - 0 ?DO 0 OUT-C LOOP
   ENTRY @ FW   CN @ FW   dl FW   PN @ FW   SN @ FW   CONSN @ FW   DESN @ FW
   67 FMK  CN @ 0 ?DO I CELLS CODE @ + @ FW LOOP
   68 FMK  dl 0 ?DO I DBYTE OUT-C LOOP
   80 FMK  PN @ 3 * 0 ?DO I CELLS PATCH @ + @ FW LOOP
   99 FMK  CONSN @ 0 ?DO I CELLS CONS @ + @ FW LOOP
  100 FMK  DESN @ 0 ?DO I CELLS DESS @ + @ FW LOOP
   83 FMK
   SN @ 0 ?DO
      I SYM[] TO y
      I FW                               \ id
      y y.type @ FW   y y.class @ FW   y y.agg @ FW
      y y.nlen @ OUT-C
      y y.nlen @ 0 ?DO y y.name @ I + C@ OUT-C LOOP
      y y.val @ FW
   LOOP
   OUT-FLUSH ;

\ The counters that live beside the tables they index. They are here
\ rather than in EMIT-INIT only because EMIT-INIT is defined before the
\ tables are; -O compiles a unit twice and both passes start clean.
: EMIT-RESET ( -- )
   EMIT-INIT
   0 DSN !  0 TABN !  0 FWN !  0 GPN !  -1 LABMAX ! ;
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
\ -mcisc and -mfuse: opcodes c4m does not have. Off by default, because
\ turning them on silently changes which machines an image runs on --
\ -mcisc needs c4mp, and -mfuse takes plain c4 away too.
VARIABLE CISC       0 CISC !
VARIABLE FUSE       0 FUSE !

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
\ c4fc gen.f -- code emission, one method per node kind.
\
\ Nothing here dispatches. A kind reaches a phase by having a method in
\ it, and a kind that cannot be assigned to simply has no GEN-ADDR --
\ which is the whole lvalue rule, enforced by the method table rather
\ than by a flag.

:M GEN n_num   >val @ oIMM OP2, ;M
:M GEN n_str   {: n -- :}  n >off @ n >slen @ D-STR, IMMD, ;M

\ A variable's symbol carries the LEA operand it was given: positive for
\ a parameter, negative for a local. c4 addresses both the same way, so
\ one method covers both and the symbol table is where the difference
\ lives.
: LOAD, ( ct -- )   t_char = IF oLC OP, ELSE oLI OP, THEN ;
: STORE, ( ct -- )  t_char = IF oSC OP, ELSE oSI OP, THEN ;

\ An AGGREGATE -- an array, or a struct variable -- is a name that
\ stands for its own address, so its value IS the LEA and there is no
\ load. That one flag is the whole of array decay.
:M GEN-ADDR n_var  >sym @ y.val @ oLEA OP2, ;M
:M GEN      n_var {: n | y -- :}
   n >sym @ TO y   y y.val @ oLEA OP2,
   y y.agg @ IF EXIT THEN   y y.ct @ LOAD, ;M
\ An initialised global's address is final the moment it is declared,
\ because region 1 begins at zero; every other global is a slot number
\ until the end of the program.
: GADDR, ( y -- ) {: y -- :}
   y y.class @ c_extg = IF oIMM y y.val @ EXTREF, EXIT THEN
   y y.ini @ IF y y.val @ IMMI, ELSE y y.val @ IMMG, THEN ;
\ Calling through a POINTER, which is what a threaded Forth spends its
\ life doing: JSRI through a global, JSRS through a frame slot. Only a
\ name whose class is fun or ext is a JSR to a known address.
: GJSRI, ( y -- ) {: y -- :}
   y y.ini @ IF oJSRI y y.val @ DOP, ELSE oJSRI y y.val @ GOP, THEN ;
:M GEN-ADDR n_gvar >sym @ GADDR, ;M
:M GEN      n_gvar {: n | y -- :}
   n >sym @ TO y   y GADDR,
   y y.agg @ IF EXIT THEN   y y.ct @ LOAD, ;M

: FNADDR, ( y -- ) {: y -- :}
   y y.class @ c_ext = IF oIMM y y.val @ EXTREF, EXIT THEN
   oIMM y FREF, ;
:M GEN      n_fnref  >sym @ FNADDR, ;M
:M GEN-ADDR n_fnref  >sym @ FNADDR, ;M
:M CT       n_fnref  >sym @ y.ct @ ;M

:M GEN      n_cast >expr @ GEN ;M
:M GEN-ADDR n_cast >expr @ GEN-ADDR ;M
:M CT       n_cast >ctype @ ;M

:M CT n_num  DROP t_int ;M
:M CT n_str  DROP 2 ;M                  \ char *
:M CT n_var  >sym @ y.ct @ ;M
:M CT n_gvar >sym @ y.ct @ ;M
:M CT n_bin {: n | lt -- :}
   n >lhs @ CT TO lt
   n >op @ oADD = lt T-PTR? AND IF lt EXIT THEN
   n >op @ oSUB = lt T-PTR? AND IF
      n >rhs @ CT lt = IF t_int EXIT THEN  lt EXIT THEN
   t_int ;M
:M CT n_call >fn @ y.ct @ ;M
:M CT n_asgn >lhs @ CT ;M

\ -mcisc: LXI and SXI fold scale-add-load (or -store) into one opcode,
\ but only for `var[expr]` whose base is a PLAIN VARIABLE of statically
\ known 8-byte scalar element type. Anything more complex -- a member, a
\ dereference, a call -- falls back, because establishing its type here
\ would mean either evaluating it twice or duplicating the inference
\ that codegen already does as a side effect. Deliberately the same
\ conservatism as c4lc's, so the two agree.
: CISC-INDEX? ( n -- f ) {: n | b t -- f :}
   CISC @ 0= IF 0 EXIT THEN
   n >TAG n_index <> IF 0 EXIT THEN
   n >lhs @ TO b
   b >TAG n_var <> b >TAG n_gvar <> AND IF 0 EXIT THEN
   b CT TO t
   t T-PTR? 0= IF 0 EXIT THEN
   t T-DEREF TO t
   t T-STRUCT? IF 0 EXIT THEN
   t t_char <> ;

:M GEN n_asgn {: n -- :}
   \ base and index both stay on the stack, unscaled, until the value is
   \ evaluated -- SXI wants all three, and computing the address first
   \ would need a slot to hold it across the right-hand side.
   n >lhs @ CISC-INDEX? IF
      n >lhs @ >lhs @ GEN  oPSH OP,
      n >lhs @ >rhs @ GEN  oPSH OP,
      n >rhs @ GEN  oSXI OP, EXIT THEN
   n >lhs @ GEN-ADDR  oPSH OP,
   n >rhs @ GEN
   n >lhs @ CT STORE, ;M

\ Pointer arithmetic scales by what the pointer points AT, and a step of
\ one emits no multiply at all -- which is not an optimisation but what
\ c4lc does: char * walks byte by byte with no MUL in sight.
: SCALE, ( t -- )   T-STEP DUP 1 = IF DROP EXIT THEN oPSH OP, oIMM OP2, oMUL OP, ;
: STEP-OF ( t -- n ) DUP T-PTR? IF T-STEP ELSE DROP 1 THEN ;

:M GEN n_bin {: n | lt op -- :}
   n >lhs @ CT TO lt   n >op @ TO op
   op oADD = lt T-PTR? AND IF
      n >lhs @ GEN oPSH OP,  n >rhs @ GEN  lt SCALE,  oADD OP, EXIT THEN
   op oSUB = lt T-PTR? AND IF
      n >rhs @ CT lt = IF                \ pointer minus pointer is a count
         n >lhs @ GEN oPSH OP,  n >rhs @ GEN  oSUB OP,
         lt T-STEP DUP 1 = IF DROP EXIT THEN
         oPSH OP, oIMM OP2, oDIV OP, EXIT THEN
      n >lhs @ GEN oPSH OP,  n >rhs @ GEN  lt SCALE,  oSUB OP, EXIT THEN
   n >lhs @ GEN  oPSH OP,  n >rhs @ GEN  op OP, ;M

:M GEN-ADDR n_index {: n -- :}
   n >lhs @ GEN  oPSH OP,
   n >rhs @ GEN  n >lhs @ CT SCALE,
   oADD OP, ;M
:M GEN n_index {: n -- :}
   n CISC-INDEX? IF
      n >lhs @ GEN  oPSH OP,  n >rhs @ GEN  oLXI OP, EXIT THEN
   n GEN-ADDR  n CT LOAD, ;M
:M CT  n_index  >lhs @ CT T-DEREF ;M

:M GEN-ADDR n_member {: n -- :}
   n >lhs @ GEN
   n >moff @ ?DUP IF oPSH OP, oIMM OP2, oADD OP, THEN ;M
:M GEN n_member {: n -- :}
   n GEN-ADDR
   n >magg @ IF EXIT THEN               \ an array member is its address
   n >mtype @ LOAD, ;M
:M CT  n_member  >mtype @ ;M

\ Arguments push left to right, then the call, then the drop. A builtin
\ is the same shape with an opcode where the JSR goes -- which is why
\ printf needs no special case anywhere else.
\ Calling a variadic function: push everything, push how many of them
\ were EXTRA, call __c4cc_make_va, drop the count and the extras, and
\ push what it returned. The callee sees its fixed parameters plus one
\ more -- which is why `int vsum(int n, ...)` finds n at bp+3 and not
\ bp+2, and why the ... needs no prologue of its own.
\ Every operand is evaluated, in order, and the value is the last one's
\ -- so the type is the last one's too.
:M CT n_comma {: n -- t :}  n >args @ n >argn @ 1- CELLS + @ CT ;M
:M GEN n_comma {: n | v -- :}
   n >args @ TO v
   n >argn @ 0 ?DO I CELLS v + @ GEN LOOP ;M

\ What a call compiles to is decided entirely by the CLASS of the name
\ being called: an opcode for a builtin, JSR to a known address, JSRI
\ through a global, JSRS through a frame slot, and an unresolved
\ reference for anything this unit only declares.
: CALL, ( y -- ) {: f -- :}
   f y.class @ c_builtin = IF f y.val @ OP, EXIT THEN
   f y.class @ c_glo     = IF f GJSRI, EXIT THEN
   f y.class @ c_loc     = IF f y.val @ oJSRS OP2, EXIT THEN
   f y.class @ c_ext     = IF oJSR  f y.val @ EXTREF, EXIT THEN
   f y.class @ c_extg    = IF oJSRI f y.val @ EXTREF, EXIT THEN
   f JSRF, ;

:M GEN n_call {: n | f k -- :}
   n >fn @ TO f
   n >argn @ TO k
   k 0 ?DO  n >args @ I CELLS + @ GEN  oPSH OP,  LOOP
   f y.va @ IF
      k f y.nfix @ - TO k                \ how many were extra
      k oIMM OP2,  oPSH OP,
      VA-MAKE @ 0= IF
         ." c4fc: a variadic call needs __c4cc_make_va -- include stdarg.h"
         CR ABORT THEN
      VA-MAKE @ CALL,
      k 1+ oADJ OP2,
      oPSH OP,
      f CALL,
      f y.nfix @ 1+ oADJ OP2,
      EXIT
   THEN
   f CALL,
   k ?DUP IF oADJ OP2, THEN ;M

\ A scalar takes SI even when it is a char, which is what c4cc did and
\ what c4lc kept; an array is stored element by element, the elements
\ past the initialiser list zeroed.
:M STMT n_linit {: n | y off v c s val -- :}
   n >isym @ TO y   y y.val @ TO off
   n >icount @ TO s
   s 0< IF
      off oLEA OP2,  oPSH OP,  n >expr @ GEN  oSI OP,  EXIT THEN
   n >ivals @ TO v   n >ivn @ TO c
   s 0 ?DO
      I c < IF I CELLS v + @ ELSE 0 THEN TO val
      n >ibyte @ IF
         off oLEA OP2,  oPSH OP,  I oIMM OP2,  oADD OP,  oPSH OP,
         val oIMM OP2,  oSC OP,
      ELSE
         off I + oLEA OP2,  oPSH OP,  val oIMM OP2,  oSI OP,
      THEN
   LOOP ;M

:M STMT n_expst  >expr @ GEN ;M
:M STMT n_ret    >expr @ ?DUP IF GEN THEN  oLEV OP, ;M
:M STMT n_blk {: n -- :}
   n >len @ 0 ?DO n >list @ I CELLS + @ STMT LOOP ;M

\ -- unary --------------------------------------------------------------
\ Each is what c4 emits, and each is worth reading once: ! is a compare
\ against zero, ~ is an XOR with -1, and unary minus is a multiply by -1
\ with the -1 pushed FIRST, because C4's MUL takes its left operand off
\ the stack.

:M GEN n_not   >opnd @ GEN  oPSH OP,  0 oIMM OP2,  oEQ  OP, ;M
:M GEN n_bnot  >opnd @ GEN  oPSH OP, -1 oIMM OP2,  oXOR OP, ;M
:M GEN n_neg   -1 oIMM OP2, oPSH OP,  >opnd @ GEN  oMUL OP, ;M

\ A pointer's VALUE is the address it points at, so dereferencing for an
\ address is the operand's value and nothing else.
:M GEN-ADDR n_deref  >opnd @ GEN ;M
:M GEN      n_deref {: n -- :}  n >opnd @ GEN  n CT LOAD, ;M
:M GEN      n_addr   >opnd @ GEN-ADDR ;M

:M CT n_not   DROP t_int ;M
:M CT n_bnot  DROP t_int ;M
:M CT n_neg   DROP t_int ;M
:M CT n_addr  >opnd @ CT 2 + ;M
:M CT n_deref >opnd @ CT 2 - ;M

\ ++ and -- reuse one address for both the load and the store, which is
\ why the sequence is LEA, PSH, LI rather than LEA, LI, PSH. The postfix
\ forms undo the change on the RESULT afterwards, which is exactly how
\ c4 gets the old value without a temporary.
: INCDEC, ( node op -- ) {: n op | t -- :}
   n >opnd @ CT TO t
   n >opnd @ GEN-ADDR  oPSH OP,
   t LOAD,
   oPSH OP,  t STEP-OF oIMM OP2,  op OP,
   t STORE, ;
:M GEN n_preinc   oADD INCDEC, ;M
:M GEN n_predec   oSUB INCDEC, ;M
:M GEN n_postinc {: n -- :}
   n oADD INCDEC,  oPSH OP, n >opnd @ CT STEP-OF oIMM OP2, oSUB OP, ;M
:M GEN n_postdec {: n -- :}
   n oSUB INCDEC,  oPSH OP, n >opnd @ CT STEP-OF oIMM OP2, oADD OP, ;M
:M CT n_preinc   >opnd @ CT ;M
:M CT n_predec   >opnd @ CT ;M
:M CT n_postinc  >opnd @ CT ;M
:M CT n_postdec  >opnd @ CT ;M

\ -- short-circuit and the conditional ---------------------------------

:M GEN n_lor {: n | m -- :}
   n >lhs @ GEN   oBNZ BR, TO m   n >rhs @ GEN   m >RES ;M
:M GEN n_land {: n | m -- :}
   n >lhs @ GEN   oBZ  BR, TO m   n >rhs @ GEN   m >RES ;M
:M GEN n_cond {: n | m1 m2 -- :}
   n >cond @ GEN   oBZ BR, TO m1
   n >body @ GEN   oJMP BR, TO m2
   m1 >RES
   n >else @ GEN
   m2 >RES ;M
:M CT n_lor  DROP t_int ;M
:M CT n_land DROP t_int ;M
:M CT n_cond >body @ CT ;M

\ -- statements ---------------------------------------------------------
\ break and continue are forward branches recorded on two stacks and
\ resolved when the loop that owns them closes. continue is forward even
\ in a while loop, where its target is behind it -- a mark is a hole to
\ fill, and filling it with an address already known is the same work.

1024 CONSTANT MMAX
CREATE BRKM MMAX CELLS ALLOT   VARIABLE BRKN   0 BRKN !
CREATE CNTM MMAX CELLS ALLOT   VARIABLE CNTN   0 CNTN !
: BRK, ( -- )  oJMP BR, BRKM BRKN @ CELLS + !  1 BRKN +! ;
: CNT, ( -- )  oJMP BR, CNTM CNTN @ CELLS + !  1 CNTN +! ;
: RESOLVE-LOOP ( brkbase cntbase brktarget cnttarget -- ) {: bb cb bt ct -- :}
   BRKN @ bb ?DO BRKM I CELLS + @ bt RESTO LOOP   bb BRKN !
   CNTN @ cb ?DO CNTM I CELLS + @ ct RESTO LOOP   cb CNTN ! ;

:M STMT n_break  DROP BRK, ;M
:M STMT n_cont   DROP CNT, ;M
:M STMT n_empty  DROP ;M

:M STMT n_if {: n | m1 m2 -- :}
   n >cond @ GEN   oBZ BR, TO m1
   n >body @ STMT
   n >else @ IF
      oJMP BR, TO m2   m1 >RES   n >else @ STMT   m2 >RES
   ELSE
      m1 >RES
   THEN ;M

:M STMT n_while {: n | top m bb cb -- :}
   BRKN @ TO bb   CNTN @ TO cb
   CHERE TO top
   n >cond @ GEN   oBZ BR, TO m
   n >body @ STMT
   oJMP top BACK,
   m >RES
   bb cb CHERE top RESOLVE-LOOP ;M

:M STMT n_do {: n | top m bb cb cont -- :}
   BRKN @ TO bb   CNTN @ TO cb
   CHERE TO top
   n >body @ STMT
   CHERE TO cont
   n >cond @ GEN
   oBNZ top BACK,
   bb cb CHERE cont RESOLVE-LOOP ;M

\ continue in a for loop goes to the STEP, not to the condition, and the
\ step is emitted after the body -- so it is a genuine forward branch.
:M STMT n_for {: n | top m bb cb cont -- :}
   BRKN @ TO bb   CNTN @ TO cb
   n >init @ ?DUP IF GEN THEN
   CHERE TO top
   -1 TO m                              \ patch 0 is a real patch index
   n >cond @ ?DUP IF GEN  oBZ BR, TO m THEN
   n >body @ STMT
   CHERE TO cont
   n >step @ ?DUP IF GEN THEN
   oJMP top BACK,
   m 0< 0= IF m >RES THEN
   bb cb CHERE cont RESOLVE-LOOP ;M

\ -- switch -------------------------------------------------------------
\ A jump table, not a chain of compares, because that is what c4lc emits:
\
\    <expr>                     JMP dispatch
\    <case bodies, each labelled where it starts>
\    JMP end                    -- the fall-out of the last case
\  dispatch:
\    PSH IMM lo SUB             -- index = value - lowest case
\    PSH PSH PSH                -- three copies: two compares and the index
\    IMM hi-lo GT  BNZ oob1
\    IMM 0     LT  BNZ oob2
\    IMM 8 MUL PSH IMM table ADD LI JMPA
\  oob1: ADJ 2 JMP default      -- each path drops what its compare left
\  oob2: ADJ 1 JMP default
\  end:
\
\ Entries with no case of their own hold the default target, which is the
\ end when there is no default at all.

BEGIN-STRUCTURE SWC
   FIELD: w.tab  FIELD: w.lo  FIELD: w.hi  FIELD: w.def  FIELD: w.ent
END-STRUCTURE
VARIABLE CURSW   0 CURSW !

:M STMT n_case {: n | w i -- :}
   CURSW @ TO w
   w 0= IF ." c4fc: case outside a switch" CR ABORT THEN
   n >val @ w w.lo @ - TO i
   i 0< i w w.hi @ w w.lo @ - > OR IF
      ." c4fc: case " n >val @ .N ." is outside its switch's range " CR ABORT THEN
   CHERE  w w.ent @  i CELLS +  ! ;M
:M STMT n_default {: n | w -- :}
   CURSW @ TO w
   w 0= IF ." c4fc: default outside a switch" CR ABORT THEN
   CHERE w w.def ! ;M

:M STMT n_switch {: n | w save m e bb m1 m2 m3 m4 end def k -- :}
   CURSW @ TO save
   SWC ALLOT: TO w
   n >lo @ w w.lo !  n >hi @ w w.hi !  -1 w w.def !
   n >hi @ n >lo @ - 1+ TO k
   k CELLS ALLOT: w w.ent !
   k 0 ?DO -1 w w.ent @ I CELLS + ! LOOP
   w CURSW !
   BRKN @ TO bb
   n >cond @ GEN
   oJMP BR, TO m
   n >body @ STMT
   oJMP BR, TO e                        \ the last case falls out here
   m >RES
   \ The table is allocated HERE, after the body and before the dispatch
   \ code, which is where c4lc puts it: a string literal inside the
   \ switch gets the lower address.
   D-ALIGN  k CELLS D-ALLOT w w.tab !
   \ Subtracting the lowest case is skipped when it is zero -- four
   \ words c4lc does not spend, and a difference invisible until a
   \ switch happens to start at case 0.
   n >lo @ ?DUP IF oPSH OP, oIMM OP2, oSUB OP, THEN
   oPSH OP,  oPSH OP,  oPSH OP,
   k 1- oIMM OP2,  oGT OP,   oBNZ BR, TO m1
   0 oIMM OP2,     oLT OP,   oBNZ BR, TO m2
   1 CELLS oIMM OP2,  oMUL OP,
   oPSH OP,  w w.tab @ IMMD,  oADD OP,
   oLI OP,   oJMPA OP,
   m1 >RES  2 oADJ OP2,  oJMP BR, TO m3
   m2 >RES  1 oADJ OP2,  oJMP BR, TO m4
   CHERE TO end
   e >RES
   w w.def @ 0< IF end ELSE w w.def @ THEN TO def
   m3 def RESTO   m4 def RESTO
   k 0 ?DO
      w w.tab @ I CELLS +
      w w.ent @ I CELLS + @ DUP 0< IF DROP def THEN
      TABPAT,
   LOOP
   BRKN @ bb ?DO BRKM I CELLS + @ end RESTO LOOP   bb BRKN !
   save CURSW ! ;M
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
: LOCAL-DECLS ( v -- ) {: v | ct dt t a u sz ag na iv ivn y nd nn -- :}
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
            nn v V,
         THEN
         TK Comma = WHILE TNEXT
      REPEAT
      Semi WANT
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
\ c4fc opt.f -- the peephole optimizer, matching c4opt.lisp.
\
\ The passes cannot work on a finished image: deleting one instruction
\ moves every address after it. So this decodes the image into the
\ LABELLED form c4opt works on -- branch targets as label ids rather
\ than addresses -- optimises that, and assembles it again.
\
\ Decoding is c4r.lisp's reconstruction, and it has to be exactly that,
\ because a different set of labels is a different program shape. A code
\ offset becomes a label if anything points at it: the entry, the value
\ of a -1 patch, the value of a -3 patch (a switch table entry names
\ code from data), a constructor or destructor entry, or a defined
\ function's symbol.
\
\ Round-tripping with no passes at all must reproduce the image byte for
\ byte, and that is checked before any pass is trusted.

\ Sized from the code, not guessed. By the time the optimizer runs, CN
\ is final and every instruction is at least one word, so the item count
\ starts below CN; the passes then remove far more than the one item
\ PASS-SHL ever adds. Twice the code length plus a floor is generous and
\ it is PROPORTIONAL, which the old fixed quarter-million was not -- on
\ a 32-bit machine that was 33 MB of item buffers for a hello world.
VARIABLE MAXI
VARIABLE IBUF  VARIABLE IN#
VARIABLE JBUF  VARIABLE JN#
VARIABLE LMAP                           \ label id -> address, when encoding
VARIABLE ISLBL                          \ code offset -> is it a label
VARIABLE MAXLBL

\ item: kind, op, arg, argkind
0 CONSTANT k_insn   1 CONSTANT k_label
0 CONSTANT a_none   1 CONSTANT a_plain  2 CONSTANT a_code  3 CONSTANT a_data
\ An unresolved reference: the argument is the placeholder patch type
\ itself, carried through untouched -- the symbol it names has no
\ address in this unit, so nothing here can move it.
4 CONSTANT a_ext

VARIABLE PMAP                           \ code address -> its patch, or 0
\ The pass working sets, allocated with everything else: FT for thread,
\ EMM/SMM/BANM for tail.
VARIABLE FT   VARIABLE EMM   VARIABLE SMM   VARIABLE BANM
: I[] ( buf i -- a )  4 CELLS * + ;
: I.K ( a -- a )  ;
: I.O ( a -- a )  1 CELLS + ;
: I.A ( a -- a )  2 CELLS + ;
: I.T ( a -- a )  3 CELLS + ;

: OPT-INIT
   CN @ 2 * 4096 + MAXI !
   MAXI @ 4 CELLS * ALLOCATE IBUF !
   MAXI @ 4 CELLS * ALLOCATE JBUF !
   MAXI @ CELLS ALLOCATE LMAP !
   MAXI @ CELLS ALLOCATE FT !
   MAXI @ CELLS ALLOCATE EMM !
   MAXI @ CELLS ALLOCATE SMM !
   MAXI @ ALLOCATE BANM !
   CN @ 1+ ALLOCATE ISLBL !
   CN @ 1+ CELLS ALLOCATE PMAP ! ;

: ITEM, ( buf n kind op arg at -- n' ) {: b n k o a t | p -- n :}
   n MAXI @ < 0= IF ." c4fc: too many instructions to optimise" CR ABORT THEN
   b n I[] TO p
   k p I.K !   o p I.O !   a p I.A !   t p I.T !
   n 1+ ;

\ -- decoding ------------------------------------------------------------

: MARK-LABEL ( off -- )  ISLBL @ + 1 SWAP C! ;
: LABEL? ( off -- f )    ISLBL @ + C@ 0<> ;

: MARK-LABELS {: | y p -- :}
   ISLBL @ CN @ 1+ 0 FILL
   ENTRY @ 0< 0= IF ENTRY @ MARK-LABEL THEN
   PN @ 0 ?DO
      I 3 * CELLS PATCH @ + TO p
      p @ -1 = p @ -3 = OR IF p 2 CELLS + @ MARK-LABEL THEN
   LOOP
   CONSN @ 0 ?DO I CELLS CONS @ + @ MARK-LABEL LOOP
   DESN  @ 0 ?DO I CELLS DESS @ + @ MARK-LABEL LOOP
   SN @ 0 ?DO
      I SYM[] TO y
      y y.class @ 129 = IF y y.val @ MARK-LABEL THEN
   LOOP ;

\ The patch aimed at this word, or 0.
\
\ This was a scan of the whole patch list per instruction, which is a
\ product: twenty-five thousand instructions against two and a half
\ thousand patches is sixty million comparisons, and it cost more than
\ every other phase of -O put together. One walk of the patch list fills
\ a map indexed by code address instead, and the question becomes a
\ fetch. Only CODE-RESIDENT patches go in it; the data-resident ones
\ address the data segment and would collide.
: PMAP-BUILD ( -- ) {: | p -- :}
   PMAP @ CN @ 1+ CELLS 0 FILL
   PN @ 0 ?DO
      I 3 * CELLS PATCH @ + TO p
      p @ -1 = p @ -2 = OR p @ -1000 <= OR IF
         p  p CELL+ @ CELLS PMAP @ + !
      THEN
   LOOP ;
: PATCH-AT ( off -- p|0 )  CELLS PMAP @ + @ ;

\ LEA..ADJ carry an operand, and so do JSRI and JSRS -- which c4fc did
\ not emit until it had to compile a call through a function pointer.
\ Getting this list wrong does not corrupt the code, because the operand
\ word round-trips as if it were an instruction; it silently drops the
\ PATCH on it, and the call then goes to wherever address zero is.
: HAS-OPERAND? ( op -- f ) {: o -- f :}
   o 0< IF 0 EXIT THEN
   o 7 <= IF -1 EXIT THEN
   o oJSRI = o oJSRS = OR ;

: DECODE {: | i n w p op -- :}
   MARK-LABELS
   PMAP-BUILD
   0 TO n   0 TO i
   BEGIN i CN @ < WHILE
      i LABEL? IF IBUF @ n k_label i 0 a_none ITEM, TO n THEN
      i CELLS CODE @ + @ TO w
      w HAS-OPERAND?  i 1+ CN @ < AND  i 1+ LABEL? 0= AND IF
         i 1+ PATCH-AT TO p
         p IF
            p @ -1 = IF IBUF @ n k_insn w  p 2 CELLS + @ a_code ITEM, TO n ELSE
            p @ -1000 <= IF IBUF @ n k_insn w  p @ a_ext ITEM, TO n
                     ELSE IBUF @ n k_insn w  p 2 CELLS + @ a_data ITEM, TO n THEN THEN
         ELSE
            IBUF @ n k_insn w  i 1+ CELLS CODE @ + @  a_plain ITEM, TO n
         THEN
         i 2 + TO i
      ELSE
         IBUF @ n k_insn w 0 a_none ITEM, TO n
         i 1+ TO i
      THEN
   REPEAT
   n IN# !
   CN @ MAXLBL ! ;

\ -- encoding ------------------------------------------------------------

: ASSIGN-ADDRESSES {: | i n a p -- :}
   LMAP @ MAXI @ CELLS 0 FILL
   0 TO a
   IN# @ 0 ?DO
      IBUF @ I I[] TO p
      p I.K @ k_label = IF a  p I.O @ CELLS LMAP @ + !
                        ELSE a p I.T @ a_none = IF 1 ELSE 2 THEN + TO a THEN
   LOOP ;
: LADDR ( labelid -- addr )  CELLS LMAP @ + @ ;

: ENCODE {: | p y -- :}
   ASSIGN-ADDRESSES
   0 CN !   0 PN !
   IN# @ 0 ?DO
      IBUF @ I I[] TO p
      p I.K @ k_insn = IF
         p I.T @ a_none = IF p I.O @ C, ELSE
         p I.T @ a_plain = IF p I.A @ p I.O @ OP2, ELSE
         p I.T @ a_code = IF
            p I.O @ OP,  -1 CHERE p I.A @ LADDR PAT,  p I.A @ LADDR C,
         ELSE
         p I.T @ a_ext = IF
            p I.O @ OP,  p I.A @ CHERE 0 PAT,  0 C,
         ELSE
            p I.O @ OP,  -2 CHERE p I.A @ PAT,  0 C,
         THEN THEN THEN THEN
      THEN
   LOOP
   ENTRY @ 0< 0= IF ENTRY @ LADDR ENTRY ! THEN
   CONSN @ 0 ?DO I CELLS CONS @ + DUP @ LADDR SWAP ! LOOP
   DESN  @ 0 ?DO I CELLS DESS @ + DUP @ LADDR SWAP ! LOOP
   SN @ 0 ?DO
      I SYM[] TO y
      y y.class @ 129 = IF y y.val @ LADDR y y.val ! THEN
   LOOP
   \ the data-resident patches come last, as they did before -- jump
   \ tables, `int *fp = &fn;` and `char *s = "..."` alike, with the code
   \ addresses among them remapped through LADDR
   ['] LADDR IS TAB-MAP
   DB2 @ EMIT-TABPATS
   ['] TAB-SAME IS TAB-MAP ;

\ -- reading the item list ----------------------------------------------

: SWAP-BUFS  IBUF @ JBUF @ IBUF ! JBUF !  JN# @ IN# ! ;
: EMIT-ITEM ( kind op arg at -- )  {: k o a t -- :}
   JBUF @ JN# @ k o a t ITEM, JN# ! ;
: COPY-ITEM ( i -- ) {: i | p -- :}
   IBUF @ i I[] TO p
   p I.K @ p I.O @ p I.A @ p I.T @ EMIT-ITEM ;
: IK ( i -- k )   IBUF @ SWAP I[] I.K @ ;
: IOP ( i -- op ) IBUF @ SWAP I[] I.O @ ;
: IARG ( i -- a ) IBUF @ SWAP I[] I.A @ ;
: ITY ( i -- t )  IBUF @ SWAP I[] I.T @ ;
: LBL? ( i -- f )  IK k_label = ;
: BARE? ( i op -- f ) {: i o -- f :}          \ this opcode, no operand
   i LBL? IF 0 EXIT THEN
   i IOP o = i ITY a_none = AND ;
: PLAINOP? ( i op -- f ) {: i o -- f :}       \ this opcode, plain operand
   i LBL? IF 0 EXIT THEN
   i IOP o = i ITY a_plain = AND ;
: CODEOP? ( i op -- f ) {: i o -- f :}        \ this opcode, code target
   i LBL? IF 0 EXIT THEN
   i IOP o = i ITY a_code = AND ;
: ANYBARE? ( i -- f )                          \ any opcode with no operand
   DUP LBL? IF DROP 0 EXIT THEN  ITY a_none = ;

\ -- fold ---------------------------------------------------------------
\ The window is IMM a; PSH; IMM b; OP, and the stack operand is on the
\ LEFT, so the value is a OP b. A folded IMM is fed back in: constants
\ cascade. FOLD1 itself lives in emit.f, because the TREE pass folds
\ the same operators on the same rules before any of this runs.

: PASS-FOLD {: | i j pv go ok v -- :}
   0 JN# !   0 TO i
   BEGIN i IN# @ < WHILE
      i oIMM PLAINOP? 0= IF
         i COPY-ITEM   i 1+ TO i
      ELSE
         i IARG TO pv   i 1+ TO j   1 TO go
         BEGIN
            go
            j 2 + IN# @ < AND
            j oPSH BARE? AND
            j 1+ oIMM PLAINOP? AND
            j 2 + ANYBARE? AND
         WHILE
            pv  j 1+ IARG  j 2 + IOP  FOLD1 TO ok TO v
            ok IF v TO pv  j 3 + TO j  ELSE 0 TO go THEN
         REPEAT
         k_insn oIMM pv a_plain EMIT-ITEM
         j TO i
      THEN
   REPEAT
   SWAP-BUFS ;

\ -- shl ----------------------------------------------------------------
\ PSH; IMM 8; MUL -> PSH; IMM 3; SHL, which every pointer subscript emits.
\
\ EIGHT, not `1 CELLS`. Matching the host's word size looks more correct
\ and is a wrong-code bug: c4opt's rule fires only on a multiply by 8
\ and emits a shift by 3, so on a 32-bit machine -- where a subscript
\ scales by 4 -- it simply does not fire and c4lc leaves the MUL alone.
\ c4fc matched on `1 CELLS` and still emitted the hardcoded 3, which on
\ c4bb turned every `p[i]` into a multiply by EIGHT. Found by compiling
\ a C4IX module on the breadboard and diffing the object against c4lc's.
\
\ So this is deliberately the same 64-bit-only rule c4opt has, because
\ byte-identity with c4lc is the bar at both word sizes. That c4opt
\ leaves a free shift on the table at 32 bits is true, and is c4opt's to
\ take up if anyone wants it.

: PASS-SHL {: | i -- :}
   0 JN# !   0 TO i
   BEGIN i IN# @ < WHILE
      i oPSH BARE?
      i 2 + IN# @ < AND
      i 1+ oIMM PLAINOP? AND
      IF i 1+ IARG 8 = i 2 + oMUL BARE? AND ELSE 0 THEN
      IF
         k_insn oPSH 0 a_none EMIT-ITEM
         k_insn oIMM 3 a_plain EMIT-ITEM
         k_insn oSHL 0 a_none EMIT-ITEM
         i 3 + TO i
      ELSE
         i COPY-ITEM  i 1+ TO i
      THEN
   REPEAT
   SWAP-BUFS ;

\ -- adj0 ---------------------------------------------------------------

: PASS-ADJ0 {: | i -- :}
   0 JN# !   0 TO i
   BEGIN i IN# @ < WHILE
      i oADJ PLAINOP? IF i IARG 0= ELSE 0 THEN
      0= IF i COPY-ITEM THEN
      i 1+ TO i
   REPEAT
   SWAP-BUFS ;

\ -- jmpnext ------------------------------------------------------------
\ A JMP to a label that the very next items reach by falling through.

: LABEL-FOLLOWS? ( t i -- f ) {: t i -- f :}
   BEGIN i IN# @ < WHILE
      i LBL? 0= IF 0 EXIT THEN
      i IOP t = IF -1 EXIT THEN
      i 1+ TO i
   REPEAT 0 ;
: PASS-JMPNEXT {: | i -- :}
   0 JN# !   0 TO i
   BEGIN i IN# @ < WHILE
      i oJMP CODEOP? IF i IARG i 1+ LABEL-FOLLOWS? ELSE 0 THEN
      0= IF i COPY-ITEM THEN
      i 1+ TO i
   REPEAT
   SWAP-BUFS ;

\ -- thread -------------------------------------------------------------
\ A jump to a label whose only content is JMP L2 can go to L2 directly.
\ One level, and never a self-loop.

: MAX-LABEL ( -- n ) {: | m -- m :}
   0 TO m
   IN# @ 0 ?DO I LBL? IF I IOP m > IF I IOP TO m THEN THEN LOOP
   m ;
: FOLLOWING-JMP ( i -- t|-1 ) {: i -- t :}
   BEGIN i IN# @ < WHILE
      i LBL? 0= IF
         i oJMP CODEOP? IF i IARG EXIT THEN
         -1 EXIT
      THEN
      i 1+ TO i
   REPEAT -1 ;
: PASS-THREAD {: | i t f -- :}

   FT @ MAXI @ CELLS 0 FILL
   IN# @ 0 ?DO
      I LBL? IF
         I 1+ FOLLOWING-JMP TO t
         t 0< 0= IF t 1+ I IOP CELLS FT @ + ! THEN
      THEN
   LOOP
   0 JN# !   0 TO i
   BEGIN i IN# @ < WHILE
      i oJMP CODEOP? i oBZ CODEOP? OR i oBNZ CODEOP? OR IF
         i IARG TO t
         t CELLS FT @ + @ TO f
         f 0> IF f 1- t <> IF
            k_insn i IOP f 1- a_code EMIT-ITEM
            i 1+ TO i
            0
         ELSE 1 THEN ELSE 1 THEN
         IF i COPY-ITEM i 1+ TO i THEN
      ELSE
         i COPY-ITEM  i 1+ TO i
      THEN
   REPEAT
   SWAP-BUFS ;

\ -- dead ---------------------------------------------------------------
\ After JMP, LEV or JMPA nothing runs until a label.

: PASS-DEAD {: | i drop? -- :}
   0 JN# !   0 TO i   0 TO drop?
   BEGIN i IN# @ < WHILE
      i LBL? IF 0 TO drop? THEN
      drop? 0= IF
         i COPY-ITEM
         i oJMP CODEOP? i oLEV BARE? OR i oJMPA BARE? OR IF 1 TO drop? THEN
      THEN
      i 1+ TO i
   REPEAT
   SWAP-BUFS ;

\ -- tail calls ----------------------------------------------------------
\ JSR f; LEV  ->  ADJ (m - k); JMP f+2, where m is the enclosing ENT's
\ operand and k the callee's: the ADJ moves sp from bp-m to bp-k, bp is
\ untouched, so the callee's locals sit in our frame and its LEV pops OUR
\ saved pair and returns to our caller. Tail recursion becomes O(1)
\ stack. Variadic callees are excluded -- their argument slots would be
\ read out of the caller's frame.

: EM[] ( l -- a )  CELLS EMM @ + ;
: SM[] ( l -- a )  CELLS SMM @ + ;
: BAN[] ( l -- a ) BANM @ + ;

: TAIL-SITE ( i -- f|-1 ) {: i | f -- f :}
   i oJSR CODEOP? 0= IF -1 EXIT THEN
   i 1+ IN# @ >= IF -1 EXIT THEN
   i 1+ oLEV BARE? 0= IF -1 EXIT THEN
   i IARG TO f
   f EM[] @ 0= IF -1 EXIT THEN
   f BAN[] C@ 1 = IF -1 EXIT THEN
   f ;
: TAIL-GO ( i -- f|-1 ) {: i | f -- f :}
   i TAIL-SITE TO f
   f 0< IF -1 EXIT THEN
   f SM[] @ 0> IF f ELSE -1 THEN ;

: PASS-TAIL {: | i f maxl nextid curm pending y -- :}
   EMM @ MAXI @ CELLS 0 FILL   SMM @ MAXI @ CELLS 0 FILL   BANM @ MAXI @ 0 FILL
   MAX-LABEL TO maxl
   SN @ 0 ?DO
      I SYM[] TO y
      y y.class @ 129 = IF
         y y.val @ maxl <= IF
            y y.agg @ 32 AND 0<> IF 1 y y.val @ BAN[] C! THEN
         THEN
      THEN
   LOOP
   IN# @ 0 ?DO
      I LBL? IF
         I 1+ IN# @ < IF
            I 1+ oENT PLAINOP? IF I 1+ IARG 1+ I IOP EM[] ! THEN
         THEN
      THEN
   LOOP
   maxl 1+ TO nextid
   IN# @ 0 ?DO
      I TAIL-SITE TO f
      f 0< 0= IF
         f SM[] @ 0= IF  nextid 1+ f SM[] !  nextid 1+ TO nextid  THEN
      THEN
   LOOP
   0 TO curm   0 TO pending
   0 JN# !   0 TO i
   BEGIN i IN# @ < WHILE
      i TAIL-GO TO f
      f 0< 0= curm 0> AND IF
         k_insn oADJ  curm 1- f EM[] @ 1- -  a_plain EMIT-ITEM
         k_insn oJMP  f SM[] @ 1-  a_code EMIT-ITEM
         i 2 + TO i
      ELSE
         i LBL? IF i IOP SM[] @ ?DUP IF TO pending THEN THEN
         i oENT PLAINOP? IF i IARG 1+ TO curm THEN
         i COPY-ITEM
         i oENT PLAINOP? pending 0> AND IF
            k_label pending 1- 0 a_none EMIT-ITEM
            0 TO pending
         THEN
         i 1+ TO i
      THEN
   REPEAT
   SWAP-BUFS ;

\ -- the fixpoint --------------------------------------------------------
\ Threading enables jmpnext which enables dead, and folds cascade, so the
\ passes run until the instruction count stops falling -- bounded at ten
\ rounds. adj0 runs again after tail because a same-size tail transform
\ emits ADJ 0.

: COUNT-INSNS ( -- n ) {: | n -- n :}
   0 TO n
   IN# @ 0 ?DO I LBL? 0= IF n 1+ TO n THEN LOOP
   n ;
\ -- fuse ----------------------------------------------------------------
\ docs/fused-opcodes.md. Two- and three-instruction windows collapsed
\ into one opcode, LONGEST FIRST -- LEA;LI;PSH is PSHL, not LDL then a
\ stray PSH. The fused instruction keeps the operand AND the operand
\ kind of the one it starts with, so an IMM of a data address fuses to a
\ PSHG of the same data address and the patch survives.
\
\ It runs ONCE, after the fixpoint, so that no other pass has to
\ understand the fused forms and so that fusing never hides a fold from
\ the round that would have followed. A label ends a window, which is
\ the same reason every other pass here is safe.

: FUSE2 ( a b -- op ) {: a b -- op :}    \ two-instruction windows, or -1
   b oLI = IF  a oLEA = IF oLDL EXIT THEN  a oIMM = IF oLDG EXIT THEN
               a oADD = IF oADDL EXIT THEN  THEN
   b oPSH = IF  a oLEA = IF oLEAP EXIT THEN  a oIMM = IF oIMMP EXIT THEN
                a oLI  = IF oLIP EXIT THEN  THEN
   -1 ;

: PASS-FUSE {: | i a b c op -- :}
   0 JN# !   0 TO i
   BEGIN i IN# @ < WHILE
      i LBL? IF i COPY-ITEM  i 1+ TO i
      ELSE
         i IOP TO a
         i 1+ IN# @ < IF i 1+ LBL? IF -1 ELSE i 1+ IOP THEN ELSE -1 THEN TO b
         i 2 + IN# @ < IF i 2 + LBL? IF -1 ELSE i 2 + IOP THEN ELSE -1 THEN TO c
         c oPSH = b oLI = AND  a oLEA = a oIMM = OR AND IF
            k_insn  a oLEA = IF oPSHL ELSE oPSHG THEN
            i IARG  i ITY  EMIT-ITEM
            i 3 + TO i
         ELSE
            a b FUSE2 TO op
            op 0< IF i COPY-ITEM  i 1+ TO i
            ELSE
               op oLIP = op oADDL = OR IF
                  k_insn op 0 a_none EMIT-ITEM
               ELSE
                  k_insn op  i IARG  i ITY  EMIT-ITEM
               THEN
               i 2 + TO i
            THEN
         THEN
      THEN
   REPEAT
   SWAP-BUFS ;

: OPT-PASSES
   PASS-FOLD PASS-SHL PASS-ADJ0 PASS-JMPNEXT PASS-THREAD PASS-TAIL
   PASS-ADJ0 PASS-DEAD ;

: OPT-RUN {: | n prev rounds -- :}
   OPT-INIT DECODE
   COUNT-INSNS TO n   -1 TO prev   0 TO rounds
   BEGIN n prev <> rounds 10 < AND WHILE
      OPT-PASSES
      n TO prev   COUNT-INSNS TO n   rounds 1+ TO rounds
   REPEAT
   FUSE @ IF PASS-FUSE THEN
   ENCODE ;
\ c4fc.f -- the driver.

\ c4cc's library table plus c4's intrinsics, in c4lc's order. Every one
\ compiles to its opcode where the call would be, so `read(fd, b, n)` is
\ three pushes and one instruction.
: BUILTIN ( a u op -- )  c_builtin SWAP t_int 0 0 ST, ;
: BUILTINS
   S" open"   oOPEN BUILTIN   S" read"    oREAD BUILTIN
   S" close"  oCLOS BUILTIN   S" printf"  oPRTF BUILTIN
   S" malloc" oMALC BUILTIN   S" free"    oFREE BUILTIN
   S" memset" oMSET BUILTIN   S" memcmp"  oMCMP BUILTIN
   S" exit"   oEXIT BUILTIN   S" putchar" oPUTC BUILTIN
   S" puts"   oPUTS BUILTIN   S" realloc" oRALC BUILTIN
   S" memcpy" oMCPY BUILTIN   S" stacktrace" oSTRC BUILTIN
   S" install_trap_handler" oITH BUILTIN
   S" __opcode" o_OPC BUILTIN  S" __builtin" o_BLT BUILTIN
   S" __c4_trap" o_TRP BUILTIN S" __c4_opcode" oOPCD BUILTIN
   S" __c4_jmp" o_JMP BUILTIN  S" __c4_adjust" o_ADJ BUILTIN
   S" __c4_configure" oC4CF BUILTIN  S" __c4_cycles" oC4CY BUILTIN
   S" __time" oTIME BUILTIN    S" __c4_signal" oSIGH BUILTIN
   S" __c4_sigint" oSIGI BUILTIN     S" __c4_usleep" oUSLP BUILTIN
   S" __c4_info" oINFO BUILTIN S" __c4_ops_list" oOPSL BUILTIN
   S" __c4_invoke" oC4IV BUILTIN     S" __c4_float" oFLT BUILTIN
   S" __c4_cpu_id" oCPUI BUILTIN     S" __c4_cpu_count" oCPUN BUILTIN
   S" __c4_cpu_start" oCPUS BUILTIN  S" __c4_cpu_halt" oCPUH BUILTIN
   S" __c4_cas" oCAS BUILTIN   S" __c4_xchg" oXCHG BUILTIN
   S" __c4_fadd" oFADD BUILTIN S" __c4_wait" oCWAI BUILTIN
   S" __c4_wake" oCWAK BUILTIN S" __c4_ipi" oIPI BUILTIN
   S" __c4_termraw" oTRAW BUILTIN ;

\ The arena has to exist before -I and -D can be recorded, so the setup
\ is its own word and C4FC falls back to it -- which keeps the bare
\ `S" f.c" C4FC` that every spike test uses working unchanged.
\ -P is c4lc's flag and c4lc's default: without it the LEXER skips '#'
\ lines, which is what a source that has already been through gcc -E
\ needs. With it c4fc does the job itself. Keeping the default the same
\ as the oracle's is what lets every F2-F8 differential stay a straight
\ byte comparison.
VARIABLE PREPROCESS   0 PREPROCESS !
: -P ( -- )  1 PREPROCESS ! ;
VARIABLE C4FC-READY   0 C4FC-READY !
: C4FC-INIT ( -- )  262144 ARENA-INIT  PP-RESET  1 C4FC-READY ! ;
: -I ( a u -- )  PP-PATH ;
: -D ( a u -- )  PP-DEFINE ;
\ -c: compile one unit to an OBJECT. What it cannot resolve it names,
\ and c4rlink resolves it later against the units that can.
: -c ( -- )  1 OBJECT ! ;
\ -o names the output file. Without it the image goes to stdout, which
\ is what a Makefile wants and what C4DOS cannot use -- there is no '>'
\ on that system by decision, so a tool writes its own file.
: -o ( a u -- )  OUT>FILE ;
\ -mcisc needs c4mp; -mfuse takes plain c4 away as well, and implies -O
\ because the pass it turns on lives in the optimizer.
: -mcisc ( -- )  1 CISC ! ;
: -mfuse ( -- )  1 FUSE !  1 OPTIMIZE ! ;

\ Everything a compile of one unit starts from. -O runs it twice: the
\ first pass exists only to learn which functions are reachable, and
\ throws its buffers away.
: UNIT-RESET ( -- )
   EMIT-RESET
   NSYM SYMR * ALLOCATE STAB !  0 STN !  0 NGLO !
   0 #STRUCTS !  0 #MEMS !  0 VA-MAKE !
   0 TP !
   BUILTINS ;

: C4FC ( a u -- )                       \ compile that file, image to stdout
   C4FC-READY @ 0= IF C4FC-INIT THEN
   PREPROCESS @ IF PP-FILE ELSE LEX-FILE THEN
   \ The discovery pass, which runs whatever the flags say. -O wants the
   \ call graph and -c wants to know which names this unit defines, but
   \ the plain compile wants something from it too: a call to a function
   \ defined further down with no prototype anywhere resolves only if
   \ something has read ahead. Making that depend on -O would mean
   \ `c4fc f.c` rejecting a file `c4fc -O f.c` compiles.
   T2-RESET  DECL-RESET  0 EXTN !  1 COLLECT !
   UNIT-RESET PROGRAM
   T2-CLOSE  MAKE-EXTERNS  0 COLLECT !
   UNIT-RESET PREREGISTER PROGRAM
   OBJECT @ 0= ENTRY @ 0< AND IF ." c4fc: no main" CR ABORT THEN
   OPTIMIZE @ IF OPT-RUN THEN
   OBJECT @ IF SN @ FIX-EXTERNS  EXT-SYMS THEN
   WRITE-IMAGE ;
