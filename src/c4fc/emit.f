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
