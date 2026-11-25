\ c4th ext.f -- the Forth-2012 word sets c4th's kernel does not carry.
\
\ Nothing here is invented. CASE/OF/ENDOF/ENDCASE and DEFER/IS are CORE
\ EXT; BEGIN-STRUCTURE and friends are the Structures word set; VALUE and
\ TO are CORE EXT. They are written in Forth because they can be, which
\ is the same reason core.f exists -- and because a DSL built on standard
\ syntax has a standard test suite waiting for it, which one built on
\ invented syntax never does.

\ -- selection ----------------------------------------------------------
\ The classic implementation: CASE leaves a count on the compile-time
\ stack, each OF adds one, and ENDCASE resolves that many THENs.

: CASE     0 ; IMMEDIATE
: OF       1+ >R POSTPONE OVER POSTPONE = POSTPONE IF POSTPONE DROP R> ; IMMEDIATE
: ENDOF    >R POSTPONE ELSE R> ; IMMEDIATE
: ENDCASE  POSTPONE DROP 0 ?DO POSTPONE THEN LOOP ; IMMEDIATE

\ -- deferred words -----------------------------------------------------
\ Mutual recursion needs these. A recursive-descent parser is one big
\ mutually recursive family and Forth insists on definition before use,
\ so the family is DEFERred first and filled in as each member is
\ written. That is not a workaround -- it is a table of what the parser
\ consists of, written down before the code.

: DEFER      CREATE ['] ABORT , DOES> @ EXECUTE ;
: DEFER@     >BODY @ ;
: DEFER!     >BODY ! ;
: IS         STATE @ IF POSTPONE ['] POSTPONE DEFER! ELSE ' DEFER! THEN ; IMMEDIATE
: ACTION-OF  STATE @ IF POSTPONE ['] POSTPONE DEFER@ ELSE ' DEFER@ THEN ; IMMEDIATE

\ -- values -------------------------------------------------------------

: VALUE  CREATE , DOES> @ ;
: (TO)   ' >BODY STATE @ IF POSTPONE LITERAL POSTPONE ! ELSE ! THEN ;
: TO     (TO) ; IMMEDIATE

\ -- structures ---------------------------------------------------------
\ Records, so that an AST node is  node .op @  and not  2 CELLS + @ .
\ self.f has twenty sites doing the second thing; that is the argument
\ for this file in one line.

: BEGIN-STRUCTURE  CREATE HERE 0 0 , DOES> @ ;
: END-STRUCTURE    SWAP ! ;
: +FIELD   ( n1 n2 "name" -- n3 )  CREATE OVER , + DOES> @ + ;
: FIELD:   ( n1 "name" -- n2 )     ALIGNED 1 CELLS +FIELD ;
: CFIELD:  ( n1 "name" -- n2 )     1 CHARS +FIELD ;
