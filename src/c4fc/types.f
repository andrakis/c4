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
