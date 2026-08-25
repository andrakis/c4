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

VARIABLE CODE   VARIABLE CN
VARIABLE DATA   VARIABLE DN
VARIABLE PATCH  VARIABLE PN
VARIABLE SYMS   VARIABLE SN
VARIABLE ENTRY

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

: EMIT-INIT
   CMAX CELLS ALLOCATE CODE !      0 CN !
   DMAX ALLOCATE DATA !            0 DN !
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

\ A patched operand carries its value in the patch, and what the CODE
\ word holds depends on which kind it is: a data reference writes ZERO
\ there, a code reference writes the target. The asymmetry is real and
\ neither half is visible in a small program -- the data rule shows up
\ the moment there are two string literals (the second's IMM is 0 in the
\ code and 3 in the patch), the code rule the moment a call target is
\ not function zero.
: IMMD, ( off -- ) {: off -- :}         \ IMM of an address in the data
   oIMM OP,  -2 CHERE off PAT,  0 C, ;
: JSRC, ( target -- ) {: t -- :}        \ a call to a known function
   oJSR OP,  -1 CHERE t PAT,  t C, ;

\ A global's address is not known until every string literal has been
\ seen, because c4lc lays the globals out AFTER them -- so the patch is
\ recorded with the global's number and revisited at the end.
1024 CONSTANT GPMAX
CREATE GPL GPMAX CELLS ALLOT   VARIABLE GPN   0 GPN !
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

: FIX-GLOBALS ( base -- ) {: base | p -- :}
   GPN @ 0 ?DO
      GPL I CELLS + @ 3 * CELLS PATCH @ + 2 CELLS + TO p
      base p @ CELLS + p !
   LOOP ;

\ -- symbols ------------------------------------------------------------

\ y.type and y.class are the .c4r symbol record's fields; y.ct is the C
\ TYPE, which the image format has no room for and the compiler cannot
\ do without -- char and int differ by one opcode at every load and
\ every store.
BEGIN-STRUCTURE SYMR
   FIELD: y.name  FIELD: y.nlen  FIELD: y.type  FIELD: y.class  FIELD: y.val
   FIELD: y.ct
END-STRUCTURE
: SYM[] ( i -- a )  SYMR * SYMS @ + ;
: SYM, ( a u type class val -- ) {: a u t c v | y -- :}
   SN @ SMAX < 0= IF ." c4fc: too many symbols" CR ABORT THEN
   SN @ SYM[] TO y
   a y y.name !  u y y.nlen !  t y y.type !  c y y.class !  v y y.val !
   1 SN +! ;

\ -- writing it out -----------------------------------------------------

CREATE WSCR 1 CELLS ALLOT
: FW  ( w -- )  WSCR !  1 CELLS 0 ?DO WSCR I + C@ EMIT LOOP ;
: FMK ( c -- )  EMIT  1 CELLS 1- 0 ?DO 0 EMIT LOOP ;
: MEMSZ ( -- n )  DN @ 1 CELLS 1- + 1 CELLS 1- INVERT AND ;
: DTRIM ( -- n )                        \ bytes worth writing
   MEMSZ BEGIN DUP 0> IF DUP 1- DATA @ + C@ 0= ELSE 0 THEN WHILE 1- REPEAT ;

: WRITE-IMAGE {: | dl y -- :}
   DTRIM TO dl
   67 EMIT 52 EMIT 82 EMIT               \ "C4R"
   3 EMIT                                \ version 3, as c4lc writes
   1 CELLS 8 * EMIT
   MEMSZ FW                              \ v3 puts memsz in v2's padding
   ENTRY @ FW   CN @ FW   dl FW   PN @ FW   SN @ FW   0 FW   0 FW
   67 FMK  CN @ 0 ?DO I CELLS CODE @ + @ FW LOOP
   68 FMK  dl 0 ?DO I DATA @ + C@ EMIT LOOP
   80 FMK  PN @ 3 * 0 ?DO I CELLS PATCH @ + @ FW LOOP
   99 FMK
  100 FMK
   83 FMK
   SN @ 0 ?DO
      I SYM[] TO y
      I FW                               \ id
      y y.type @ FW   y y.class @ FW   0 FW
      y y.nlen @ EMIT   y y.name @ y y.nlen @ TYPE
      y y.val @ FW
   LOOP ;
