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
1024 CONSTANT TABMAX
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
: TABPAT, ( slot code -- )   1 -3 (TABPAT) ;
\ `int *fp = &fn;`: a region 1 slot holding a code address
: FNPAT,  ( slot code -- )   0 -3 (TABPAT) ;
\ `char *s = "...";`: a region 1 slot holding the address of a string
\ that is ALSO in region 1 -- see ID-STR, below for why.
: STRPAT, ( slot off -- )    0 -4 (TABPAT) ;
: EMIT-TABPATS ( base -- ) {: b | f -- :}
   TABN @ 0 ?DO
      TABB I CELLS + @ TO f
      TABD I CELLS + @  f 1 AND IF b + THEN
      TABC I CELLS + @  f 2 AND IF b + THEN
      TABT I CELLS + @  ROT ROT PAT,
   LOOP ;

\ A call to a function that has only been PROTOTYPED does not know where
\ it will land, so every call to a user function is recorded and fixed
\ when the whole program has been read. Both halves need fixing: the
\ patch's value and the code word, because a code reference carries its
\ target in both.
2048 CONSTANT FWMAX
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
   t p CELL+ @ CELLS CODE @ + ! ;
: >RES ( mark -- )  CHERE RESTO ;
: BACK, ( op target -- ) {: op t -- :}
   op OP,  -1 CHERE t PAT,  t C, ;

\ Region 1: an initialised global's address IS its offset, because that
\ region starts at zero.
: ID-ALLOT ( n -- off )
   IDN @ 1 CELLS 1- + 1 CELLS 1- INVERT AND IDN !
   IDN @ SWAP IDN +! ;
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
1024 CONSTANT GPMAX
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
