\ c4fc dsl.f -- the vocabulary a C compiler is written in.
\
\ Three ideas, and every C feature added later is one row in each.
\
\ NODES. A node is a record whose first cell is a TAG. NODE: allocates
\ the tag and opens a field list; field names are shared across kinds on
\ purpose -- two kinds that both have >lhs at the same offset SHOULD use
\ the same accessor, and Forth's redefinition makes that free rather than
\ a collision.
\
\ GENERICS. A generic is a table of execution tokens indexed by tag, so
\ dispatch is one indexed fetch, not a chain of comparisons. self.f's
\ DODIR is a twenty-arm  DUP n = IF ... EXIT THEN  chain: linear in the
\ number of C constructs, and every new construct edits it. A generic is
\ O(1) and nothing existing is touched.
\
\ TABLES. Keywords, operator precedence, type rules -- each is a list of
\ rows, one per feature, declared next to nothing else.

\ -- arena --------------------------------------------------------------
\ A compiler is a batch process: bump-allocate and let go of everything
\ at the end. No free, no GC, no ownership.

VARIABLE ARENA   VARIABLE ARENA-TOP   VARIABLE ARENA-END
: ARENA-INIT ( n -- )  DUP ALLOCATE DUP ARENA ! DUP ARENA-TOP !  SWAP + ARENA-END ! ;
: ALLOT: ( n -- a )                       \ cell-aligned: cells are what go in it
   1 CELLS 1- + 1 CELLS 1- INVERT AND
   ARENA-TOP @ SWAP OVER + DUP ARENA-END @ > IF ." c4fc: arena full" CR ABORT THEN
   ARENA-TOP ! ;

\ -- growable vectors ---------------------------------------------------
\ Token lists, instruction lists, symbol tables. Arrays, not cons cells.

BEGIN-STRUCTURE VEC
   FIELD: v.data
   FIELD: v.len
   FIELD: v.cap
END-STRUCTURE

: VEC-INIT ( v n -- )  {: v n -- :}  n CELLS ALLOCATE v v.data !  0 v v.len !  n v v.cap ! ;
: V, ( x v -- )
   {: x v -- :}
   v v.len @ v v.cap @ >= IF ." c4fc: vector full" CR ABORT THEN
   x  v v.data @ v v.len @ CELLS + !
   1 v v.len +! ;
: V@ ( i v -- x )  v.data @ SWAP CELLS + @ ;
: V! ( x i v -- )  v.data @ SWAP CELLS + ! ;
: V# ( v -- n )    v.len @ ;

\ -- diagnostics --------------------------------------------------------
\ A number with no trailing space, because c4th's . prints one and a
\ message reads better without it.

CREATE .NBUF 24 ALLOT   VARIABLE .NBP
: .N ( n -- )
   DUP 0< IF 45 EMIT NEGATE THEN
   .NBUF 24 + .NBP !
   BEGIN -1 .NBP +!  DUP 10 MOD 48 + .NBP @ C!  10 /  DUP 0= UNTIL DROP
   .NBP @  .NBUF 24 + OVER -  TYPE ;

\ -- nodes --------------------------------------------------------------

64 CONSTANT TAG-MAX
VARIABLE #TAGS       0 #TAGS !
VARIABLE NODE-SIZE                       \ field cursor for the kind in hand

: NODE: ( "name" -- )                    \ allocate a tag, open a field list
   #TAGS @ TAG-MAX < 0= IF ." c4fc: too many node kinds" CR ABORT THEN
   #TAGS @ CONSTANT
   1 #TAGS +!
   1 CELLS NODE-SIZE ! ;                 \ cell 0 is the tag
: NFIELD: ( "name" -- )                  \ a slot in the node in hand
   CREATE NODE-SIZE @ ,  1 CELLS NODE-SIZE +!
   DOES> @ + ;
: ;NODE ( -- ) ;

: >TAG ( node -- tag )  @ ;
: NEW ( tag size -- node )  ALLOT: TUCK ! ;

\ -- generics -----------------------------------------------------------

: NO-METHOD ( node -- )
   ." c4fc: no method for tag " >TAG . CR ABORT ;

: GENERIC: ( "name" -- )
   CREATE  TAG-MAX 0 ?DO ['] NO-METHOD , LOOP
   DOES> ( node body -- )  OVER >TAG CELLS + @ EXECUTE ;

VARIABLE M-TABLE   VARIABLE M-TAG
CREATE M-NAME 96 ALLOT   VARIABLE M-LEN
: M-CAT ( a u -- )                       \ append to the method's name
   DUP >R  M-NAME M-LEN @ +  SWAP MOVE  R> M-LEN +! ;
: :M ( "generic" "tag" -- )              \ open a method on a generic
   S" : " M-LEN ! DROP  2 M-LEN !  S" : " DROP M-NAME 2 MOVE
   BL WORD DUP COUNT M-CAT
   FIND 0= IF ." c4fc: :M unknown generic" CR ABORT THEN
   >BODY M-TABLE !
   S" /" M-CAT
   BL WORD DUP COUNT M-CAT
   FIND 0= IF ." c4fc: :M unknown tag" CR ABORT THEN
   EXECUTE M-TAG !
   S"  " M-CAT
   M-NAME M-LEN @ EVALUATE ;             \ ": EVAL/n_add " -- a real name
: ;M ( -- )
   POSTPONE ;
   LATEST M-TAG @ CELLS M-TABLE @ + ! ; IMMEDIATE
