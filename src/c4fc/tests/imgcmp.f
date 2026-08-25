\ imgcmp.f -- compare two .c4r images as the LOADER sees them.
\
\ c4opt leaves the pre-optimisation address in the code word of a patched
\ operand and puts the correct one in the patch. The loader overwrites
\ the word from the patch, so the image is correct and the stale word is
\ never executed -- but it means two images can be identical in every way
\ that runs and still differ byte for byte.
\
\ So: apply every code-resident patch into its word, in BOTH images, and
\ compare what is left. That touches only words the loader overwrites
\ anyway, and a difference in anything else -- the patches themselves,
\ the data, the symbols, an instruction -- still shows up.

VARIABLE B1  VARIABLE L1  VARIABLE B2  VARIABLE L2
CREATE ZPATHB 1024 ALLOT

: ZP ( a u -- z )  {: a u -- z :}
   a ZPATHB u MOVE  0 ZPATHB u + C!  ZPATHB ;
: RDFILE ( a u -- buf len ) {: a u | fd b n k -- b n :}
   a u ZP OPENF TO fd
   fd 0< IF ." imgcmp: cannot open " a u TYPE CR 1 HALT THEN
   4194304 ALLOCATE TO b   0 TO n
   BEGIN fd b n + 65536 READF TO k  k 0> WHILE  n k + TO n  REPEAT
   fd CLOSEF
   b n ;

\ header: 13 bytes, then entry, codelen, datalen, patchlen, ...
: HW ( buf i -- v )  CELLS SWAP 13 + + @ ;
: CODE-AT ( buf -- a )  13 + 7 CELLS + 1 CELLS + ;
: PATCH-AT2 ( buf -- a ) {: b -- a :}
   b CODE-AT  b 1 HW CELLS +          \ past the code
   1 CELLS +  b 2 HW +                \ past the D marker and the data
   1 CELLS + ;                        \ past the P marker

: NORMALISE ( buf -- ) {: b | p n t -- :}
   b 3 HW TO n
   b PATCH-AT2 TO p
   n 0 ?DO
      p @ TO t
      t -1 = t -2 = OR IF
         p 2 CELLS + @   b CODE-AT  p CELL+ @ CELLS +  !
      THEN
      p 3 CELLS + TO p
   LOOP ;

: IMGCMP ( a1 u1 a2 u2 -- ) {: a1 u1 a2 u2 | i -- :}
   a1 u1 RDFILE L1 ! B1 !
   a2 u2 RDFILE L2 ! B2 !
   B1 @ NORMALISE   B2 @ NORMALISE
   L1 @ L2 @ <> IF
      ." imgcmp: different lengths, " L1 @ .N ."  and " L2 @ .N CR 1 HALT
   THEN
   L1 @ 0 ?DO
      B1 @ I + C@  B2 @ I + C@ <> IF
         ." imgcmp: differ at byte " I .N CR 1 HALT
      THEN
   LOOP
   ." imgcmp: identical once loaded" CR ;
