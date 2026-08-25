\ c4th c4r.f -- the metacompiler: write a real .c4r image.
\
\ This is what turns c4th from a REPL with a code generator into a
\ compiler. native.f emits C4 instructions into ASMBUF; this copies them
\ into a target image, records every intra-image pointer as it goes, and
\ writes the result in the format load-c4r.c already reads.
\
\ ONE VOCABULARY, not a two-vocabulary cross compiler: host and target
\ are the same word size and the same machine, so a target word is an
\ ordinary c4th word that happens to have been copied into the image.
\
\ Two facts from load-c4r.c settle the design:
\
\   *(code + addr) = (int)(code + val)     a code reference   (type -1)
\   *(code + addr) = (int)(data + val)     a data reference   (type -2)
\
\ so a reference only has to be RECORDED, never computed -- which is why
\ "record every intra-image pointer while building into a buffer" and
\ "produce a patch table" are the same activity. And the entry point is
\ called as entry(argc, argv), an ordinary C4 function, which is what
\ B5c's calling convention already emits.
\
\ Writing the file needs SAVE-FILE, which needs a native build: the C4 VM
\ has no write syscall. Under c4m an image can still be built and
\ inspected in memory.

\ -- the target image ---------------------------------------------------

16384 CONSTANT TCMAX                  \ code words
 8192 CONSTANT TDMAX                  \ data bytes
 2048 CONSTANT TPMAX                  \ patches
  128 CONSTANT TVMAX                  \ target variables

CREATE TCODE TCMAX CELLS ALLOT   VARIABLE TCP    \ next free code word
CREATE TDATA TDMAX ALLOT         VARIABLE TDP    \ next free data byte
CREATE TPAT  TPMAX 3 * CELLS ALLOT  VARIABLE TPN
CREATE TVAR  TVMAX 3 * CELLS ALLOT  VARIABLE TVN \ host body, data offset, length
VARIABLE TENTRY                                  \ entry word index, or -1
VARIABLE TOK                                     \ cleared by a failed compile

-1 CONSTANT LT-CODE
-2 CONSTANT LT-DATA

\ Code word 0 is never an instruction. The .c4r code stream is 1-BASED,
\ because c4cc emits through *++e -- src/oisc4/oisc4.c:1108 says so, and
\ c4rdump assumes it too. Starting at 0 produces an image c4m happens to
\ run correctly (its entry index is right either way) and that every
\ tool which WALKS the stream mis-decodes from the first word. Reserving
\ the word here is the whole fix.
: TRESET  1 TCP !  0 TDP !  0 TPN !  0 TVN !  -1 TENTRY !  1 TOK ! ;
TRESET

: TC, ( w -- )                        \ append a code word
   TCP @ TCMAX < IF  TCP @ CELLS TCODE + !  1 TCP +!
   ELSE DROP ." c4r: code overflow" CR THEN ;

: TPAT, ( type addr value -- )
   TPN @ TPMAX < IF
      TPN @ 3 * CELLS TPAT +          ( type addr value p )
      DUP >R  2 CELLS + !             \ value
      R@ 1 CELLS + !                  \ addr
      R> !                            \ type
      1 TPN +!
   ELSE 2DROP DROP ." c4r: patch overflow" CR THEN ;

\ Data space. Cell-aligned, because everything c4th puts there is a cell.
: TD-ALLOT ( n -- off )
   TDP @ SWAP OVER + TDP !
   TDP @ TDMAX > IF ." c4r: data overflow" CR THEN ;

\ -- target variables ---------------------------------------------------
\ A target VARIABLE is an ordinary c4th VARIABLE -- so the compiler can
\ use it at compile time, and the backend compiles a reference to it as
\ one IMM exactly as before -- plus an entry in a map from its host body
\ address to its offset in the image's data segment. The relocation walk
\ turns that IMM into a data reference.
\
\ The map carries the length as well, which buys initialized data for
\ nothing: TSYNC copies each one's host bytes into the image just before
\ it is written, so whatever the host holds at save time is what the
\ image starts with. `TVARIABLE SEED 12345 SEED !` needs no more
\ machinery than that.

: TV[] ( i -- a )  3 * CELLS TVAR + ;
: TVAR! ( host off len -- )
   TVN @ TVMAX < IF
      TVN @ TV[]  DUP >R  2 CELLS + !   \ len
      R@ 1 CELLS + !                    \ off
      R> !                              \ host
      1 TVN +!
   ELSE 2DROP DROP ." c4r: too many target variables" CR THEN ;

: TVAR@ ( host -- off | -1 )
   TVN @ 0 ?DO
      DUP I TV[] @ = IF DROP I TV[] 1 CELLS + @ UNLOOP EXIT THEN
   LOOP DROP -1 ;

\ Copy every target variable's current contents into the image.
: TSYNC ( -- )
   TVN @ 0 ?DO
      I TV[] @                          ( host )
      I TV[] 1 CELLS + @ TDATA +        ( host dst )
      I TV[] 2 CELLS + @  MOVE
   LOOP ;

: TBUFFER: ( n "name" -- )              \ n bytes of target data
   CREATE  DUP ALLOT
   LATEST >BODY SWAP  DUP TD-ALLOT SWAP  ( body off len )
   TVAR! ;

: TVARIABLE  ( "name" -- )  1 CELLS TBUFFER: ;

\ The format string EMIT compiles against. Three bytes of target data --
\ '%', 'c', nul -- and native.f is told where they are; without that it
\ declines EMIT, because where the string lives is the metacompiler's
\ business and not the code generator's.
3 TBUFFER: TFMTC
37 TFMTC C!   99 TFMTC 1+ C!   0 TFMTC 2 + C!
TFMTC NFMTC !

\ -- relocation ---------------------------------------------------------
\ Walk the instructions native.f just emitted, copying them into the
\ image. An operand is a reference in exactly two cases: a branch or call
\ target, which is an address inside ASMBUF and becomes a code-relative
\ patch; and an IMM of a target variable's body, which becomes a
\ data-relative one. Everything else is a value and is copied.

VARIABLE TBASE  VARIABLE TOP  VARIABLE TARG  VARIABLE TAT

: CODEREF? ( op -- f )  DUP #JMP >= SWAP #BNZ <= AND ;   \ JMP JSR BZ BNZ

: TRELOC1 ( a -- a' )
   DUP @ TOP !
   TOP @ TC,
   TOP @ HAS-OPERAND? 0= IF 1 CELLS + EXIT THEN
   DUP 1 CELLS + @ TARG !
   TCP @ TAT !                        \ where this operand will land
   TOP @ CODEREF? IF
      TARG @ ASMBUF - 1 CELLS / TBASE @ +
      DUP TC,  LT-CODE TAT @ ROT TPAT,
   ELSE
      TOP @ #IMM = IF TARG @ TVAR@ ELSE -1 THEN     ( off | -1 )
      DUP 0 >= IF
         DUP TC,  LT-DATA TAT @ ROT TPAT,
      ELSE DROP TARG @ TC, THEN
   THEN
   2 CELLS + ;

: TRELOC ( -- start )
   TCP @ DUP TBASE !
   ASMBUF
   BEGIN DUP ASM-HERE < WHILE TRELOC1 REPEAT
   DROP ;

\ -- compiling a word into the image ------------------------------------

: TCOMPILE ( xt n -- ok? )           \ n = entry arguments
   >R DUP >BODY SWAP >WEND R> NCOMPILE-N
   DUP IF TRELOC DROP ELSE 0 TOK ! THEN ;

: TMAIN ( xt -- ok? )                \ compile it as the image's entry
   TCP @ TENTRY !
   0 TCOMPILE ;

\ -- writing the file ---------------------------------------------------
\ Words go out byte by byte, in the host's own order, because the header
\ is thirteen bytes long and everything after it is therefore unaligned
\ in the file. c4cc writes it the same way, one write() per field.

262144 CONSTANT FMAX
CREATE FBUF FMAX ALLOT   VARIABLE FP
CREATE FSCR 1 CELLS ALLOT

: FC, ( c -- )
   FP @ FMAX < IF FP @ FBUF + C!  1 FP +! ELSE DROP THEN ;
: FW, ( w -- )  FSCR !  1 CELLS 0 ?DO FSCR I + C@ FC, LOOP ;
: FMARK ( c -- )  FC,  1 CELLS 1- 0 ?DO 0 FC, LOOP ;

: TBUILD ( -- )                       \ lay the whole image out in FBUF
   0 FP !
   67 FC, 52 FC, 82 FC,               \ "C4R"
   2 FC,                              \ version 2
   1 CELLS 8 * FC,                    \ word bits
   8 0 ?DO 112 FC, LOOP               \ 8 bytes of 'p' padding
   TSYNC
   TENTRY @ FW,
   TCP @ FW,                          \ code length, in words
   TDP @ FW,                          \ data length, in bytes
   TPN @ FW,                          \ patches
   0 FW,  0 FW,  0 FW,                \ symbols, constructors, destructors
   67 FMARK  TCP @ 0 ?DO I CELLS TCODE + @ FW, LOOP
   68 FMARK  TDP @ 0 ?DO I TDATA + C@ FC, LOOP
   80 FMARK  TPN @ 3 * 0 ?DO I CELLS TPAT + @ FW, LOOP
   99 FMARK                           \ 'c', no constructors
  100 FMARK                           \ 'd', no destructors
   83 FMARK ;                         \ 'S', no symbols

\ Refuse to write an image whose compilation failed. A compiler that
\ emits something after declining is worse than one that emits nothing:
\ the file exists, looks plausible, and traps on its first instruction.
: TSAVE ( c-addr u -- )
   TOK @ 0= IF 2DROP ." c4r: not written -- a definition did not compile" CR EXIT THEN
   TBUILD
   FBUF FP @ 2SWAP SAVE-FILE
   0= IF ." c4r: could not write the image" CR THEN ;

\ The whole thing, from the command line:
\
\   ./c4th src/c4th/forth/core.f src/c4th/forth/asm.f \
\          src/c4th/forth/native.f src/c4th/forth/c4r.f \
\          src/c4th/forth/trt.f prog.f \
\          -e 'S" prog.c4r" \' MAIN TC4R'
\
\ IF/THEN are compile-only, so the report lives in a definition rather
\ than at the interpreter, where it would lay branches into HERE.
: TCHECK ( ok -- )
   0= IF
      ." c4r: did not compile -- "
      NBAD @ ?DUP IF DUP 2 CELLS + @ SWAP 3 CELLS + @ TYPE
               ELSE ." (underflow or a mismatched branch)" THEN
      CR
   THEN ;

: TC4R ( c-addr u xt -- )   \ compile it as the entry, then write
   TMAIN TCHECK TSAVE ;

: .TIMAGE
   ." c4r: " TCP @ . ." code words, " TDP @ . ." data bytes, "
   TPN @ . ." patches, entry " TENTRY @ . CR ;
