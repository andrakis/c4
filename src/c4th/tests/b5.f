\ c4th B5: the native backend, checked against the threaded interpreter.
\
\ The threaded engine is the oracle -- it is the one that passes the
\ Forth-2012 CORE suite. Every word here is run both ways and the answers
\ must agree; a word the backend declines to compile says so, which is the
\ behaviour that matters most. A native backend that quietly emits worse
\ or wrong code is worse than no backend.
\
\ Words that take arguments are compiled with NCOMPILE-N and called with
\ INVOKE1/2/3, which pass them the way C4 passes any function's arguments
\ -- so the check is that the compiled word IS callable, not merely that
\ the compiler did not complain.

: .WNAME ( xt -- )  DUP 2 CELLS + @ SWAP 3 CELLS + @ TYPE ;
: .WHY   ( -- )     ." declined " NBAD @ ?DUP IF .WNAME THEN ;

VARIABLE XA  VARIABLE XB  VARIABLE XC  VARIABLE XR

: CHECK ( body end threaded -- )
   >R NCOMPILE
   IF   ASMBUF INVOKE R@ = IF ." ok" ELSE ." MISMATCH" THEN
   ELSE .WHY THEN
   R> DROP CR ;

: CK1 ( xt -- ) >BODY DUP BODY-END 1 NCOMPILE-N
   IF XA @ ASMBUF INVOKE1 XR @ = IF ." ok" ELSE ." MISMATCH" THEN ELSE .WHY THEN CR ;
: CK2 ( xt -- ) >BODY DUP BODY-END 2 NCOMPILE-N
   IF XA @ XB @ ASMBUF INVOKE2 XR @ = IF ." ok" ELSE ." MISMATCH" THEN ELSE .WHY THEN CR ;
: CK3 ( xt -- ) >BODY DUP BODY-END 3 NCOMPILE-N
   IF XA @ XB @ XC @ ASMBUF INVOKE3 XR @ = IF ." ok" ELSE ." MISMATCH" THEN ELSE .WHY THEN CR ;
\ For a word with no single result to compare: does it compile at all?
: CKC ( xt n -- ) >R >BODY DUP BODY-END R> NCOMPILE-N
   IF ." compiles" ELSE .WHY THEN CR ;

\ -- B5: the reorder-free subset -------------------------------------
: T1 2 3 + ;                 T1 ." T1 " LATEST >BODY HERE ROT CHECK
: T2 10 4 - ;                T2 ." T2 " LATEST >BODY HERE ROT CHECK
: T3 6 7 * ;                 T3 ." T3 " LATEST >BODY HERE ROT CHECK
: T4 5 DUP * ;               T4 ." T4 " LATEST >BODY HERE ROT CHECK
: T5 20 5 / 3 MOD ;          T5 ." T5 " LATEST >BODY HERE ROT CHECK
: T6 5 5 = ;                 T6 ." T6 " LATEST >BODY HERE ROT CHECK
: T7 5 6 = ;                 T7 ." T7 " LATEST >BODY HERE ROT CHECK
: T8 9 4 < ;                 T8 ." T8 " LATEST >BODY HERE ROT CHECK
: TA 12 10 AND ;             TA ." TA " LATEST >BODY HERE ROT CHECK
: TC 7 1 IF DROP 222 THEN ;  TC ." TC " LATEST >BODY HERE ROT CHECK
: TD 0 BEGIN 1+ DUP 5 < WHILE REPEAT ;  TD ." TD " LATEST >BODY HERE ROT CHECK

\ An IF whose arms leave different numbers of values has no single state
\ at the join, so every item's frame offset past that point would depend
\ on which way it was reached. That is the shape silent wrong answers
\ come in, and it must decline.
: TB 7 0 IF 111 THEN ;       TB ." TB (unbalanced, expect declined) "
                             LATEST >BODY HERE ROT CHECK

\ -- B5b: the deferred model -----------------------------------------
\ SWAP and OVER are compile-time reorderings of already-emitted code, !
\ falls out of SWAP, and a VARIABLE reference is just a literal address.
VARIABLE V
: TE 1 2 SWAP - ;            TE ." TE " LATEST >BODY HERE ROT CHECK
: TF 3 4 OVER + + ;          TF ." TF " LATEST >BODY HERE ROT CHECK
: TG 42 V ! V @ ;            TG ." TG " LATEST >BODY HERE ROT CHECK
: TH 7 V ! 3 V @ + ;         TH ." TH " LATEST >BODY HERE ROT CHECK
: TI 1 2 3 SWAP DROP - ;     TI ." TI " LATEST >BODY HERE ROT CHECK
: TJ 9 V ! V @ V @ * ;       TJ ." TJ " LATEST >BODY HERE ROT CHECK
: TK 5 0= ;                  TK ." TK " LATEST >BODY HERE ROT CHECK
: TL 0 0= ;                  TL ." TL " LATEST >BODY HERE ROT CHECK
: TM 6 NEGATE ;              TM ." TM " LATEST >BODY HERE ROT CHECK
: TN 5 INVERT ;              TN ." TN " LATEST >BODY HERE ROT CHECK
: TO 3 CELLS ;               TO ." TO " LATEST >BODY HERE ROT CHECK
: TP 4 2 NIP ;               TP ." TP " LATEST >BODY HERE ROT CHECK

\ -- B5c: counted loops ----------------------------------------------
: A1 0 10 0 DO I + LOOP ;              A1 ." A1 " LATEST >BODY HERE ROT CHECK
: A2 0 5 0 DO 3 0 DO 1+ LOOP LOOP ;    A2 ." A2 " LATEST >BODY HERE ROT CHECK
: A3 0 10 0 DO I 5 = IF LEAVE THEN 1+ LOOP ;
                                       A3 ." A3 " LATEST >BODY HERE ROT CHECK
: A4 0 10 0 DO 1+ 2 +LOOP ;            A4 ." A4 " LATEST >BODY HERE ROT CHECK
: A5 0 -10 0 DO 1+ -2 +LOOP ;          A5 ." A5 " LATEST >BODY HERE ROT CHECK
: A6 0 3 0 DO 4 0 DO I J + + LOOP LOOP ;
                                       A6 ." A6 " LATEST >BODY HERE ROT CHECK
: A7 10 3 0 ?DO 1+ LOOP ;              A7 ." A7 " LATEST >BODY HERE ROT CHECK
: A8 10 0 0 ?DO 1+ LOOP ;              A8 ." A8 (?DO with equal limits) "
                                       LATEST >BODY HERE ROT CHECK

\ -- B5c: the return stack, in the frame ------------------------------
: A9 7 >R 3 R> + ;                     A9 ." A9 " LATEST >BODY HERE ROT CHECK

\ -- B5c: stack permutation and copying -------------------------------
: B1 1 2 3 ROT ;                       B1 ." B1 " LATEST >BODY HERE ROT CHECK
: B2 1 2 3 -ROT + + ;                  B2 ." B2 " LATEST >BODY HERE ROT CHECK
: B3 1 2 3 4 2SWAP - SWAP - ;          B3 ." B3 " LATEST >BODY HERE ROT CHECK
: B4 3 4 2DUP + + + ;                  B4 ." B4 " LATEST >BODY HERE ROT CHECK
: B5 3 4 TUCK + + ;                    B5 ." B5 " LATEST >BODY HERE ROT CHECK
: B6 3 4 5 6 2OVER + + + + + ;         B6 ." B6 " LATEST >BODY HERE ROT CHECK
: B7 3 4 2DROP 9 ;                     B7 ." B7 " LATEST >BODY HERE ROT CHECK

\ -- B5c: the ones that need a branch of their own --------------------
: C1 3 4 MAX 9 MIN ;                   C1 ." C1 " LATEST >BODY HERE ROT CHECK
: C2 -7 ABS 3 - ;                      C2 ." C2 " LATEST >BODY HERE ROT CHECK
: C3 1 -1 U< ;                         C3 ." C3 " LATEST >BODY HERE ROT CHECK
: C4 -1 1 U> ;                         C4 ." C4 " LATEST >BODY HERE ROT CHECK
: C5 17 5 /MOD - ;                     C5 ." C5 " LATEST >BODY HERE ROT CHECK
: C6 5 V ! 3 V +! V @ ;                C6 ." C6 " LATEST >BODY HERE ROT CHECK
: C7 1 6 LSHIFT 2/ ;                   C7 ." C7 " LATEST >BODY HERE ROT CHECK

\ -- B5c: calls, by inlining ------------------------------------------
\ A CONSTANT is CREATE , DOES> @, so inlining one gives IMM addr; LI --
\ and a called colon word's items merge into the caller's model, so it
\ optimizes as if it had been written out.
7 CONSTANT SEVEN
: SQ DUP * ;
: D1 SEVEN SEVEN * ;                   D1 ." D1 " LATEST >BODY HERE ROT CHECK
: D2 5 SQ 3 SQ + ;                     D2 ." D2 " LATEST >BODY HERE ROT CHECK
: D3 0 4 0 DO SEVEN + SQ 1000 MOD LOOP ;
                                       D3 ." D3 " LATEST >BODY HERE ROT CHECK
\ Recursion has no bottom for an inliner, so it declines rather than
\ expanding for ever.
: D4 DUP 0> IF 1- RECURSE THEN ;       ." D4 (recursive, expect declined) "
                                       ' D4 CK1

\ -- B5c: definitions that take arguments -----------------------------
: E1 DUP * ;               7 XA ! XA @ E1 XR !            ." E1 " ' E1 CK1
: E2 - ;                   9 XA ! 4 XB ! XA @ XB @ E2 XR ! ." E2 " ' E2 CK2
: E3 SWAP - ;              9 XA ! 4 XB ! XA @ XB @ E3 XR ! ." E3 " ' E3 CK2
: E4 OVER + + ;            3 XA ! 5 XB ! XA @ XB @ E4 XR ! ." E4 " ' E4 CK2
: E5 ROT + + ;             1 XA ! 2 XB ! 3 XC ! XA @ XB @ XC @ E5 XR ! ." E5 " ' E5 CK3
: E6 DROP DROP 42 ;        1 XA ! 2 XB ! XA @ XB @ E6 XR ! ." E6 " ' E6 CK2
: E7 NIP 1+ ;              1 XA ! 2 XB ! XA @ XB @ E7 XR ! ." E7 " ' E7 CK2
: E8 0 SWAP 0 ?DO OVER + LOOP NIP ;
                           6 XA ! 4 XB ! XA @ XB @ E8 XR ! ." E8 " ' E8 CK2
: E9 >R DUP R> + ;         5 XA ! 6 XB ! XA @ XB @ E9 XR ! ." E9 " ' E9 CK2
: F1 OVER - >R - R> U< ;   5 XA ! 1 XB ! 9 XC ! XA @ XB @ XC @ F1 XR ! ." F1 " ' F1 CK3
: F2 0 MAX ;               -3 XA ! XA @ F2 XR !            ." F2 " ' F2 CK1
: F3 2DUP + - ;            9 XA ! 4 XB ! XA @ XB @ F3 XR ! ." F3 " ' F3 CK2
: F4 /MOD SWAP - ;         17 XA ! 5 XB ! XA @ XB @ F4 XR ! ." F4 " ' F4 CK2
: F5 SQ SWAP SQ - ;        3 XA ! 5 XB ! XA @ XB @ F5 XR ! ." F5 " ' F5 CK2
: F6 SWAP OVER ! CELL+ ! ; ." F6 (2!) "  ' F6 3 CKC
\ A word whose body reaches a primitive the backend cannot emit says so,
\ and names it.
: F7 @ . ;                 ." F7 (calls . , expect declined) " ' F7 CK1
