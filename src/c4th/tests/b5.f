\ c4th B5: the native backend, checked against the threaded interpreter.
\
\ The threaded engine is the oracle -- it is the one that passes the
\ Forth-2012 CORE suite. Every word here is run both ways and the answers
\ must agree; a word the backend declines to compile says so, which is the
\ behaviour that matters most. A native backend that quietly emits worse
\ or wrong code is worse than no backend.

: NAT ( body end -- ok? ) NCOMPILE ;

: CHECK ( body end threaded -- )
   >R NCOMPILE
   IF   ASMBUF INVOKE R@ = IF ." ok" ELSE ." MISMATCH" THEN
   ELSE ." declined" THEN
   R> DROP CR ;

: T1 2 3 + ;                 T1 ." T1 " LATEST >BODY HERE ROT CHECK
: T2 10 4 - ;                T2 ." T2 " LATEST >BODY HERE ROT CHECK
: T3 6 7 * ;                 T3 ." T3 " LATEST >BODY HERE ROT CHECK
: T4 5 DUP * ;               T4 ." T4 " LATEST >BODY HERE ROT CHECK
: T5 20 5 / 3 MOD ;          T5 ." T5 " LATEST >BODY HERE ROT CHECK
: T6 5 5 = ;                 T6 ." T6 " LATEST >BODY HERE ROT CHECK
: T7 5 6 = ;                 T7 ." T7 " LATEST >BODY HERE ROT CHECK
: T8 9 4 < ;                 T8 ." T8 " LATEST >BODY HERE ROT CHECK
: TA 12 10 AND ;             TA ." TA " LATEST >BODY HERE ROT CHECK
: TB 7 0 IF 111 THEN ;       TB ." TB " LATEST >BODY HERE ROT CHECK
: TC 7 1 IF DROP 222 THEN ;  TC ." TC " LATEST >BODY HERE ROT CHECK
: TD 0 BEGIN 1+ DUP 5 < WHILE REPEAT ;  TD ." TD " LATEST >BODY HERE ROT CHECK

\ Declined, and it must say so rather than emit something wrong.
: TE 1 2 SWAP - ;            TE ." TE " LATEST >BODY HERE ROT CHECK
: TF 3 4 OVER + + ;          TF ." TF " LATEST >BODY HERE ROT CHECK
