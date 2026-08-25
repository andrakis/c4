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

262144 CONSTANT CMAX
 65536 CONSTANT DMAX
  8192 CONSTANT PMAX
  4096 CONSTANT SMAX

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
38 CONSTANT oEXIT 63 CONSTANT oJMPA

: EMIT-INIT
   CMAX CELLS ALLOCATE CODE !      0 CN !
   DMAX ALLOCATE DATA !            0 DN !
   DMAX ALLOCATE IDATA !           0 IDN !
   0 UDN !  0 DB2 !  0 DB3 !
   256 CELLS ALLOCATE CONS !       0 CONSN !
   256 CELLS ALLOCATE DESS !       0 DESN !
   IDATA @ DMAX 0 FILL
   PMAX 3 * CELLS ALLOCATE PATCH ! 0 PN !
   SMAX 5 * CELLS ALLOCATE SYMS !  0 SN !
   DATA @ DMAX 0 FILL
   -1 ENTRY ! ;

\ -- code ---------------------------------------------------------------

: C, ( w -- )
   CN @ CMAX < 0= IF ." c4fc: code overflow" CR ABORT THEN
   CODE @ CN @ CELLS + !  1 CN +! ;
: OP,   ( op -- )    C, ;
: OP2,  ( n op -- )  C, C, ;
: CHERE ( -- n )     CN @ ;
: LAST-OP ( -- w )   CN @ 0= IF -1 EXIT THEN CODE @ CN @ 1- CELLS + @ ;

: PAT, ( type addr value -- ) {: t a v | p -- :}
   PN @ PMAX < 0= IF ." c4fc: patch overflow" CR ABORT THEN
   PN @ 3 * CELLS PATCH @ + TO p
   t p !   a p CELL+ !   v p 2 CELLS + !
   1 PN +! ;

4096 CONSTANT DSMAX
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
: IMMI, ( off -- ) {: off -- :}         \ IMM of a REGION 1 address, final
   oIMM OP,  -2 CHERE off PAT,  0 C, ;
: JSRC, ( target -- ) {: t -- :}        \ a call to a known function
   oJSR OP,  -1 CHERE t PAT,  t C, ;

\ A jump table's entries are code addresses living in DATA, which is
\ patch type -3. They are collected rather than emitted as they are
\ found, because c4lc writes every one of them after every code patch
\ and c4r.lisp walks the patch list against the instruction stream.
1024 CONSTANT TABMAX
CREATE TABD TABMAX CELLS ALLOT
CREATE TABC TABMAX CELLS ALLOT
VARIABLE TABN   0 TABN !
: TABPAT, ( dataoff code -- )
   TABN @ TABMAX < 0= IF ." c4fc: too many jump table entries" CR ABORT THEN
   TABC TABN @ CELLS + !   TABD TABN @ CELLS + !   1 TABN +! ;
: EMIT-TABPATS ( base -- )              \ jump tables live in region 2 too
   TABN @ 0 ?DO DUP TABD I CELLS + @ +  TABC I CELLS + @  -3 ROT ROT PAT, LOOP
   DROP ;

\ A call to a function that has only been PROTOTYPED does not know where
\ it will land, so every call to a user function is recorded and fixed
\ when the whole program has been read. Both halves need fixing: the
\ patch's value and the code word, because a code reference carries its
\ target in both.
2048 CONSTANT FWMAX
CREATE FWP FWMAX CELLS ALLOT
CREATE FWS FWMAX CELLS ALLOT
VARIABLE FWN   0 FWN !
: JSRF, ( sym -- ) {: y -- :}
   oJSR OP,  -1 CHERE 0 PAT,  0 C,
   PN @ 1- FWP FWN @ CELLS + !   y FWS FWN @ CELLS + !   1 FWN +! ;
\ Branches carry a code patch exactly as calls do, so a forward branch
\ has two things to fill in later -- the patch's value and the code word
\ -- and what it carries around meanwhile is its PATCH index.
: BR, ( op -- mark ) {: op -- m :}
   op OP,  -1 CHERE 0 PAT,  0 C,  PN @ 1- ;
: RESTO ( mark target -- ) {: m t | p -- :}
   m 3 * CELLS PATCH @ + TO p
   t p 2 CELLS + !
   t p CELL+ @ CELLS CODE @ + ! ;
: >RES ( mark -- )  CHERE RESTO ;
: BACK, ( op target -- ) {: op t -- :}
   op OP,  -1 CHERE t PAT,  t C, ;

\ Region 1: an initialised global's address IS its offset, because that
\ region starts at zero.
: ID-ALLOT ( n -- off )
   IDN @ 1 CELLS 1- + 1 CELLS 1- INVERT AND IDN !
   IDN @ SWAP IDN +! ;
: ID-C! ( c off -- )  IDATA @ + C! ;
: ID-! ( v off -- )   IDATA @ + ! ;

\ A global's address is not known until every string literal has been
\ seen, because c4lc lays the globals out AFTER them -- so the patch is
\ recorded with the global's number and revisited at the end.
1024 CONSTANT GPMAX
CREATE GPL GPMAX CELLS ALLOT   VARIABLE GPN   0 GPN !
: FIX-REGION2 ( base -- ) {: b | p -- :}   \ shift every region 2 reference
   DSN @ 0 ?DO
      DSL I CELLS + @ 3 * CELLS PATCH @ + 2 CELLS + TO p
      p @ b + p !
   LOOP ;
: IMMG, ( slot -- ) {: slot -- :}
   oIMM OP,  -2 CHERE slot PAT,  0 C,
   PN @ 1- GPL GPN @ CELLS + !  1 GPN +! ;

\ -- data ---------------------------------------------------------------

: D-ALIGN  DN @ 1 CELLS 1- + 1 CELLS 1- INVERT AND DN ! ;
: D-ALLOT ( n -- off )  DN @ SWAP DN +! ;
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
BEGIN-STRUCTURE SYMR
   FIELD: y.name  FIELD: y.nlen  FIELD: y.type  FIELD: y.class  FIELD: y.val
   FIELD: y.ct    FIELD: y.agg   FIELD: y.sz
   FIELD: y.va    FIELD: y.nfix  FIELD: y.ini   FIELD: y.sc
END-STRUCTURE
VARIABLE VA-MAKE   0 VA-MAKE !          \ code index of __c4cc_make_va
: SYM[] ( i -- a )  SYMR * SYMS @ + ;
\ attrs 0x40 marks an AGGREGATE -- an array, or a struct variable. Both
\ are names that stand for an address rather than a value, and c4lc
\ flags them the same way.
64 CONSTANT ATTR-ARRAY
: SYM, ( a u type class val attrs -- ) {: a u t c v at | y -- :}
   SN @ SMAX < 0= IF ." c4fc: too many symbols" CR ABORT THEN
   SN @ SYM[] TO y
   a y y.name !  u y y.nlen !  t y y.type !  c y y.class !  v y y.val !
   at y y.agg !
   1 SN +! ;

: FIX-FORWARDS {: | p v -- :}
   FWN @ 0 ?DO
      FWP I CELLS + @ 3 * CELLS PATCH @ + TO p
      FWS I CELLS + @ y.val @ TO v
      v 0< IF ." c4fc: a function was called but never defined" CR ABORT THEN
      v p 2 CELLS + !
      v p CELL+ @ CELLS CODE @ + !
   LOOP ;


\ -- writing it out -----------------------------------------------------

CREATE WSCR 1 CELLS ALLOT
: FW  ( w -- )  WSCR !  1 CELLS 0 ?DO WSCR I + C@ EMIT LOOP ;
: FMK ( c -- )  EMIT  1 CELLS 1- 0 ?DO 0 EMIT LOOP ;
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
   67 EMIT 52 EMIT 82 EMIT               \ "C4R"
   3 EMIT                                \ version 3, as c4lc writes
   1 CELLS 8 * EMIT
   MEMSZ FW                              \ v3 puts memsz in v2's padding
   ENTRY @ FW   CN @ FW   dl FW   PN @ FW   SN @ FW   CONSN @ FW   DESN @ FW
   67 FMK  CN @ 0 ?DO I CELLS CODE @ + @ FW LOOP
   68 FMK  dl 0 ?DO I DBYTE EMIT LOOP
   80 FMK  PN @ 3 * 0 ?DO I CELLS PATCH @ + @ FW LOOP
   99 FMK  CONSN @ 0 ?DO I CELLS CONS @ + @ FW LOOP
  100 FMK  DESN @ 0 ?DO I CELLS DESS @ + @ FW LOOP
   83 FMK
   SN @ 0 ?DO
      I SYM[] TO y
      I FW                               \ id
      y y.type @ FW   y y.class @ FW   y y.agg @ FW
      y y.nlen @ EMIT   y y.name @ y y.nlen @ TYPE
      y y.val @ FW
   LOOP ;
