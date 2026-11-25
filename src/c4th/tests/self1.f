\ B5d.5: a program for checking the self-hosting compiler against c4th.
\
\ Written in self.f's dialect, which is a subset of c4th's -- so c4th can
\ run it directly and be the oracle, exactly as it was for B5d. The
\ arithmetic is chosen to have to be RIGHT rather than merely present:
\ signed division, a logical shift where C4's is arithmetic, a
\ multiply that overflows, nested counted loops, and a string.

VARIABLE ACC
VARIABLE SEED
CREATE NBUF 32 ALLOT
VARIABLE NBP

: CR1  10 EMIT ;
: SPC  32 EMIT ;
: U1 ( u -- )
   NBUF 24 + NBP !
   BEGIN -1 NBP +! DUP 10 MOD 48 + NBP @ C! 10 / DUP 0= UNTIL DROP
   NBP @ BEGIN DUP NBUF 24 + < WHILE DUP C@ EMIT 1+ REPEAT DROP ;
: D1 ( n -- )   DUP 0< IF 45 EMIT NEGATE THEN U1 SPC ;
: T1 ( a n -- ) 0 ?DO DUP C@ EMIT 1+ LOOP DROP ;

: SQUARES  10 1 DO I DUP * D1 LOOP CR1 ;
: ACCUM    0 ACC ! 20 0 DO ACC @ I + ACC ! LOOP ACC @ D1 CR1 ;
: SIGNS    5 -3 DO I D1 LOOP CR1  -7 ABS D1  7 NEGATE D1  0 17 - D1 CR1 ;
: NEXTR    SEED @ 1103515245 * 12345 + DUP SEED ! 1 RSHIFT 65535 AND ;
: PICKS    12345 SEED ! 0 8 0 DO NEXTR 10 MOD + LOOP D1
           17 5 /MOD D1 D1 CR1 ;
: BRANCHY  10 0 DO I 2 MOD 0= IF 48 I + EMIT ELSE 46 EMIT THEN LOOP CR1 ;
: NESTED   0 4 0 DO 3 0 DO I J * + LOOP LOOP D1 CR1 ;
: STRINGS  S" hello, self" T1 CR1 ;
: SHIFTS   -1 1 RSHIFT D1  -1 60 RSHIFT D1  1 10 LSHIFT D1  -1 0 RSHIFT D1 CR1 ;
: MEMS     NBUF 8 + 4242 SWAP !  NBUF 8 + @ D1
           NBUF 100 SWAP C!  NBUF C@ D1  CR1 ;
: STACKY   1 2 3 ROT D1 D1 D1  1 2 OVER D1 D1 D1
           7 8 SWAP D1 D1  5 DUP D1 D1  9 ?DUP D1 D1 CR1 ;
: CMPS     1 2 < D1  2 1 < D1  3 3 = D1  1 2 U< D1  -1 2 U< D1
           5 3 MIN D1  5 3 MAX D1 CR1 ;
: MAIN
   SQUARES ACCUM SIGNS PICKS BRANCHY NESTED STRINGS SHIFTS MEMS
   STACKY CMPS ;
