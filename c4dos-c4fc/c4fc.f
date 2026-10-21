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
\ M4: the integer type words and qualifiers, and the two literal kinds a
\ U suffix makes (unsigned int, unsigned long). Appended, so every kind
\ that was already here keeps its number.
KIND: Const  KIND: Volatile KIND: Signed KIND: Unsigned
KIND: Short  KIND: Long
KIND: NumU   KIND: NumUL
\ M7b: void, which is char in c4, has a kind of its own only so that a
\ parameter list can tell `(void)` from `(char)`. The parser takes Void
\ wherever it takes Char. Appended, like the M4 kinds.
KIND: Void

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
KEYWORD void    Void
KEYWORD const   Const       KEYWORD volatile Volatile
KEYWORD signed  Signed      KEYWORD unsigned Unsigned
KEYWORD short   Short       KEYWORD long    Long

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

\ u/U makes a literal unsigned and l/L (any number of them) widens it;
\ only the unsigned part changes the token: NumU, or NumUL with an l.
: SCAN-SUFFIX ( -- kind ) {: | u l -- k :}
   0 TO u  0 TO l
   BEGIN
      CH 117 = CH 85 = OR IF 1 TO u 1 POS +! 1 ELSE
      CH 108 = CH 76 = OR IF l 1+ TO l 1 POS +! 1 ELSE 0 THEN THEN
   WHILE REPEAT
   u 0= IF Num EXIT THEN
   l 0= IF NumU ELSE NumUL THEN ;
: T-NUM   SCAN-NUMBER SCAN-SUFFIX SWAP 0 TOK, ;
\ every kind that carries a number in t.val
: NUMK? ( k -- f )  DUP Num = OVER NumU = OR SWAP NumUL = OR ;

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
   t t.kind @ NUMK? IF t t.val @ OB-N ELSE
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
\ of what C says, all documented in docs/c4fc-design.md. In M7b c4lc
\ caught up on all of them: TAKEN, defined(X), the including file's
\ directory, the blue paint below, and a function-like macro name with
\ no '(' after it standing for itself. Variadic macros (`...`,
\ __VA_ARGS__, GNU `name...` and `, ## __VA_ARGS__`) came in together.

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

\ m.var is true for a variadic macro, whose last parameter is the
\ variable part: __VA_ARGS__ for `...`, or the name of GNU `name...`.
BEGIN-STRUCTURE MAC
   FIELD: m.name  FIELD: m.nlen  FIELD: m.hash
   FIELD: m.params  FIELD: m.body  FIELD: m.busy  FIELD: m.var
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
   p m m.params !  b m m.body !  0 m m.busy !  0 m m.var !
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
SPELL Int int    SPELL Char char  SPELL Void void  SPELL If if      SPELL Else else
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
   k NUMK? IF t t.val @ N>TEXT EXIT THEN
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
   k NUMK? IF E-TOK t.val @ EXIT THEN
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

\ __LINE__ and __FILE__ (M7b), unless a #define has taken the names: a
\ name the walk is about to emit as itself goes through PP-BUILTIN, which
\ makes those two a number and a string -- c4lc-pp's pp:builtin.
DEFER PP-BUILTIN ( t -- t' )

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
            m IF t m PP-APPLY ELSE t PP-BUILTIN PP-EMIT THEN
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
\ already been taken off the stream. ARG-LIM, when not -1, is how many
\ arguments split off at commas before the rest is one: a variadic
\ macro's variable part, commas and all.
VARIABLE ARG-LIM   -1 ARG-LIM !
: PP-ARGS ( -- args ) {: | args cur d t k -- args :}
   16 NEW-VEC TO args   16 NEW-VEC TO cur   0 TO d
   BEGIN
      PP-SETTLE
      PP# 0= IF ." c4fc: unterminated macro arguments" CR ABORT THEN
      PP-POP TO t   t t.kind @ TO k
      k Eof = IF ." c4fc: unterminated macro arguments" CR ABORT THEN
      k Rparen = d 0= AND IF cur args V,  args EXIT THEN
      k Comma = d 0= AND  ARG-LIM @ -1 = args V# ARG-LIM @ < OR  AND
      IF cur args V,  16 NEW-VEC TO cur
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

\ The variadic parameter of the macro PP-SUBST is filling, or 0.
VARIABLE PP-VA   0 PP-VA !
\ Is token T the name of that parameter?
: VA-PARAM? ( t -- f ) {: t | p -- f :}
   PP-VA @ 0= IF 0 EXIT THEN
   t t.kind @ Id <> IF 0 EXIT THEN
   PP-VA @ TO p
   t t.val @ t t.len @  p t.val @ p t.len @ BYTES2= ;

\ Replace parameters in BODY. An ordinary parameter takes the EXPANDED
\ argument; an operand of # or ## takes the raw one, which is the whole
\ reason STR(V) gives "V" while XSTR(V) gives V's value.
\
\ GNU `, ## __VA_ARGS__` is no paste: an empty variable part takes the
\ comma with it, and otherwise the comma stays and the raw argument
\ follows, as in gcc.
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
            t t.kind @ Comma =  i 2 + body V# < AND
            IF i 2 + body V@ VA-PARAM? ELSE 0 THEN
            IF
               i 2 + body V@ ps as PARAM-OF TO pv
               pv V# IF t r V,  pv V# 0 ?DO I pv V@ r V, LOOP THEN
               i 3 + TO i
            ELSE
            i 2 + body V# < IF
               t ps as PARAM-OF t LAST-OF TO lt
               i 2 + body V@  DUP ps as PARAM-OF SWAP FIRST-OF TO rt
               lt rt PP-PASTE TO pv  pv V# 0 ?DO I pv V@ r V, LOOP
               i 3 + TO i
            ELSE t r V,  i 1+ TO i THEN
            THEN
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

: (APPLY) ( t mac -- ) {: t m | args eargs ex ps -- :}
   m m.params @ 0= IF
      m m.body @ t RELINE TO ex
      m MARK PP-PUSH   ex PP-PUSHV   1 m m.busy !  EXIT THEN
   PP-SETTLE
   PP# 0= IF t PP-EMIT EXIT THEN
   PP-TOP t.kind @ Lparen <> IF t PP-EMIT EXIT THEN
   PP-DROP
   m m.params @ TO ps
   m m.var @ IF ps V# 1- ELSE -1 THEN ARG-LIM !
   PP-ARGS TO args
   -1 ARG-LIM !
   args EXPAND-ARGS TO eargs
   m m.var @ IF ps V# 1- ps V@ ELSE 0 THEN PP-VA !
   m m.body @ ps args eargs PP-SUBST  t RELINE TO ex
   0 PP-VA !
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
\
\ `...` lexes as three Dots. On its own it is the parameter __VA_ARGS__;
\ after a name (GNU `args...`) it makes that name the variable part.
\ Either way it must be last, and PP-PVAR says the macro is variadic.
VARIABLE PP-PVAR
: DOTS? ( v i -- f ) {: v i -- f :}           \ three Dots at v[i]?
   i 2 + v V# < 0= IF 0 EXIT THEN
   i v V@ t.kind @ Dot =  i 1+ v V@ t.kind @ Dot = AND  i 2 + v V@ t.kind @ Dot = AND ;
: VA-TOK ( t -- t' ) {: t | n -- n :}         \ an Id __VA_ARGS__ at t's place
   t TOK-COPY TO n
   Id n t.kind !  S" __VA_ARGS__" PP-STR n t.len ! n t.val !  0 n t.adj !  n ;
: PP-PARAMS ( v i -- params i' ) {: v i | ps t k -- ps i :}
   16 NEW-VEC TO ps   0 PP-PVAR !
   BEGIN i v V# < WHILE
      v i DOTS? IF
         \ after a name: GNU `name...`; otherwise the parameter __VA_ARGS__
         i 0> IF i 1- v V@ t.kind @ Id = ELSE 0 THEN 0=
         IF i v V@ VA-TOK ps V, THEN
         -1 PP-PVAR !   i 3 + TO i
         i v V# < IF i v V@ t.kind @ Rparen = ELSE 0 THEN
         0= IF ." c4fc: ... must end the macro parameter list" CR ABORT THEN
         ps i 1+ EXIT
      THEN
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
      PP-PVAR @  MACROS DUP V# 1- SWAP V@  m.var !
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
: (PP-BUILTIN) ( t -- t' ) {: t | n -- n :}
   t t.val @ t t.len @ S" __LINE__" BYTES2= IF
      t TOK-COPY TO n  Num n t.kind !  t t.line @ n t.val !  0 n t.len !  n EXIT THEN
   t t.val @ t t.len @ S" __FILE__" BYTES2= IF
      t t.file @ FNAME@ t STR-TOK EXIT THEN
   t ;
' (PP-BUILTIN) IS PP-BUILTIN
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
         m IF t m PP-APPLY ELSE t PP-BUILTIN PP-EMIT THEN
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

\ -D NAME   or   -D NAME=VALUE
: PP-DEFINE ( a u -- ) {: a u | e -- :}
   0 TO e
   BEGIN e u < IF a e + C@ 61 <> ELSE 0 THEN WHILE e 1+ TO e REPEAT
   e u >= IF
      a u 0  S" 1" LEX-STR>V V-NOEOF MAC-DEF
   ELSE
      a e 0  a e + 1+  u e - 1-  LEX-STR>V V-NOEOF MAC-DEF
   THEN ;

\ What is defined before any -D, as c4lc-pp.lisp's pp:builtins.
\ __c4_inttypes__: signed, unsigned, short and long are real types
\ here (M4), so stddef.h must not #define them away as it does for
\ c4cc. A gcc -E standing in for this passes it as -D (PREPROC_LC).
: PP-RESET ( -- )
   MACROS 256 VEC-INIT  0 CDEPTH !  0 #INCL !  0 #PATHS !  0 SPLN !
   S" __c4_inttypes__=1" PP-DEFINE ;

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
      t t.kind @ NUMK? IF t t.val @ OB-N ELSE
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

\ A literal carries its type: int, or uint / ulong from a U suffix.
NODE: n_num    NFIELD: >val  NFIELD: >nty      ;NODE
\ A string literal carries its BYTES, not a data offset: the offset is
\ handed out during emission, and a literal inside a branch the tree
\ pass deletes must never be handed one at all.
NODE: n_str    NFIELD: >off  NFIELD: >slen     ;NODE
NODE: n_var    NFIELD: >sym                    ;NODE
NODE: n_gvar   NFIELD: >sym                    ;NODE
NODE: n_asgn   NFIELD: >lhs  NFIELD: >rhs      ;NODE
NODE: n_bin    NFIELD: >lhs  NFIELD: >rhs  NFIELD: >op ;NODE
NODE: n_call   NFIELD: >fn   NFIELD: >args NFIELD: >argn ;NODE
\ A call through an expression (M7b): ops[i](x), s->fn(x), (*fp)(x).
\ >fn holds the target EXPRESSION, not a symbol; the other two are
\ n_call's, at n_call's offsets.
NODE: n_icall  NFIELD: >fn   NFIELD: >args NFIELD: >argn ;NODE
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
NODE: n_member NFIELD: >lhs  NFIELD: >moff NFIELD: >mtype NFIELD: >magg
               NFIELD: >mq   NFIELD: >masz                 ;NODE

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
\ goto NAME; and NAME: STATEMENT (M7b). >body keeps n_if's offset, which
\ is why the name's length comes after it; a goto has no body.
NODE: n_goto    NFIELD: >lname NFIELD: >body NFIELD: >llen ;NODE
NODE: n_labeled NFIELD: >lname NFIELD: >body NFIELD: >llen ;NODE
\ A cast emits nothing and changes everything: (char *)p is the same
\ address and a different type, and the type is what decides LC against
\ LI, SC against SI, and whether p[i] scales by one or by eight.
NODE: n_cast   NFIELD: >expr NFIELD: >ctype NFIELD: >cq ;NODE
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
\ A local array's or struct's brace list (M7b): the object is zeroed, then
\ each of >lrecs -- (offset type node value), parse.f's LREC, -- stored.
NODE: n_lagg   NFIELD: >lsym NFIELD: >lrecs NFIELD: >lbytes ;NODE
NODE: n_expst  NFIELD: >expr                   ;NODE
NODE: n_blk    NFIELD: >list NFIELD: >len      ;NODE

\ Constructors for the one- and two-field shapes, which is most of them.
: N1 ( v tag -- n )   2 CELLS NEW TUCK 1 CELLS + ! ;
\ A literal of a given type -- every n_num is made here, because its
\ type field has to exist even when it is int.
: NUM-OF ( v ty -- n ) {: v ty | n -- n :}
   n_num 3 CELLS NEW TO n   v n 1 CELLS + !   ty n 2 CELLS + !   n ;
: N2 ( a b tag -- n ) 3 CELLS NEW {: a b n -- n :}
   a n 1 CELLS + !  b n 2 CELLS + !  n ;

GENERIC: GEN                            \ an expression, as a value
GENERIC: GEN-ADDR                       \ an expression, as an address
GENERIC: STMT                           \ a statement
GENERIC: CT                             \ an expression's C type
GENERIC: CQ                             \ ... and its qualifier mask (M4)

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
\
\ M4 put a band of scalars between the two, also sixty-four apart and
\ also two per star, so the struct rule covers them unchanged:
\
\    uchar 512   schar 576   short 640   ushort 704   uint 768   ulong 832
\
\ long is int and unsigned long is ulong; uint is 32 bits at either word
\ size (gen.f keeps it in [0, 2^32) on a 64-bit host).
\
\ Qualifiers are NOT here. They are a mask beside the type -- two bits a
\ pointer level, bit 2k const and 2k+1 volatile, level 0 the object
\ itself -- in the symbol (y.q), the cast (>cq) and the member (MB-QUAL).

1024 CONSTANT T-STRUCT0
  64 CONSTANT T-STRIDE
 512 CONSTANT T-BAND0
 512 CONSTANT t_uchar   576 CONSTANT t_schar   640 CONSTANT t_short
 704 CONSTANT t_ushort  768 CONSTANT t_uint    832 CONSTANT t_ulong

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
CREATE MB-QUAL  MMAX-MEM CELLS ALLOT     \ its qualifier mask
CREATE MB-ASZ   MMAX-MEM CELLS ALLOT     \ an array member's bytes, for sizeof (M7b)
VARIABLE #MEMS   0 #MEMS !

: T-BASE ( t -- b )
   DUP T-BAND0 < IF 1 AND EXIT THEN
   T-BAND0 - T-STRIDE / T-STRIDE * T-BAND0 + ;
: T-STARS ( t -- n )  DUP T-BASE - 2 / ;
: T-PTR?  ( t -- f )  T-STARS 0> ;
: T-STRUCT? ( t -- f ) DUP T-STRUCT0 >= SWAP T-STARS 0= AND ;
: T-INDEX ( t -- k )  T-BASE T-STRUCT0 - T-STRIDE / ;
: T-DEREF ( t -- t' ) 2 - ;
: T-ADDR  ( t -- t' ) 2 + ;
\ an integer VALUE type: no star, no struct
: T-ARITH? ( t -- f ) DUP T-STRUCT0 < SWAP T-STARS 0= AND ;
: T-BYTE? ( t -- f )  DUP t_char = OVER t_uchar = OR SWAP t_schar = OR ;
: T-HALF? ( t -- f )  DUP t_short = SWAP t_ushort = OR ;

: T-SIZE ( t -- n )
   DUP T-PTR? IF DROP 1 CELLS EXIT THEN
   DUP T-BYTE? IF DROP 1 EXIT THEN
   DUP T-HALF? IF DROP 2 EXIT THEN
   DUP T-STRUCT0 < IF DROP 1 CELLS EXIT THEN
   T-INDEX CELLS ST-SIZE + @ ;
\ What a pointer of this type steps by. One means "emit no multiply at
\ all", which is not an optimisation but what c4lc does -- char * walks
\ byte by byte with no MUL in sight.
: T-STEP ( t -- n )   T-DEREF T-SIZE ;

\ The usual arithmetic conversions. Everything narrower than a word
\ promotes to int, and int < uint < ulong.
: T-PROMOTE ( t -- t' )
   DUP t_uint = IF EXIT THEN  DUP t_ulong = IF EXIT THEN  DROP t_int ;
: T-UNS? ( t -- f )   DUP t_uint = SWAP t_ulong = OR ;
: T-UAC ( a b -- t ) {: a b | pa pb -- t :}
   a T-PROMOTE TO pa   b T-PROMOTE TO pb
   pa t_ulong = pb t_ulong = OR IF t_ulong EXIT THEN
   pa t_uint  = pb t_uint  = OR IF t_uint  EXIT THEN
   t_int ;
\ two arithmetic operands, at least one of them unsigned: the only case
\ that generates anything other than what c4 always has
: T-UNSBIN? ( a b -- f ) {: a b -- f :}
   a T-ARITH? b T-ARITH? AND 0= IF 0 EXIT THEN
   a T-PROMOTE T-UNS? b T-PROMOTE T-UNS? OR ;
\ ~x and -x: unsigned stays unsigned, everything else is int
: T-UNRES ( t -- t' )  T-PROMOTE DUP T-UNS? 0= IF DROP t_int THEN ;
\ c4 gives a + b and a - b the LEFT operand's type (char + 1 is a char);
\ a band type promotes instead, as c4lc's g:sumty
: T-SUMTY ( t -- t' )
   DUP T-BAND0 >= OVER T-ARITH? AND IF DROP t_int THEN ;
\ the narrow integers and uint: what an inlined call has to cast to
: T-CONV? ( t -- f )
   DUP t_uchar = OVER t_schar = OR OVER t_short = OR
   OVER t_ushort = OR SWAP t_uint = OR ;

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
   0 #MEMS @ CELLS MB-QUAL  + !
   0 #MEMS @ CELLS MB-ASZ   + !
   1 #MEMS +! ;
\ the qualifier mask of the member MEM, just recorded
: MEM-Q! ( q -- )  #MEMS @ 1- CELLS MB-QUAL + ! ;
\ ... and its storage in bytes, when it is an array
: MEM-ASZ! ( n -- )  #MEMS @ 1- CELLS MB-ASZ + ! ;
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
\ F3: IEEE arithmetic (docs/c4-float.md)
89 CONSTANT oFPU

BEGIN-STRUCTURE SYMR
   FIELD: y.name  FIELD: y.nlen  FIELD: y.type  FIELD: y.class  FIELD: y.val
   FIELD: y.ct    FIELD: y.agg   FIELD: y.sz
   FIELD: y.va    FIELD: y.nfix  FIELD: y.ini   FIELD: y.sc
   \ Set the moment a function's name is used as a VALUE (parse.f's
   \ n_fnref). The inliner reads it; nothing else does.
   FIELD: y.atk
   \ M4. The qualifier mask of a variable (types.f); a function's return
   \ mask and its parameters' types and masks (arrays of y.nfix cells,
   \ 0 when unknown), which the const checks read at a call; and the
   \ value and type of a static const scalar, which is a constant.
   FIELD: y.q    FIELD: y.rq   FIELD: y.pt   FIELD: y.pq
   FIELD: y.cv   FIELD: y.cval FIELD: y.cty
   \ A global's forward record: `extern int x;` for an x the unit
   \ defines further down. The definition takes its number over.
   FIELD: y.fwd
END-STRUCTURE
VARIABLE VA-MAKE   0 VA-MAKE !          \ code index of __c4cc_make_va
: SYM[] ( i -- a )  SYMR * SYMS @ + ;

\ The parser's symbol table. It lives here rather than in parse.f
\ because SYMR does, and because inline.f -- which loads between this
\ file and gen.f -- needs both the table and the frame counter in order
\ to point a spliced body's locals at slots in the caller.
\
\ One table for the whole unit, and nothing frees an entry: a compiler
\ is a batch process. Eight thousand entries is under half a megabyte.
8192 CONSTANT NSYM
VARIABLE STAB   VARIABLE STN
VARIABLE NLOC                           \ locals in the function in hand
VARIABLE NGLO                           \ globals, numbered; placed at the end
VARIABLE TP                             \ the token cursor

: ST[] ( i -- a )  SYMR * STAB @ + ;

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
                                        \   4 = the VALUE is a global's number
\ Where each uninitialised global lands, by its number; parse.f's
\ PLACE-GLOBALS fills it in, and DECL does for a declaration a later
\ definition initialises.
8192 CONSTANT GMAX
CREATE GOFF GMAX CELLS ALLOT
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
\ an object's data word holding extern K's address (M7b): type -5, the
\ symbol id filled in by FIX-EXTERNS, resolved by c4rlink
: SYMPAT, ( slot k -- )   -1000 SWAP -  0 -5 (TABPAT) ;
\ `int *p = &x;` (M7b): a region 1 slot holding uninitialised global N's
\ address, which is known once PLACE-GLOBALS has run
: GLOPAT, ( slot n -- )   4 -4 (TABPAT) ;
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
      TABC I CELLS + @  ty -3 = IF TAB-MAP ELSE f 2 AND IF b + THEN
         f 4 AND IF CELLS GOFF + @ THEN THEN
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
\ N bytes of region 1 where the last allocation ended, unaligned
: ID-BYTES ( n -- off ) {: n | off -- off :}
   IDN @ TO off
   BEGIN off n + IMAX @ > WHILE IDATA IMAX DGROW REPEAT
   n IDN +!  off ;
\ ... and a global's, which starts on a word
: ID-ALLOT ( n -- off )
   IDN @ 1 CELLS 1- + 1 CELLS 1- INVERT AND IDN !
   ID-BYTES ;
\ A string a GLOBAL INITIALISER needs is allocated here rather than with
\ the other literals, because c4lc lays the data out in one pass over
\ the declarations and the string is met while that global is: the
\ pointer's own word comes AFTER the bytes it points at.
\
\ The bytes are not aligned: c4lc puts them where the last global ended,
\ so after `char t[4] = "ab";` the string "hi" of `char *s = "hi";` sits
\ in t's last four bytes and s at the next word. Aligning them moved s
\ a word further on than c4lc puts it (found by M7b's static locals).
: ID-STR, ( a u -- off ) {: a u | off -- off :}
   u 1+ ID-BYTES TO off
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
\ M7b: a DATA word holding extern K's address is patch type -5 with the
\ placeholder in its VALUE, which becomes the symbol id here as well
: FIX-EXTERNS ( base -- ) {: b | p -- :}
   PN @ 0 ?DO
      I 3 * CELLS PATCH @ + TO p
      p @ -1000 <= IF  b -1000 p @ - +  p !  0 p 2 CELLS + !  THEN
      p @ -5 = p 2 CELLS + @ -1000 <= AND IF  b -1000 p 2 CELLS + @ - +  p 2 CELLS + !  THEN
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

\ -- typed constants (M4) ------------------------------------------------
\ The same arithmetic on constants that carry a type -- c4lc-parse.lisp's
\ p:tfold, used by the constant expressions in parse.f and the tree pass
\ alike. int is FOLD1 exactly. uint on a 64-bit host lives in [0, 2^32),
\ where the signed operations are already right once the operands are
\ cut into range and a result that can leave it is cut back. A whole
\ unsigned word -- ulong, or uint on a 32-bit host -- compares with its
\ sign bit flipped, divides by UDIV and shifts with RSHIFT, which is
\ Forth's LOGICAL shift: the one c4's >> is not.
VARIABLE FTY                             \ the type TFOLD's result has
1 CELLS 8 * CONSTANT WBITS
1 WBITS 1- LSHIFT CONSTANT WMIN
1 CELLS 8 = CONSTANT W64?
: (M32) ( -- m )  W64? IF 1 32 LSHIFT 1- ELSE -1 THEN ;
(M32) CONSTANT M32
: ULT ( a b -- f )  WMIN XOR SWAP WMIN XOR SWAP < ;
: UDIV ( a b -- q ) {: a b | q -- q :}
   b 0< IF a b ULT IF 0 ELSE 1 THEN EXIT THEN
   a 0< 0= IF a b / EXIT THEN
   a 1 RSHIFT b / 1 LSHIFT TO q
   a q b * - b ULT IF q ELSE q 1+ THEN ;
: UMOD ( a b -- r )  OVER OVER UDIV * - ;
: CMPOP? ( op -- f ) {: o -- f :}
   o oEQ = o oNE = OR o oLT = OR o oGT = OR o oLE = OR o oGE = OR ;
: TFOLD ( a at b bt op -- v ok ) {: a at b bt o | r v ok -- v ok :}
   o oSHL = o oSHR = OR IF at T-PROMOTE ELSE at bt T-UAC THEN TO r
   o CMPOP? IF t_int ELSE r THEN FTY !
   r t_int = IF a b o FOLD1 EXIT THEN
   r t_uint = W64? AND IF
      a M32 AND TO a
      o oSHL = o oSHR = OR 0= IF b M32 AND TO b THEN
      a b o FOLD1 TO ok TO v
      o oADD = o oSUB = OR o oMUL = OR o oSHL = OR o oOR = OR o oXOR = OR
      IF v M32 AND TO v THEN
      v ok EXIT THEN
   o oDIV = IF b 0= IF 0 0 EXIT THEN a b UDIV 1 EXIT THEN
   o oMOD = IF b 0= IF 0 0 EXIT THEN a b UMOD 1 EXIT THEN
   o oSHR = IF a b RSHIFT 1 EXIT THEN
   o oLT = IF a b ULT 1 AND 1 EXIT THEN
   o oGT = IF b a ULT 1 AND 1 EXIT THEN
   o oLE = IF b a ULT 0= 1 AND 1 EXIT THEN
   o oGE = IF a b ULT 0= 1 AND 1 EXIT THEN
   a b o FOLD1 ;
\ a value converted to an integer type, the way the generated code does
: SEXT ( v n -- v' )  WBITS SWAP - TUCK LSHIFT SWAP ASHR ;
: CONVVAL ( v t -- v' ) {: v t -- v' :}
   t t_char = t t_schar = OR IF v 8 SEXT EXIT THEN
   t t_uchar = IF v 255 AND EXIT THEN
   t t_short = IF v 16 SEXT EXIT THEN
   t t_ushort = IF v 65535 AND EXIT THEN
   t t_uint = IF v M32 AND EXIT THEN
   v ;

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

\ M4: the function being generated, for the const diagnostics, and its
\ return type and mask, which a return converts to and checks against.
\ A spliced body (inline.f) returns as its callee, so it swaps the two.
VARIABLE FNA   VARIABLE FNU   0 FNU !
VARIABLE RETT  1 RETT !  VARIABLE RETQ  0 RETQ !

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
: MKNUM ( v -- n )   t_int NUM-OF ;
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
:M FOLD n_goto    ;M
:M FOLD n_labeled {: n -- n :}  n >body @ FOLD n >body !  n ;M
\ M7b: set while a function with a label or a goto is parsed. Then a
\ constant condition drops nothing, since a goto may enter the branch
\ that looks dead; c4lc-tree's t:keeplabels is the same rule.
VARIABLE FN-LABELS   0 FN-LABELS !
:M FOLD n_cont    ;M
:M FOLD n_empty   ;M

\ -- arithmetic ----------------------------------------------------------

\ Typed (M4): TFOLD is FOLD1 for two ints, and the unsigned arithmetic
\ when a U-suffixed literal is in it -- c4lc's p:tfold.
:M FOLD n_bin {: n | a b v ok -- n :}
   n >lhs @ FOLD DUP n >lhs !  TO a
   n >rhs @ FOLD DUP n >rhs !  TO b
   a NUM? b NUM? AND IF
      a >val @ a >nty @  b >val @ b >nty @  n >op @ TFOLD TO ok TO v
      ok IF FOLDED v FTY @ NUM-OF EXIT THEN
   THEN
   n ;M

:M FOLD n_not  {: n | a -- n :}
   n >opnd @ FOLD DUP n >opnd ! TO a
   a NUM? IF FOLDED a >val @ 0= 1 AND MKNUM EXIT THEN  n ;M
:M FOLD n_bnot {: n | a -- n :}
   n >opnd @ FOLD DUP n >opnd ! TO a
   a NUM? IF FOLDED -1 t_int a >val @ a >nty @ oXOR TFOLD DROP FTY @ NUM-OF EXIT THEN
   n ;M
:M FOLD n_neg  {: n | a -- n :}
   n >opnd @ FOLD DUP n >opnd ! TO a
   a NUM? IF FOLDED 0 t_int a >val @ a >nty @ oSUB TFOLD DROP FTY @ NUM-OF EXIT THEN
   n ;M

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
:M FOLD n_icall {: n -- n :}  n FOLD-ARGS  n >fn @ FOLD n >fn !  n ;M
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
\ c4lc's T1 folds no brace list's expression
:M FOLD n_lagg ;M
:M FOLD n_switch {: n -- n :}
   n >cond @ FOLD n >cond !  n >body @ FOLD n >body !  n ;M

:M FOLD n_if {: n | c -- n :}
   n >cond @ FOLD DUP n >cond ! TO c
   c NUM? FN-LABELS @ 0= AND IF
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
   c NUM? c >val @ 0= AND FN-LABELS @ 0= AND IF FOLDED EMPTY-STMT EXIT THEN
   n >body @ FOLD n >body !  n ;M

\ do/while runs its body at least once, so only the parts fold.
:M FOLD n_do {: n -- n :}
   n >cond @ FOLD n >cond !  n >body @ FOLD n >body !  n ;M

\ for (i; 0; s) keeps the initialiser's side effects and nothing else.
:M FOLD n_for {: n | c i -- n :}
   n >init @ ?DUP IF FOLD DUP n >init ! THEN
   n >cond @ ?DUP IF FOLD DUP n >cond ! TO c ELSE 0 TO c THEN
   c IF c NUM? c >val @ 0= AND FN-LABELS @ 0= AND IF
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
\ ...and, for the const checks (M4), its return mask and its parameters'
\ types and masks (parse.f's PSIG arrays, which live in the arena)
CREATE FN-RQ   FNMAX CELLS ALLOT
CREATE FN-PT   FNMAX CELLS ALLOT
CREATE FN-PQ   FNMAX CELLS ALLOT
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
   0 #FNS @ CELLS FN-RQ + !  0 #FNS @ CELLS FN-PT + !  0 #FNS @ CELLS FN-PQ + !
   #FNS @   1 #FNS +! ;
: FN-SIG! ( i ct nfix va -- ) {: i ct n va -- :}
   ct i CELLS FN-T + !  n i CELLS FN-N + !  va i CELLS FN-V + ! ;
: FN-SIG2! ( i rq pt pq -- ) {: i rq pt pq -- :}
   rq i CELLS FN-RQ + !  pt i CELLS FN-PT + !  pq i CELLS FN-PQ + ! ;
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
:M REFS n_goto    DROP ;M
:M REFS n_labeled >body @ REFS ;M
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
:M REFS n_lagg {: n | v -- :}
   n >lrecs @ TO v
   v V# 0 ?DO I v V@ 2 CELLS + @ REFS? LOOP ;M
:M REFS n_comma   REFS-ARGS ;M
:M REFS n_call  {: n -- :}  n >fn @ SYM-REF  n REFS-ARGS ;M
:M REFS n_icall {: n -- :}  n >fn @ REFS  n REFS-ARGS ;M

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
\ c4fc inline.f -- the inliner (docs/inline-small-functions.md, I7).
\
\ A call in c4 costs JSR, ENT, ADJ and LEV whatever is on the other side
\ of it, plus a push per argument and a bp-relative load for every
\ parameter the body reads. For a one-line accessor that is most of the
\ cost of using one.
\
\ THIS PORT IS SIMPLER THAN c4lc's, and the reason is worth stating.
\
\ c4lc rewrites the AST: it substitutes arguments, renames locals, and
\ turns `return E` into an assignment plus a jump. c4fc does none of
\ that, because it EMITS as it walks and GEN is read-only over the tree
\ (there is not one store into a node in gen.f or emit.f). So a body is
\ spliced by pointing the callee's symbols at fresh slots in the
\ CALLER's frame and generating the SAME tree again. No copy, no
\ rewrite, no substitution.
\
\ Two things fall out for free:
\
\   * `return` needs no rewriting. c4's result lives in A, and a spliced
\     `return E` leaves E in A exactly as the call would have -- so it
\     only has to jump instead of LEV. That is one flag and one branch
\     in STMT n_ret, where c4lc needed a whole second pass (T4b) with
\     two AST node kinds of its own.
\   * and because the value arrives the same way, a splice works in
\     EXPRESSION position. c4lc's T4 is restricted to `f(a);` and
\     `x = f(a);`; this one is not.
\
\ What it shares is the rule that cost c4lc a miscompilation: a function
\ whose ADDRESS IS TAKEN is closed to the inliner in BOTH directions --
\ never spliced, and never spliced into. C4KE enters a custom opcode
\ handler by reading its ENT operand and jumping past it
\ (`__c4_adjust(*(handler - 1) * -1); __c4_jmp(handler);`), so the frame
\ that ENT describes is a contract with code somewhere else, and growing
\ it rewrites one side of that contract only.

VARIABLE INLINING   0 INLINING !         \ -minline
VARIABLE NINLINE    0 NINLINE !          \ splices performed

\ How big a body is worth splicing, measured in TOKENS -- the span the
\ parser consumed for it. c4lc counts AST nodes and needs a walk to do
\ it; here the token cursor is already a size, for nothing.
\
\ 40 because a node is roughly one to two tokens, so it is the same
\ neighbourhood as the 60 AST nodes c4lc measured on C4KE. That is a
\ MATCHED default, not an independently measured one: c4fc has no
\ speed benchmark of its own (docs/c4fc-design.md is explicit that it
\ is not a speed play), so the honest thing is to inherit the figure
\ that was measured and say so. What IS measured here is the cost of
\ getting it wrong -- on the stress corpus the image goes 9,502 bytes
\ at 24, 13,469 at 32, 16,029 at 60, so this grows code quickly and
\ the budget is the whole of the control over it.
VARIABLE INLMAX     40 INLMAX !

\ ---- functions whose address is taken --------------------------------
\ Kept by NAME rather than by symbol, because UNIT-RESET reallocates the
\ symbol table between the two passes and a mark made in pass one has to
\ survive into pass two -- exactly as T2's root and reference tables do.
256 CONSTANT ATK-MAX
VARIABLE ATK#    0 ATK# !
VARIABLE ATK-A                            \ name pointer
VARIABLE ATK-U                            \ name length

: ATK-RESET ( -- )
   0 ATK# !
   ATK-MAX CELLS ALLOT: ATK-A !
   ATK-MAX CELLS ALLOT: ATK-U ! ;

: ATK? ( a u -- f ) {: a u -- f :}
   ATK# @ 0 ?DO
      I CELLS ATK-U @ + @ u = IF
         I CELLS ATK-A @ + @  a u BYTES= IF TRUE UNLOOP EXIT THEN
      THEN
   LOOP FALSE ;

: ATK, ( a u -- ) {: a u -- :}
   a u ATK? IF EXIT THEN
   ATK# @ ATK-MAX < IF
      a ATK# @ CELLS ATK-A @ + !
      u ATK# @ CELLS ATK-U @ + !
      1 ATK# +!
   THEN ;

\ ---- the candidate table ---------------------------------------------
\ Parallel arrays rather than a record, which is what the rest of this
\ compiler does with small fixed tables (PMAP and LMAP in opt.f).
64 CONSTANT IC-MAX
VARIABLE IC#     0 IC# !
VARIABLE IC-FN                            \ the function's symbol
VARIABLE IC-BODY                          \ its body node
VARIABLE IC-SB                            \ first symbol index: params, then locals
VARIABLE IC-SN                            \ how many of them
VARIABLE IC-NFIX                          \ how many of those are parameters

: INL-RESET ( -- )
   0 IC# !  0 NINLINE !
   IC-MAX CELLS ALLOT: IC-FN !
   IC-MAX CELLS ALLOT: IC-BODY !
   IC-MAX CELLS ALLOT: IC-SB !
   IC-MAX CELLS ALLOT: IC-SN !
   IC-MAX CELLS ALLOT: IC-NFIX !
   ATK-RESET ;

: IC[] ( v i -- a )  CELLS SWAP @ + ;

: IC-FIND ( y -- i|-1 ) {: y -- i :}
   IC# @ 0 ?DO  y IC-FN I IC[] @ = IF I UNLOOP EXIT THEN  LOOP  -1 ;

\ Record one. `sb` is the symbol index the parameters start at, `sn`
\ counts parameters AND locals -- they are contiguous, because PARAMS
\ declares its symbols and then the body's declarations follow, and
\ nothing between them declares anything else.
: IC, ( y body sb sn nfix -- ) {: y b sb sn nf -- :}
   IC# @ IC-MAX < 0= IF EXIT THEN
   y  IC-FN   IC# @ IC[] !
   b  IC-BODY IC# @ IC[] !
   sb IC-SB   IC# @ IC[] !
   sn IC-SN   IC# @ IC[] !
   nf IC-NFIX IC# @ IC[] !
   1 IC# +! ;

\ ---- returns inside a splice -----------------------------------------
\ Non-zero while a spliced body is being generated. It does two jobs and
\ they belong together: it tells n_ret to jump instead of LEV, and it
\ stops a splice happening inside a splice. One level only -- nothing
\ bounds a cascade but the call graph happening to be acyclic, and a
\ self-recursive candidate would splice into itself forever.
VARIABLE INLRET     0 INLRET !
64 CONSTANT INLM-MAX
VARIABLE INLM#      0 INLM# !
VARIABLE INLMARKS

: INLM-RESET ( -- )  INLM-MAX CELLS ALLOT: INLMARKS !  0 INLM# ! ;
: INLM, ( m -- )
   INLM# @ INLM-MAX < IF
      INLM# @ CELLS INLMARKS @ + !  1 INLM# +!
   ELSE DROP THEN ;

\ every `return` in the body jumped here; resolve them all to this point
: INLM-RES ( base -- ) {: b -- :}
   INLM# @ b ?DO  I CELLS INLMARKS @ + @ >RES  LOOP
   b INLM# ! ;

\ ---- the splice ------------------------------------------------------

\ Slots for the callee's parameters and locals, in the CALLER's frame.
\ A local's val is its slot NEGATED (parse.f), so a fresh one is just
\ the next NLOC.
\
\ Reserving and BINDING are deliberately two steps, with the arguments
\ evaluated in between. An argument can contain a call that is itself
\ spliced -- sq(sq(2)) -- and the inner splice retargets the very same
\ symbols, because it is the same function. Binding before evaluating
\ the arguments therefore has the inner splice overwrite the outer
\ one's slots, and the outer body then reads the inner's: sq(sq(2))
\ quietly returns 4 instead of 16. Assigning y.val last is what makes
\ nesting safe, and it costs nothing.
: INL-RESERVE ( i -- first ) {: i | n f -- f :}
   IC-SN i IC[] @ TO n
   NLOC @ 1+ TO f
   NLOC @ n + NLOC !
   f ;

\ Each argument is evaluated ONCE, in order, into its parameter's slot
\ -- addressed by NUMBER, since y.val is not bound yet. That is what
\ makes this safe where c4lc's substitution needed purity rules: an
\ argument here may call, assign and increment freely.
: INL-ARGS ( n i first -- ) {: n i f | k -- :}
   n >argn @ TO k
   k 0 ?DO
      f I + NEGATE oLEA OP2,
      oPSH OP,
      n >args @ I CELLS + @ GEN
      oSI OP,
   LOOP ;

: INL-BIND ( i first -- ) {: i f | b n -- :}
   IC-SB i IC[] @ TO b
   IC-SN i IC[] @ TO n
   n 0 ?DO  f I + NEGATE  b I + ST[] y.val !  LOOP ;

: INL-SPLICE ( n i -- ) {: n i | old base f rt rq -- :}
   i INL-RESERVE TO f
   n i f INL-ARGS
   i f INL-BIND
   INLRET @ TO old   1 INLRET !
   INLM# @ TO base
   \ a return in the body converts to the CALLEE's type, as the call would
   RETT @ TO rt   RETQ @ TO rq
   IC-FN i IC[] @ DUP y.ct @ RETT !  y.rq @ RETQ !
   IC-BODY i IC[] @ STMT
   rt RETT !   rq RETQ !
   base INLM-RES
   old INLRET !
   1 NINLINE +! ;

\ May this call be spliced? -1 for no, otherwise the candidate index.
\ The argument count must match exactly: c4 lets a call pass fewer than
\ the callee declares and the callee simply reads rubbish, which is a
\ thing a CALL can survive and a splice cannot.
: INL-TRY ( n -- i ) {: n | f i -- i :}
   INLINING @ 0= IF -1 EXIT THEN
   INLRET @ IF -1 EXIT THEN
   n >fn @ TO f
   f IC-FIND TO i
   i 0< IF -1 EXIT THEN
   n >argn @ IC-NFIX i IC[] @ = 0= IF -1 EXIT THEN
   i ;
\ c4fc gen.f -- code emission, one method per node kind.
\
\ Nothing here dispatches. A kind reaches a phase by having a method in
\ it, and a kind that cannot be assigned to simply has no GEN-ADDR --
\ which is the whole lvalue rule, enforced by the method table rather
\ than by a flag.

:M GEN n_num   >val @ oIMM OP2, ;M
:M GEN n_str   {: n -- :}  n >off @ n >slen @ D-STR, IMMD, ;M

\ -- M4: the helpers -----------------------------------------------------
\ Six small routines called where the VM has no opcode: a 16-bit load
\ (sign- or zero-extending), a 16-bit store, unsigned word division and
\ remainder, and a logical right shift by a run-time count. Each is laid
\ down ONCE, after the last function, and only if something called it;
\ a call is a JSR to an address in the same unit, so there is no symbol
\ and nothing for c4rlink to know. c4lc-gen.lisp's g:helpbody is the
\ same code, instruction for instruction.
0 CONSTANT h_ld16s  1 CONSTANT h_ld16u  2 CONSTANT h_st16
3 CONSTANT h_udiv   4 CONSTANT h_umod   5 CONSTANT h_lsr
CREATE HM-ID VEC ALLOT   CREATE HM-MK VEC ALLOT
: HELP-RESET ( -- )  HM-ID 64 VEC-INIT  HM-MK 64 VEC-INIT ;
HELP-RESET
\ the argument(s) are on the stack but for the one in A
: HCALL ( h n -- ) {: h n -- :}
   oPSH OP,  oJSR BR, HM-MK V,  h HM-ID V,  n oADJ OP2, ;
: HUSED? ( h -- f ) {: h -- f :}
   HM-ID V# 0 ?DO I HM-ID V@ h = IF -1 UNLOOP EXIT THEN LOOP 0 ;
: HRES ( h -- ) {: h -- :}               \ every call to it lands here
   HM-ID V# 0 ?DO I HM-ID V@ h = IF I HM-MK V@ >RES THEN LOOP ;

: L2, ( n -- )  oLEA OP2, ;
: I2, ( n -- )  oIMM OP2, ;
: H-LD16 ( signed -- ) {: sg -- :}
   0 oENT OP2,
   2 L2, oLI OP, oPSH OP, 1 I2, oADD OP, oLC OP,
   sg 0= IF oPSH OP, 255 I2, oAND OP, THEN
   oPSH OP, 8 I2, oSHL OP,
   oPSH OP, 2 L2, oLI OP, oLC OP, oPSH OP, 255 I2, oAND OP, oOR OP, oLEV OP, ;
: H-ST16 ( -- )
   0 oENT OP2,
   3 L2, oLI OP, oPSH OP, 2 L2, oLI OP, oSC OP,
   3 L2, oLI OP, oPSH OP, 1 I2, oADD OP, oPSH OP,
   2 L2, oLI OP, oPSH OP, 8 I2, oSHR OP, oSC OP,
   2 L2, oLI OP, oLEV OP, ;
: H-ULT, ( -- )                          \ A := stack (^MIN) vs b (^MIN), flag to come
   oPSH OP, WMIN I2, oXOR OP, oPSH OP, 2 L2, oLI OP, oPSH OP, WMIN I2, oXOR OP, ;
: H-UDIV ( -- ) {: | m1 m2 -- :}
   1 oENT OP2,
   2 L2, oLI OP, oPSH OP, 0 I2, oLT OP, oBZ BR, TO m1
   3 L2, oLI OP, H-ULT, oGE OP, oLEV OP,
   m1 >RES
   3 L2, oLI OP, oPSH OP, 0 I2, oGE OP, oBZ BR, TO m2
   3 L2, oLI OP, oPSH OP, 2 L2, oLI OP, oDIV OP, oLEV OP,
   m2 >RES
   -1 L2, oPSH OP, 3 L2, oLI OP, oPSH OP, 1 I2, oSHR OP,
   oPSH OP, WMIN INVERT I2, oAND OP, oPSH OP, 2 L2, oLI OP, oDIV OP,
   oPSH OP, 1 I2, oSHL OP, oSI OP,
   3 L2, oLI OP, oPSH OP, -1 L2, oLI OP, oPSH OP, 2 L2, oLI OP, oMUL OP, oSUB OP,
   H-ULT, oGE OP,
   oPSH OP, -1 L2, oLI OP, oADD OP, oLEV OP, ;
: H-UMOD ( -- ) {: | m1 m2 m3 m4 -- :}
   1 oENT OP2,
   2 L2, oLI OP, oPSH OP, 0 I2, oLT OP, oBZ BR, TO m1
   3 L2, oLI OP, H-ULT, oLT OP, oBZ BR, TO m2
   3 L2, oLI OP, oLEV OP,
   m2 >RES
   3 L2, oLI OP, oPSH OP, 2 L2, oLI OP, oSUB OP, oLEV OP,
   m1 >RES
   3 L2, oLI OP, oPSH OP, 0 I2, oGE OP, oBZ BR, TO m3
   3 L2, oLI OP, oPSH OP, 2 L2, oLI OP, oMOD OP, oLEV OP,
   m3 >RES
   -1 L2, oPSH OP, 3 L2, oLI OP, oPSH OP, 3 L2, oLI OP, oPSH OP, 1 I2, oSHR OP,
   oPSH OP, WMIN INVERT I2, oAND OP, oPSH OP, 2 L2, oLI OP, oDIV OP,
   oPSH OP, 1 I2, oSHL OP, oPSH OP, 2 L2, oLI OP, oMUL OP, oSUB OP, oSI OP,
   -1 L2, oLI OP, H-ULT, oLT OP, oBZ BR, TO m4
   -1 L2, oLI OP, oLEV OP,
   m4 >RES
   -1 L2, oLI OP, oPSH OP, 2 L2, oLI OP, oSUB OP, oLEV OP, ;
: H-LSR ( -- ) {: | m -- :}
   0 oENT OP2,
   2 L2, oLI OP, oBZ BR, TO m
   3 L2, oLI OP, oPSH OP, 2 L2, oLI OP, oSHR OP,
   oPSH OP, -1 I2, oPSH OP, WBITS I2, oPSH OP, 2 L2, oLI OP, oSUB OP,
   oSHL OP, oPSH OP, -1 I2, oXOR OP, oAND OP, oLEV OP,
   m >RES
   3 L2, oLI OP, oLEV OP, ;
: EMIT-HELPERS ( -- )
   h_ld16s HUSED? IF h_ld16s HRES -1 H-LD16 THEN
   h_ld16u HUSED? IF h_ld16u HRES  0 H-LD16 THEN
   h_st16  HUSED? IF h_st16  HRES H-ST16 THEN
   h_udiv  HUSED? IF h_udiv  HRES H-UDIV THEN
   h_umod  HUSED? IF h_umod  HRES H-UMOD THEN
   h_lsr   HUSED? IF h_lsr   HRES H-LSR  THEN ;

\ -- M4: loads, stores and conversions by type ----------------------------
: MASK, ( m -- )  oPSH OP, oIMM OP2, oAND OP, ;
: SEXT, ( n -- )  WBITS SWAP - DUP oPSH OP, oIMM OP2, oSHL OP, oPSH OP, oIMM OP2, oSHR OP, ;
: XMIN, ( -- )    oPSH OP, WMIN oIMM OP2, oXOR OP, ;

\ A variable's symbol carries the LEA operand it was given: positive for
\ a parameter, negative for a local. c4 addresses both the same way, so
\ one method covers both and the symbol table is where the difference
\ lives.
: LOAD, ( ct -- ) {: t -- :}
   t t_char = t t_schar = OR IF oLC OP, EXIT THEN
   t t_uchar = IF oLC OP, 255 MASK, EXIT THEN
   t t_short = IF h_ld16s 1 HCALL EXIT THEN
   t t_ushort = IF h_ld16u 1 HCALL EXIT THEN
   t t_uint = W64? AND IF oLI OP, M32 MASK, EXIT THEN
   oLI OP, ;
: STORE, ( ct -- ) {: t -- :}
   t T-BYTE? IF oSC OP, EXIT THEN
   t T-HALF? IF h_st16 2 HCALL EXIT THEN
   oSI OP, ;

\ The conversion code for a value of type f in A, to type t: only to an
\ integer type, and nothing when it already fits (c4lc's g:convert!).
: CONVERT, ( f t -- ) {: f t -- :}
   t T-ARITH? 0= IF EXIT THEN
   t t_char = t t_schar = OR IF f t_char = f t_schar = OR 0= IF 8 SEXT, THEN EXIT THEN
   t t_uchar = IF f t_uchar <> IF 255 MASK, THEN EXIT THEN
   t t_short = IF f T-BYTE? f t_short = OR 0= IF 16 SEXT, THEN EXIT THEN
   t t_ushort = IF f t_uchar = f t_ushort = OR 0= IF 65535 MASK, THEN EXIT THEN
   t t_uint = W64? AND IF
      f t_uchar = f t_ushort = OR f t_uint = OR 0= IF M32 MASK, THEN THEN ;
\ ... and the one an assignment, a ++ or a return makes. char keeps c4's
\ own rule (SC drops the rest, the value stays); the new types get C's.
: SCONV, ( f t -- ) {: f t -- :}
   t t_char = t t_int = OR t t_ulong = OR IF EXIT THEN
   f t CONVERT, ;

\ -- M4: const --------------------------------------------------------------
\ Worded exactly as c4lc-gen.lisp words them, because the diagnostics
\ battery holds the two to the same output. A warning prints only when
\ the image goes to a file (-o): c4th has no stderr, and on stdout it
\ would be part of the image.
\ FNA/FNU and RETT/RETQ, the function being generated and its return
\ type and mask, are in emit.f: the inliner swaps the latter too.
: FN-NAME! ( a u -- )  FNU ! FNA ! ;
: FN-RET! ( t q -- )   RETQ ! RETT ! ;
357913940 CONSTANT CQ1                  \ the const bits of levels 1..14
: .INFN ( -- )  ."  (in function " FNA @ FNU @ TYPE ." )" CR ;
: WCHECK ( lvalue a u -- ) {: n a u -- :}
   n CQ 1 AND IF ." error: " a u TYPE ."  of read-only location" .INFN ABORT THEN ;
: DROPPED? ( lq rq -- f )  SWAP INVERT AND CQ1 AND 0<> ;
: WARN? ( -- f )  COLLECT @ 0= OUTNAME @ 0<> AND ;
: QWARN ( a u -- )
   WARN? 0= IF 2DROP EXIT THEN
   ." warning: " TYPE ."  discards const qualifier" .INFN ;
\ argument i of a call to f, after it is generated
: ARGCHECK ( arg i f -- ) {: a i f -- :}
   f y.pt @ 0= IF EXIT THEN
   i f y.nfix @ < 0= IF EXIT THEN
   f y.pt @ i CELLS + @ T-PTR? 0= IF EXIT THEN
   f y.pq @ i CELLS + @  a CQ  DROPPED? 0= IF EXIT THEN
   WARN? 0= IF EXIT THEN
   ." warning: passing argument " i 1+ .N ."  of " f y.name @ f y.nlen @ TYPE
   ."  discards const qualifier" .INFN ;

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

:M GEN      n_cast {: n -- :}  n >expr @ GEN  n >expr @ CT n >ctype @ CONVERT, ;M
:M GEN-ADDR n_cast >expr @ GEN-ADDR ;M
:M CT       n_cast >ctype @ ;M
:M CQ       n_cast >cq @ ;M
:M CQ       n_fnref DROP 0 ;M

:M CT n_num  >nty @ ;M
:M CT n_str  DROP 2 ;M                  \ char *
:M CT n_var  >sym @ y.ct @ ;M
:M CT n_gvar >sym @ y.ct @ ;M
\ c4lc's g:typeof, rule for rule: c4 gives + and - the left operand's
\ type (a band type promotes), char * - char * stays a char *, and an
\ unsigned operand brings the usual arithmetic conversions.
\ Two locals, as before M4: this recurses down the left operand, and
\ c4th's locals stack is 512 cells with no check (see c4fc-design.md).
:M CT n_bin {: n | lt -- :}
   n >lhs @ CT TO lt
   n >op @ oADD = n >op @ oSUB = OR IF
      lt T-PTR? IF
         n >op @ oADD = IF lt EXIT THEN
         lt T-STEP 1 > n >rhs @ CT lt = AND IF t_int EXIT THEN  lt EXIT THEN
      lt n >rhs @ CT T-UNSBIN? IF lt n >rhs @ CT T-UAC EXIT THEN
      lt T-SUMTY EXIT THEN
   n >op @ oSHL = n >op @ oSHR = OR IF lt T-ARITH? IF lt T-UNRES ELSE t_int THEN EXIT THEN
   n >op @ CMPOP? IF t_int EXIT THEN
   lt n >rhs @ CT T-UNSBIN? IF lt n >rhs @ CT T-UAC ELSE t_int THEN ;M
:M CT n_call >fn @ y.ct @ ;M
:M CT n_icall DROP t_int ;M
:M CT n_asgn >lhs @ CT ;M

\ The qualifier mask of an expression (c4lc's g:qualof): two bits a
\ pointer level, level 0 the object the expression designates.
:M CQ n_num  DROP 0 ;M
:M CQ n_str  DROP 0 ;M
:M CQ n_bin  DROP 0 ;M
:M CQ n_var  >sym @ y.q @ ;M
:M CQ n_gvar >sym @ y.q @ ;M
:M CQ n_call >fn @ y.rq @ ;M
:M CQ n_icall DROP 0 ;M
:M CQ n_asgn >lhs @ CQ ;M

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
   t T-BAND0 < 0= IF 0 EXIT THEN        \ a band type needs a mask or a helper
   t t_char <> ;

\ struct = struct (M7b): the bytes, by the VM's own memcpy -- what a
\ builtin memcpy(&L, &R, sizeof L) compiles to, as c4lc-gen's
\ g:structassign. The right side must be an lvalue, so it has GEN-ADDR.
: STRUCT-ASGN ( n -- ) {: n -- :}
   n >lhs @ GEN-ADDR   oPSH OP,
   n >rhs @ GEN-ADDR   oPSH OP,
   n >lhs @ CT T-SIZE oIMM OP2,   oPSH OP,
   oMCPY OP,   3 oADJ OP2, ;
:M GEN n_asgn {: n -- :}
   n >lhs @ CT T-STRUCT? IF n STRUCT-ASGN EXIT THEN
   \ base and index both stay on the stack, unscaled, until the value is
   \ evaluated -- SXI wants all three, and computing the address first
   \ would need a slot to hold it across the right-hand side.
   n >lhs @ CISC-INDEX? IF
      n >lhs @ >lhs @ GEN  oPSH OP,
      n >lhs @ >rhs @ GEN  oPSH OP,
      n >rhs @ GEN  oSXI OP, EXIT THEN
   n >lhs @ GEN-ADDR
   n >lhs @ S" assignment" WCHECK
   oPSH OP,
   n >rhs @ GEN
   n >rhs @ CT  n >lhs @ CT  SCONV,
   n >lhs @ CT T-PTR? IF
      n >lhs @ CQ  n >rhs @ CQ  DROPPED? IF S" assignment" QWARN THEN THEN
   n >lhs @ CT STORE, ;M

\ Pointer arithmetic scales by what the pointer points AT, and a step of
\ one emits no multiply at all -- which is not an optimisation but what
\ c4lc does: char * walks byte by byte with no MUL in sight.
: SCALE, ( t -- )   T-STEP DUP 1 = IF DROP EXIT THEN oPSH OP, oIMM OP2, oMUL OP, ;
: STEP-OF ( t -- n ) DUP T-PTR? IF T-STEP ELSE DROP 1 THEN ;

\ M4: an unsigned operand. uint on a 64-bit host ("narrow") is kept in
\ [0, 2^32), so the signed opcodes are right once the other side of a
\ compare or a division is cut into range, and a result that can leave
\ the range is cut back. A whole unsigned word flips the sign bit for an
\ ordering, calls a helper to divide, and masks a right shift.
: NARROW? ( n -- f )                     \ the result is a 64-bit host's uint
   W64? 0= IF DROP 0 EXIT THEN
   DUP >op @ DUP oSHL = SWAP oSHR = OR IF >lhs @ CT T-PROMOTE ELSE
   DUP >lhs @ CT SWAP >rhs @ CT T-UAC THEN  t_uint = ;
: ORD? ( op -- f )  DUP CMPOP? OVER oEQ <> AND SWAP oNE <> AND ;
: CUT? ( op t -- f )                     \ narrow: must this operand be masked?
   T-PROMOTE t_uint <> SWAP DUP CMPOP? SWAP DUP oDIV = SWAP oMOD = OR OR AND ;
\ does this operator have an unsigned operand (after promotion)?
: UNSBIN? ( n -- f )
   DUP >op @ DUP oSHL = SWAP oSHR = OR IF
      >lhs @ CT DUP T-ARITH? IF T-PROMOTE T-UNS? ELSE DROP 0 THEN EXIT THEN
   DUP >lhs @ CT SWAP >rhs @ CT T-UNSBIN? ;

\ ONE word, no locals, and the node kept on the DATA stack. Code
\ generation recurses with the expression, through this word and GEN
\ alone, so this is two return-stack cells a level -- what it was before
\ M4 -- and no locals frame at all. c4th's return stack is 256 cells
\ and checked; its locals stack is 512 and not (c4fc-design.md), and a
\ frame per level is what a 60-term unsigned expression overflowed.
:M GEN n_bin
   DUP UNSBIN? IF
      \ an unsigned operand: the usual arithmetic conversions
      DUP >lhs @ GEN
      DUP NARROW? IF DUP >op @ OVER >lhs @ CT CUT? IF M32 MASK, THEN
      ELSE DUP >op @ ORD? IF XMIN, THEN THEN
      oPSH OP,
      DUP >rhs @ GEN
      DUP NARROW? IF
         DUP >op @ OVER >rhs @ CT CUT? IF M32 MASK, THEN
         >op @ DUP OP,
         DUP oADD = OVER oSUB = OR OVER oMUL = OR OVER oSHL = OR OVER oOR = OR
         SWAP oXOR = OR IF M32 MASK, THEN
         EXIT THEN
      DUP >op @ ORD? IF XMIN, THEN
      DUP >op @ oDIV = IF DROP h_udiv 2 HCALL EXIT THEN
      DUP >op @ oMOD = IF DROP h_umod 2 HCALL EXIT THEN
      DUP >op @ oSHR = IF
         >rhs @ DUP NUM? IF
            >val @ oSHR OP,
            DUP 0> OVER WBITS < AND IF -1 WBITS ROT - LSHIFT INVERT MASK, ELSE DROP THEN
         ELSE DROP h_lsr 2 HCALL THEN
         EXIT THEN
      >op @ OP, EXIT
   THEN
   \ c4's own operators, as they always were
   DUP >lhs @ CT T-PTR? IF
      DUP >op @ oADD = IF
         DUP >lhs @ GEN oPSH OP,  DUP >rhs @ GEN  >lhs @ CT SCALE,  oADD OP, EXIT THEN
      DUP >op @ oSUB = IF
         DUP >rhs @ CT OVER >lhs @ CT = IF   \ pointer minus pointer is a count
            DUP >lhs @ GEN oPSH OP,  DUP >rhs @ GEN  oSUB OP,
            >lhs @ CT T-STEP DUP 1 = IF DROP EXIT THEN
            oPSH OP, oIMM OP2, oDIV OP, EXIT THEN
         DUP >lhs @ GEN oPSH OP,  DUP >rhs @ GEN  >lhs @ CT SCALE,  oSUB OP, EXIT THEN
   THEN
   DUP >lhs @ GEN  oPSH OP,  DUP >rhs @ GEN  >op @ OP, ;M

:M GEN-ADDR n_index {: n -- :}
   n >lhs @ GEN  oPSH OP,
   n >rhs @ GEN  n >lhs @ CT SCALE,
   oADD OP, ;M
:M GEN n_index {: n -- :}
   n CISC-INDEX? IF
      n >lhs @ GEN  oPSH OP,  n >rhs @ GEN  oLXI OP, EXIT THEN
   n GEN-ADDR  n CT LOAD, ;M
:M CT  n_index  >lhs @ CT T-DEREF ;M
:M CQ  n_index  >lhs @ CQ 2 RSHIFT ;M

:M GEN-ADDR n_member {: n -- :}
   n >lhs @ GEN
   n >moff @ ?DUP IF oPSH OP, oIMM OP2, oADD OP, THEN ;M
:M GEN n_member {: n -- :}
   n GEN-ADDR
   n >magg @ IF EXIT THEN               \ an array member is its address
   n >mtype @ LOAD, ;M
:M CT  n_member  >mtype @ ;M
\ a member inherits the qualifiers of the struct it is read out of; an
\ ARRAY member hands them on to its elements. >lhs is the struct's
\ ADDRESS (x.m is (&x)->m), so the struct's own level is level 1 there.
:M CQ  n_member {: n | sq -- q :}
   n >lhs @ CQ 2 RSHIFT 3 AND TO sq
   n >mq @  n >magg @ IF sq 2 LSHIFT ELSE sq THEN OR ;M

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
:M CQ n_comma {: n -- q :}  n >args @ n >argn @ 1- CELLS + @ CQ ;M
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

:M GEN n_call {: n | f k i -- :}
   \ The splice replaces the call entirely -- arguments into the
   \ callee's parameter slots, then its body generated in place. The
   \ value arrives the same way it would have: in A.
   n INL-TRY TO i
   i 0< 0= IF  n i INL-SPLICE  EXIT THEN
   n >fn @ TO f
   n >argn @ TO k
   k 0 ?DO
      n >args @ I CELLS + @ DUP GEN  I f ARGCHECK
      oPSH OP,
   LOOP
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

\ A call through an expression (M7b), as c4lc-gen's g:callexpr: the
\ arguments, then what JSR would have pushed -- the return address, an
\ IMM of the code just past the jump -- then the target in A and JMPA.
\ The callee finds the frame a JSR builds.
:M GEN n_icall {: n | k m -- :}
   n >argn @ TO k
   k 0 ?DO  n >args @ I CELLS + @ GEN  oPSH OP,  LOOP
   oIMM BR, TO m   oPSH OP,
   n >fn @ GEN   oJMPA OP,
   m >RES
   k ?DUP IF oADJ OP2, THEN ;M

\ goto and labels (M7b), by name and per function, as c4lc-gen's g:ulab.
\ A label's address is -1 until it is placed; a goto that gets there
\ first leaves a branch mark, resolved when the label is placed. Placing
\ a label tells LABEL@ about it, so a function ending on one gets the
\ LEV that c4lc gives it.
32 CONSTANT ULAB-MAX
CREATE ULA ULAB-MAX CELLS ALLOT   CREATE ULU ULAB-MAX CELLS ALLOT
CREATE ULADDR ULAB-MAX CELLS ALLOT
VARIABLE ULN   0 ULN !
256 CONSTANT UFWD-MAX
CREATE UFL UFWD-MAX CELLS ALLOT   CREATE UFM UFWD-MAX CELLS ALLOT
VARIABLE UFN   0 UFN !
: ULAB-RESET ( -- )  0 ULN !  0 UFN ! ;
: ULAB ( a u -- i ) {: a u -- i :}
   ULN @ 0 ?DO
      I CELLS ULU + @ u = IF
         I CELLS ULA + @ a u BYTES= IF I UNLOOP EXIT THEN
      THEN
   LOOP
   ULN @ ULAB-MAX < 0= IF ." c4fc: too many labels in one function" CR ABORT THEN
   a ULN @ CELLS ULA + !  u ULN @ CELLS ULU + !  -1 ULN @ CELLS ULADDR + !
   1 ULN +!  ULN @ 1- ;
: ULAB-CHECK ( -- )
   UFN @ 0 ?DO
      I CELLS UFL + @ CELLS ULADDR + @ 0< IF
         ." c4fc: goto to a label the function does not have: "
         I CELLS UFL + @ DUP CELLS ULA + @ SWAP CELLS ULU + @ TYPE CR ABORT
      THEN
   LOOP ;
:M STMT n_goto {: n | i t -- :}
   n >lname @ n >llen @ ULAB TO i
   i CELLS ULADDR + @ TO t
   t 0< IF
      UFN @ UFWD-MAX < 0= IF ." c4fc: too many gotos in one function" CR ABORT THEN
      oJMP BR,  UFN @ CELLS UFM + !  i UFN @ CELLS UFL + !  1 UFN +!
   ELSE oJMP t BACK, THEN ;M
:M STMT n_labeled {: n | i -- :}
   n >lname @ n >llen @ ULAB TO i
   i CELLS ULADDR + @ 0< 0= IF
      ." c4fc: duplicate label: " n >lname @ n >llen @ TYPE CR ABORT THEN
   CHERE i CELLS ULADDR + !   CHERE LABEL@
   UFN @ 0 ?DO  I CELLS UFL + @ i = IF I CELLS UFM + @ >RES THEN  LOOP
   n >body @ STMT ;M

\ A scalar takes SI even when it is a char, which is what c4cc did and
\ what c4lc kept; an array is stored element by element, the elements
\ past the initialiser list zeroed.
: LIBYTE, ( off idx v -- ) {: off i v -- :}
   off oLEA OP2,  oPSH OP,  i oIMM OP2,  oADD OP,  oPSH OP,
   v oIMM OP2,  oSC OP, ;
:M STMT n_linit {: n | y off v c s val -- :}
   n >isym @ TO y   y y.val @ TO off
   n >icount @ TO s
   s 0< IF
      off oLEA OP2,  oPSH OP,  n >expr @ GEN
      y y.ct @ T-PTR? IF
         y y.q @  n >expr @ CQ  DROPPED? IF S" initialization" QWARN THEN THEN
      oSI OP,  EXIT THEN
   n >ivals @ TO v   n >ivn @ TO c
   s 0 ?DO
      I c < IF I CELLS v + @ ELSE 0 THEN TO val
      n >ibyte @ 1 = IF
         off I val LIBYTE,
      ELSE n >ibyte @ 2 = IF               \ a 16-bit element: two bytes, low first
         off I 2* val 255 AND LIBYTE,
         off I 2* 1+ val 8 RSHIFT 255 AND LIBYTE,
      ELSE
         off I + oLEA OP2,  oPSH OP,  val oIMM OP2,  oSI OP,
      THEN THEN
   LOOP ;M

\ c4lc-gen's g:localaggr: MSET zeroes the object, then each value goes to
\ its offset -- LEA's own when it is word-aligned, else added to the base
: LADDR, ( base off -- ) {: b o -- :}
   o 1 CELLS 1- AND 0= IF b o 1 CELLS / + oLEA OP2, EXIT THEN
   b oLEA OP2,  oPSH OP,  o oIMM OP2,  oADD OP, ;
:M STMT n_lagg {: n | b v r e -- :}
   n >lsym @ y.val @ TO b
   b oLEA OP2,  oPSH OP,  0 oIMM OP2,  oPSH OP,  n >lbytes @ oIMM OP2,  oPSH OP,
   oMSET OP,  3 oADJ OP2,
   n >lrecs @ TO v
   v V# 0 ?DO
      I v V@ TO r
      b r @ LADDR,  oPSH OP,
      r 2 CELLS + @ TO e
      e IF
         e GEN
         e CT T-STRUCT? IF ." c4fc: a struct value in an initializer is not supported" CR ABORT THEN
      ELSE r 3 CELLS + @ oIMM OP2, THEN
      r CELL+ @ STORE,
   LOOP ;M

:M STMT n_expst  >expr @ GEN ;M
\ Inside a spliced body there is no frame to leave, so a return is a
\ jump to the end of the splice rather than a LEV. The value is already
\ in A either way, which is the whole reason this needs no rewriting of
\ the tree -- see inline.f.
:M STMT n_ret {: n | e -- :}
   n >expr @ TO e
   e IF
      e GEN
      e CT RETT @ SCONV,
      RETT @ T-PTR? IF RETQ @ e CQ DROPPED? IF S" return" QWARN THEN THEN
   THEN
   INLRET @ IF oJMP BR, INLM, ELSE oLEV OP, THEN ;M
:M STMT n_blk {: n -- :}
   n >len @ 0 ?DO n >list @ I CELLS + @ STMT LOOP ;M

\ -- unary --------------------------------------------------------------
\ Each is what c4 emits, and each is worth reading once: ! is a compare
\ against zero, ~ is an XOR with -1, and unary minus is a multiply by -1
\ with the -1 pushed FIRST, because C4's MUL takes its left operand off
\ the stack.

:M GEN n_not   >opnd @ GEN  oPSH OP,  0 oIMM OP2,  oEQ  OP, ;M
\ an unsigned operand stays unsigned, and a 64-bit host's uint is cut
\ back into its 32 bits
: UNCUT, ( n -- )  >opnd @ CT T-UNRES t_uint = W64? AND IF M32 MASK, THEN ;
:M GEN n_bnot {: n -- :}  n >opnd @ GEN  oPSH OP, -1 oIMM OP2,  oXOR OP,  n UNCUT, ;M
:M GEN n_neg  {: n -- :}  -1 oIMM OP2, oPSH OP,  n >opnd @ GEN  oMUL OP,  n UNCUT, ;M

\ A pointer's VALUE is the address it points at, so dereferencing for an
\ address is the operand's value and nothing else.
:M GEN-ADDR n_deref  >opnd @ GEN ;M
:M GEN      n_deref {: n -- :}  n >opnd @ GEN  n CT LOAD, ;M
:M GEN      n_addr   >opnd @ GEN-ADDR ;M

:M CT n_not   DROP t_int ;M
:M CT n_bnot  >opnd @ CT T-UNRES ;M
:M CT n_neg   >opnd @ CT T-UNRES ;M
:M CT n_addr  >opnd @ CT 2 + ;M
:M CT n_deref >opnd @ CT 2 - ;M
:M CQ n_not   DROP 0 ;M
:M CQ n_bnot  DROP 0 ;M
:M CQ n_neg   DROP 0 ;M
:M CQ n_addr  >opnd @ CQ 2 LSHIFT ;M
:M CQ n_deref >opnd @ CQ 2 RSHIFT ;M

\ ++ and -- reuse one address for both the load and the store, which is
\ why the sequence is LEA, PSH, LI rather than LEA, LI, PSH. The postfix
\ forms undo the change on the RESULT afterwards, which is exactly how
\ c4 gets the old value without a temporary.
\ The post forms store the new value unconverted and take the step back
\ off it: converting first would lose the carry the undo needs, and the
\ loads convert anyway.
: INCDEC, ( node op post -- ) {: n op post | t -- :}
   n >opnd @ CT TO t
   n >opnd @ GEN-ADDR
   n >opnd @ op oADD = IF S" increment" ELSE S" decrement" THEN WCHECK
   oPSH OP,
   t LOAD,
   oPSH OP,  t STEP-OF oIMM OP2,  op OP,
   post 0= IF t T-PROMOTE t SCONV, THEN
   t STORE, ;
:M GEN n_preinc   oADD 0 INCDEC, ;M
:M GEN n_predec   oSUB 0 INCDEC, ;M
:M GEN n_postinc {: n -- :}
   n oADD 1 INCDEC,  oPSH OP, n >opnd @ CT STEP-OF oIMM OP2, oSUB OP, ;M
:M GEN n_postdec {: n -- :}
   n oSUB 1 INCDEC,  oPSH OP, n >opnd @ CT STEP-OF oIMM OP2, oADD OP, ;M
:M CT n_preinc   >opnd @ CT ;M
:M CT n_predec   >opnd @ CT ;M
:M CT n_postinc  >opnd @ CT ;M
:M CT n_postdec  >opnd @ CT ;M
:M CQ n_preinc   >opnd @ CQ ;M
:M CQ n_predec   >opnd @ CQ ;M
:M CQ n_postinc  >opnd @ CQ ;M
:M CQ n_postdec  >opnd @ CQ ;M

\ -- short-circuit and the conditional ---------------------------------

:M GEN n_lor {: n | m -- :}
   n >lhs @ GEN   oBNZ BR, TO m   n >rhs @ GEN   m >RES ;M
:M GEN n_land {: n | m -- :}
   n >lhs @ GEN   oBZ  BR, TO m   n >rhs @ GEN   m >RES ;M
\ c4's ?: has its else-arm's type; with an unsigned arm in it both arms
\ convert to the usual arithmetic type instead, as C says.
: CONDU? ( n -- f )  DUP >body @ CT SWAP >else @ CT T-UNSBIN? ;
: CONDT ( n -- t )   DUP >body @ CT SWAP >else @ CT T-UAC ;
:M GEN n_cond {: n | m1 m2 -- :}
   n >cond @ GEN   oBZ BR, TO m1
   n >body @ GEN
   n CONDU? IF n >body @ CT n CONDT CONVERT, THEN
   oJMP BR, TO m2
   m1 >RES
   n >else @ GEN
   n CONDU? IF n >else @ CT n CONDT CONVERT, THEN
   m2 >RES ;M
:M CT n_lor  DROP t_int ;M
:M CT n_land DROP t_int ;M
:M CT n_cond {: n -- t :}  n CONDU? IF n CONDT ELSE n >else @ CT THEN ;M
:M CQ n_lor  DROP 0 ;M
:M CQ n_land DROP 0 ;M
:M CQ n_cond {: n -- q :}  n >body @ CQ  n >else @ CQ  OR ;M

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
\ NSYM, STAB, STN, NLOC, NGLO, TP and ST[] moved to emit.f, next to the
\ SYMR they are all about -- inline.f sits between emit.f and gen.f and
\ needs the symbol table and the frame counter to retarget a spliced
\ body's locals.
: ST, ( a u class val ct agg sz -- ) {: a u c v ct ag sz | y -- :}
   STN @ NSYM < 0= IF ." c4fc: symbol table full" CR ABORT THEN
   STN @ ST[] TO y
   a y y.name !  u y y.nlen !  1 y y.type !  c y y.class !  v y y.val !
   ct y y.ct !  ag y y.agg !  sz y y.sz !
   0 y y.atk !  0 y y.fwd !
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

\ L op= R is L = L op R, the lvalue's tree in both places (c4lc-parse's
\ p:compound): the binary operator an assignment operator stands for, or 0
: COMPOUND-OF ( kind -- kind' | 0 ) {: k -- k' :}
   k AddA = IF Add EXIT THEN  k SubA = IF Sub EXIT THEN  k MulA = IF Mul EXIT THEN
   k DivA = IF Div EXIT THEN  k ModA = IF Mod EXIT THEN  k ShlA = IF Shl EXIT THEN
   k ShrA = IF Shr EXIT THEN  k AndA = IF And EXIT THEN  k OrA = IF Or EXIT THEN
   k XorA = IF Xor EXIT THEN  0 ;

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
   \ long long is the word on a 64-bit host, as in c4lc. On a 32-bit one
   \ c4lc makes it two words (M8, c4lc-design 14.7); c4fc does not, and
   \ says so rather than give a long long 32 bits.
   lg 1 > 1 CELLS 4 = AND IF
      ." c4fc: " TOK@ .WHERE ." : long long on a 32-bit target needs c4lc" CR ABORT THEN
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
      ELSE TK COMPOUND-OF TO q  q 0<> lev 1 <= AND IF
         TNEXT  1 EXPR TO r
         n_bin 4 CELLS NEW TO nd
         n nd >lhs !  r nd >rhs !  q IN-FIND CELLS INO + @ nd >op !
         n nd n_asgn N2 TO n
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
      THEN THEN THEN THEN THEN
   AGAIN ;
' (EXPR) IS EXPR

\ -- statements ---------------------------------------------------------

DEFER STATEMENT                         \ blocks and statements nest
\ a local's brace list and its stores (M7b), with the global initializers
DEFER LOCAL-BRACES ( ag na -- ai iv ivn na' )
DEFER LOCAL-RECS ( ety cnt ai -- v )

\ An array or a struct takes as many frame slots as it takes cells, and
\ its name stands for the address of the LOWEST one -- locals grow
\ downwards, so int a[4] as the first local is slots 1..4 and a is
\ LEA -4.
: LOCAL-DECLS ( v -- ) {: v | ct dt t a u sz ag na iv ivn y nd nn bq q fn ai -- :}
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
         0 TO iv   0 TO ivn   0 TO nd   0 TO ai
         TK Assign = IF
            TNEXT
            TK Lbrace =  ag dt T-STRUCT? OR  AND IF
               ag na LOCAL-BRACES TO na TO ivn TO iv TO ai
            ELSE ag IF
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
            THEN THEN
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
         ai IF
            STN @ 1- ST[] TO y
            n_lagg 4 CELLS NEW TO nn
            y nn >lsym !   sz nn >lbytes !
            dt  ag IF na ELSE -1 THEN  ai LOCAL-RECS nn >lrecs !
            nn v V,
         THEN
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
\ c4lc-parse's p:exprleaf?: in a local's brace list, a leaf that starts
\ with &, or has a string, a type name, or a name no constant answers
\ before the next , or }, is an expression; a string to start is a string
VARIABLE LOCAL-INIT   0 LOCAL-INIT !
: EXPR-LEAF? ( -- f ) {: | i k d y tk -- f :}
   TK Str = IF 0 EXIT THEN
   TK And = IF -1 EXIT THEN
   TP @ TO i   0 TO d
   BEGIN
      i TOKS V@ TO tk   tk t.kind @ TO k
      k Eof = IF 0 EXIT THEN
      d 0=  k Comma = k Rbrace = OR k Semi = OR  AND IF 0 EXIT THEN
      k Str = IF -1 EXIT THEN
      k KIND-TYPE? IF -1 EXIT THEN
      k Id = IF
         tk t.val @ tk t.len @ ST-FIND TO y
         y 0= IF -1 EXIT THEN
         y y.class @ c_const = y y.cv @ OR 0= IF -1 EXIT THEN
      THEN
      k Lparen = k Brak = OR IF d 1+ TO d THEN
      k Rparen = k Rbrak = OR IF d 1- TO d THEN
      i 1+ TO i
   AGAIN ;
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
   LOCAL-INIT @ IF EXPR-LEAF? IF 6 1 EXPR 0 0 INIT-NODE EXIT THEN THEN
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

\ a local's stores (M7b): AMODE 2 records each value where it goes, as
\ (offset type node value) -- an expression node, or 0 and a number --
\ for gen.f's n_lagg to store when the declaration runs
VARIABLE LREC
: LREC, ( off ty node v -- ) {: o t nd v | r -- :}
   4 CELLS ALLOT: TO r  o r !  t r CELL+ !  nd r 2 CELLS + !  v r 3 CELLS + !
   r LREC @ V, ;
: AGG-PUT ( v ty off -- ) {: v t o -- :}
   t T-BYTE? IF v o ID-C! EXIT THEN
   t T-HALF? IF v 255 AND o ID-C!  v 8 RSHIFT 255 AND o 1+ ID-C! EXIT THEN
   v o ID-! ;
: AGG-SCALAR ( ty off node -- ) {: t o nd | k y -- :}
   nd @ TO k
   AMODE @ 2 = IF
      k 0 = IF  o t 0 nd CELL+ @ LREC,  EXIT THEN
      k 1 = IF  o t  nd CELL+ @ nd 2 CELLS + @ n_str N2  0 LREC,  EXIT THEN
      k 6 = IF  o t  nd CELL+ @  0 LREC,  EXIT THEN
      ." c4fc: unsupported initializer element" CR ABORT THEN
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
   AMODE @ 2 = IF
      nd 2 CELLS + @ 0 ?DO  o I + t_char 0 nd CELL+ @ I + C@ LREC,  LOOP EXIT THEN
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

\ a local's brace list (M7b), c4lc-parse's p:linit. An array's list of
\ plain numbers keeps the element-by-element stores it always had (iv
\ ivn, and no tree); anything else is a tree for LOCAL-RECS
: ALL-NUMS? ( node -- f ) {: nd | v f -- f :}
   nd CELL+ @ TO v   -1 TO f
   v V# 0 ?DO I v V@ @ IF 0 TO f THEN LOOP f ;
: (LOCAL-BRACES) ( ag na -- ai iv ivn na' ) {: ag na | ai lv iv ivn -- ai iv ivn na :}
   1 LOCAL-INIT !  INIT-BRACES TO ai  0 LOCAL-INIT !
   ag IF ai ALL-NUMS? IF
      ai CELL+ @ TO lv   lv V# TO ivn   ivn 1 MAX CELLS ALLOT: TO iv
      ivn 0 ?DO I lv V@ CELL+ @ iv I CELLS + ! LOOP
      na 0< IF ivn TO na THEN
      0 iv ivn na EXIT THEN THEN
   ag na 0< AND IF ai AGG-COUNT TO na THEN
   ai 0 0 na ;
' (LOCAL-BRACES) IS LOCAL-BRACES
\ ... and its stores, walked as c4lc-gen's g:localaggr walks it
: (LOCAL-RECS) ( ety cnt ai -- v )
   16 NEW-VEC LREC !  2 AMODE !  0 SWAP AGG-TOP  0 AMODE !  LREC @ ;
' (LOCAL-RECS) IS LOCAL-RECS

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
         \ read the wrong one. An initialised global is a definition the
         \ discovery pass records too, so that an extern of it above is
         \ not taken for another unit's.
         COLLECT @ IF a u DG-DEF THEN
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
         \ Until then x is a forward record, a global's number for the
         \ code in between to name, which the definition takes over --
         \ as c4lc, which allocates in a pre-pass that skips the extern.
         0 TO fx
         COLLECT @ 0= sc -1 = AND IF
            OBJECT @ TO fx
            a u ST-FIND 0= IF
               OBJECT @ IF a u EXT-FIND ELSE -1 THEN TO fk
               fk 0< 0= IF
                  a u c_extg fk t ag sz ST,
                  STN @ 1- ST[] TO y   0 y y.sc !   q y y.q !
               ELSE a u DG-DEFINED? IF
                  a u c_glo NGLO @ t ag sz ST,
                  STN @ 1- ST[] TO y   0 y y.ini !  0 y y.sc !   q y y.q !
                  1 y y.fwd !   1 NGLO +!   1 TO fx
               THEN THEN
            THEN
         THEN
         \ extern int x; int x;  is one global, not two -- a repeated
         \ declaration of a name already known is the same object. A
         \ forward record retires, and its number moves here with the
         \ name, where c4lc allocates x and lists its symbol.
         fx 0= IF
            a u ST-FIND TO y   NGLO @ TO fk
            y IF y y.class @ c_glo <> IF 0 TO y THEN THEN
            y IF y y.fwd @ IF
               y y.val @ TO fk   c_gone y y.class !   0 y y.nlen !   0 TO y
            THEN THEN
            y 0= IF
               a u c_glo fk t ag sz ST,
               STN @ 1- ST[] TO y   sc 0 MAX y y.sc !   q y y.q !
               fk NGLO @ = IF 1 NGLO +! THEN
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
\ -fno-builtin: the library names are ordinary identifiers, so a C
\ library can define them (c4lc's gen:nobuiltin). The machine's own
\ primitives, from install_trap_handler on, stay builtins.
VARIABLE NOBUILTIN   0 NOBUILTIN !
: -fno-builtin ( -- )  1 NOBUILTIN ! ;
: LIBC-BUILTINS
   S" open"   oOPEN BUILTIN   S" read"    oREAD BUILTIN
   S" close"  oCLOS BUILTIN   S" printf"  oPRTF BUILTIN
   S" malloc" oMALC BUILTIN   S" free"    oFREE BUILTIN
   S" memset" oMSET BUILTIN   S" memcmp"  oMCMP BUILTIN
   S" exit"   oEXIT BUILTIN   S" putchar" oPUTC BUILTIN
   S" puts"   oPUTS BUILTIN   S" realloc" oRALC BUILTIN
   S" memcpy" oMCPY BUILTIN   S" stacktrace" oSTRC BUILTIN ;
: BUILTINS
   NOBUILTIN @ 0= IF LIBC-BUILTINS THEN
   S" install_trap_handler" oITH BUILTIN
   S" __opcode" o_OPC BUILTIN  S" __builtin" o_BLT BUILTIN
   S" __c4_trap" o_TRP BUILTIN S" __c4_opcode" oOPCD BUILTIN
   S" __c4_jmp" o_JMP BUILTIN  S" __c4_adjust" o_ADJ BUILTIN
   S" __c4_configure" oC4CF BUILTIN  S" __c4_cycles" oC4CY BUILTIN
   S" __time" oTIME BUILTIN    S" __c4_signal" oSIGH BUILTIN
   S" __c4_sigint" oSIGI BUILTIN     S" __c4_usleep" oUSLP BUILTIN
   S" __c4_info" oINFO BUILTIN S" __c4_ops_list" oOPSL BUILTIN
   S" __c4_invoke" oC4IV BUILTIN     S" __c4_float" oFLT BUILTIN
   S" __c4_fpu" oFPU BUILTIN
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
\ -minline: splice a small function's body into its call sites instead
\ of calling it (docs/inline-small-functions.md). Unlike -mfuse this
\ needs no opcode the target lacks, so a -minline image runs wherever
\ its -O build would. Implies -O.
: -minline ( -- )  1 INLINING !  1 OPTIMIZE ! ;

\ Everything a compile of one unit starts from. -O runs it twice: the
\ first pass exists only to learn which functions are reachable, and
\ throws its buffers away.
: UNIT-RESET ( -- )
   EMIT-RESET
   NSYM SYMR * ALLOCATE STAB !  0 STN !  0 NGLO !
   0 #STRUCTS !  0 #MEMS !  0 VA-MAKE !
   0 TP !
   HELP-RESET
   BUILTINS ;

\ What c4 has no use for (M7b), as c4lc-parse's p:untag: `inline`,
\ `restrict` and `__extension__` in their spellings, and every
\ __attribute__((...)) that names neither constructor nor destructor.
\ TOKS is compacted in place before anything parses it.
: DROP-ID? ( t -- f ) {: t | a u -- f :}
   t t.kind @ Id <> IF 0 EXIT THEN
   t t.val @ TO a  t t.len @ TO u
   a u S" inline" NAME=?  a u S" __inline" NAME=? OR  a u S" __inline__" NAME=? OR
   a u S" restrict" NAME=? OR  a u S" __restrict" NAME=? OR
   a u S" __restrict__" NAME=? OR  a u S" __extension__" NAME=? OR ;
: UNTAG ( -- ) {: | i j n t e d keep k -- :}
   0 TO i  0 TO j  TOKS V# TO n
   BEGIN i n < WHILE
      i TOKS V@ TO t
      t DROP-ID? IF i 1+ TO i ELSE
      t t.kind @ Attribute = IF
         i 1+ TO e   0 TO d   0 TO keep
         BEGIN
            e n < IF
               e TOKS V@ t.kind @ TO k
               k Lparen = IF d 1+ TO d THEN
               k Rparen = IF d 1- TO d THEN
               k Constructor = k Destructor = OR IF -1 TO keep THEN
               e 1+ TO e
               d 0= k Rparen = AND
            ELSE -1 THEN
         UNTIL
         keep IF t j TOKS V!  j 1+ TO j  i 1+ TO i ELSE e TO i THEN
      ELSE t j TOKS V!  j 1+ TO j  i 1+ TO i THEN THEN
   REPEAT
   j TOKS v.len ! ;

: C4FC ( a u -- )                       \ compile that file, image to stdout
   C4FC-READY @ 0= IF C4FC-INIT THEN
   PREPROCESS @ IF PP-FILE ELSE LEX-FILE THEN
   UNTAG
   \ The discovery pass, which runs whatever the flags say. -O wants the
   \ call graph and -c wants to know which names this unit defines, but
   \ the plain compile wants something from it too: a call to a function
   \ defined further down with no prototype anywhere resolves only if
   \ something has read ahead. Making that depend on -O would mean
   \ `c4fc f.c` rejecting a file `c4fc -O f.c` compiles.
   T2-RESET  DECL-RESET  INL-RESET  INLM-RESET  0 EXTN !  1 COLLECT !
   UNIT-RESET PROGRAM
   T2-CLOSE  MAKE-EXTERNS  0 COLLECT !
   UNIT-RESET PREREGISTER PROGRAM
   OBJECT @ 0= ENTRY @ 0< AND IF ." c4fc: no main" CR ABORT THEN
   OPTIMIZE @ IF OPT-RUN THEN
   OBJECT @ IF SN @ FIX-EXTERNS  EXT-SYMS THEN
   WRITE-IMAGE ;
