\ c4th B4: the peephole's rules, each shown firing and each shown to
\ leave behaviour alone. The count after every case is what P1 removed
\ cumulatively, so a rule that stops firing shows up as a smaller number
\ rather than as silence.

: SHOW ( -- )  ." [" PEEPCUT @ . ." ] " ;

: T1 1 2 + . ;              OPT T1 SHOW CR       \ LIT LIT +
: T2 9 3 - . ;              OPT T2 SHOW CR       \ LIT LIT -
: T3 6 7 * . ;              OPT T3 SHOW CR       \ LIT LIT *
: T4 5 DUP DROP . ;         OPT T4 SHOW CR       \ DUP DROP
: T5 1 2 SWAP SWAP . . ;    OPT T5 SHOW CR       \ SWAP SWAP
: T6 8 >R R> . ;            OPT T6 SHOW CR       \ >R R>

\ Compaction moves addresses, so these check that branches still land
\ where they should after instructions in front of them disappear.
: T7 1 2 + DROP 4 0 DO I . LOOP ;        OPT T7 SHOW CR
: T8 1 1 + DROP 0 IF ." y" ELSE ." n" THEN ;  OPT T8 SHOW CR
: T9 2 0 DO 3 4 + DROP I . LOOP ;        OPT T9 SHOW CR
: TA 6 0 DO I . I 2 > IF LEAVE THEN LOOP ;    OPT TA SHOW CR

\ Here the DUP is the loop's return target. Removing the pair IS safe --
\ the target simply remaps to whatever now follows -- and this checks
\ that it does: the loop still runs three times and the stack below is
\ untouched. The refusal case is the reverse, where the SECOND
\ instruction is branched to, which CANCELS? tests for directly.
: TB 3 0 DO DUP DROP LOOP ;   OPT 7 TB . SHOW CR
