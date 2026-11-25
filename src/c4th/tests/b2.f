\ c4th B2: the outer interpreter and the primitive set.
\ Control structures are not here yet -- IF/THEN and friends arrive with
\ core.f at B3 -- so this exercises what B2 actually provides: parsing,
\ numbers, colon definitions, literals, and every primitive group.


\ -- arithmetic
7 3 + . 7 3 - . 7 3 * . 7 3 / . 7 3 MOD . CR
7 3 /MOD . . CR                   \ ( rem quot ) -> 1 2
-5 ABS . 5 NEGATE . 3 4 MIN . 3 4 MAX . CR
1 2* . 8 2/ . 5 1+ . 5 1- . CR
1 4 LSHIFT . 32 2 RSHIFT . CR

\ -- comparison: Forth-2012 flags are all-bits-set, so true prints as -1
1 1 = . 1 2 = . 1 2 < . 2 1 < . 1 2 <> . CR
0 0= . 1 0= . 0 0<> . -1 0< . 1 0> . CR
6 3 AND . 4 1 OR . 5 3 XOR . 0 INVERT . CR

\ -- stack
1 2 SWAP . . CR
1 2 OVER . . . CR
1 2 NIP . CR
1 2 3 ROT . . . CR
1 2 DUP . . . CR
1 2 2DUP . . . . CR
0 ?DUP . 9 ?DUP . . CR
1 2 3 DEPTH . 2DROP DROP CR

\ -- return stack, inside a definition (>R and R> are compile-only)
: RTEST  >R 100 R> + ;
5 RTEST . CR

\ -- memory, using dictionary space
HERE 42 , @ . CR
HERE 1 CELLS ALLOT  DUP 7 SWAP !  DUP @ .  DUP 9 SWAP !  @ . CR

\ -- definitions calling definitions
: SQ  DUP * ;
: QUAD  SQ SQ ;
3 SQ . 2 QUAD . CR

\ -- literals compile correctly inside a definition
: BIG  1000000 ;
BIG . CR

\ -- base
HEX 255 . FF . BASE @ . DECIMAL 255 . BASE @ . CR
