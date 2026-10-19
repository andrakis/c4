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
   1 #MEMS +! ;
\ the qualifier mask of the member MEM, just recorded
: MEM-Q! ( q -- )  #MEMS @ 1- CELLS MB-QUAL + ! ;
: MEM-FIND ( struct a u -- i|-1 ) {: k a u -- i :}
   #MEMS @ 0 ?DO
      I CELLS MB-OWNER + @ k = IF
         I CELLS MB-NLEN + @ u = IF
            I CELLS MB-NAME + @ a u BYTES= IF I UNLOOP EXIT THEN
         THEN
      THEN
   LOOP -1 ;
