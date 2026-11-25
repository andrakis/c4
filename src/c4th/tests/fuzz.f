\ c4th: a differential fuzzer for the native backend.
\
\ docs/c4th-design.md names the stack model as the place silent wrong
\ answers live, and b5.f is sixty-three cases somebody thought of. This
\ generates thousands nobody thought of: random balanced definitions,
\ each run on the threaded engine -- which is the one that passes the
\ Forth-2012 CORE suite, so it is the oracle -- and then compiled and
\ CALLED, with the answers required to agree.
\
\ It prints only counts, so the transcript is a golden; a mismatch prints
\ the offending definition's opcode sequence, which is what you need to
\ reproduce it. The seed is fixed, so a failure is the same failure
\ tomorrow and on the other host.

\ -- a container word whose body we overwrite ---------------------------
\ CREATE makes a word whose code field is do_var; pointing that at a
\ colon definition's code field instead makes it an ordinary Forth word
\ whose body happens to be ours to write. >BODY then gives the cell to
\ start commaing into, and calling it runs whatever is there.

: >WCODE! ( a xt -- )  5 CELLS + ! ;
CREATE GW  1024 CELLS ALLOT
' NPROBE >WCODE  ' GW >WCODE!
' GW >BODY CONSTANT GBODY

\ -- a small generator -------------------------------------------------

VARIABLE SEED   VARIABLE GP   VARIABLE GD   VARIABLE GMAX
: RND  ( -- u )  SEED @ 1103515245 * 12345 + DUP SEED ! 1 RSHIFT ;
: RND% ( n -- k ) RND SWAP MOD ;
: G,   ( x -- )  GP @ !  1 CELLS GP +! ;

\ Every entry is ( xt in out ): what it consumes and what it leaves. The
\ generator only picks one whose input is available, so a generated
\ definition never underflows -- which matters, because a word that
\ underflows tells you nothing about whether the backend is right.
CREATE GOPS
   ' + ,      2 ,  1 ,        ' - ,      2 ,  1 ,
   ' * ,      2 ,  1 ,        ' AND ,    2 ,  1 ,
   ' OR ,     2 ,  1 ,        ' XOR ,    2 ,  1 ,
   ' = ,      2 ,  1 ,        ' < ,      2 ,  1 ,
   ' > ,      2 ,  1 ,        ' MAX ,    2 ,  1 ,
   ' MIN ,    2 ,  1 ,        ' U< ,     2 ,  1 ,
   ' DUP ,    1 ,  2 ,        ' DROP ,   1 ,  0 ,
   ' SWAP ,   2 ,  2 ,        ' OVER ,   2 ,  3 ,
   ' NIP ,    2 ,  1 ,        ' TUCK ,   2 ,  3 ,
   ' ROT ,    3 ,  3 ,        ' -ROT ,   3 ,  3 ,
   ' 2SWAP ,  4 ,  4 ,        ' 2DUP ,   2 ,  4 ,
   ' 2DROP ,  2 ,  0 ,        ' 2OVER ,  4 ,  6 ,
   ' 1+ ,     1 ,  1 ,        ' 1- ,     1 ,  1 ,
   ' NEGATE , 1 ,  1 ,        ' INVERT , 1 ,  1 ,
   ' 0= ,     1 ,  1 ,        ' 0< ,     1 ,  1 ,
   ' 2* ,     1 ,  1 ,        ' ABS ,    1 ,  1 ,
33 CONSTANT #GOPS
: GOP@  ( i -- xt in out )  3 * CELLS GOPS +  DUP @ SWAP
                            1 CELLS + DUP @ SWAP 1 CELLS + @ ;

\ Memory, on a single scratch cell so the address is always valid. Worth
\ having because ! is where the permutation machinery is used in anger --
\ C4's store wants its address pushed before the value is computed, and
\ Forth writes the value first, so every ! is a rotation.
CREATE SCR 1 CELLS ALLOT
: GFETCH  ['] LIT G, SCR G, ['] @ G,   1 GD +! ;
: GSTORE  ['] LIT G, SCR G, ['] ! G,  -1 GD +! ;
: GPLUSST ['] LIT G, SCR G, ['] +! G, -1 GD +! ;

\ A literal, or an operation the current depth can afford.
: GSTEP ( -- )
   8 RND% 0= IF
      GD @ 0=  GD @ GMAX @ < AND IF GFETCH EXIT THEN
      GD @ 0> IF 2 RND% 0= IF GSTORE ELSE GPLUSST THEN EXIT THEN
      GFETCH EXIT
   THEN
   GD @ 0=  4 RND% 0= OR  GD @ GMAX @ >= 0= AND IF
      ['] LIT G,  2000 RND% 1000 - G,  1 GD +!  EXIT THEN
   #GOPS 0 DO
      #GOPS RND% GOP@                 ( xt in out )
      OVER GD @ <= IF
         >R  ( in )  GD @ SWAP - R> +  GD !   G,  UNLOOP EXIT
      THEN
      2DROP DROP
   LOOP ;

\ -- structure ---------------------------------------------------------
\ Straight-line code exercises the deferred model and the frame; the
\ branch-target depth check, the pin, and the loop's frame cells only
\ come out under control flow. So a definition is a sequence of chunks,
\ and a chunk is plain steps, an IF, an IF/ELSE, or a counted loop.
\
\ Every block is generated depth-NEUTRAL, by padding or dropping back to
\ the depth it started at. That is not a convenience: an IF whose arms
\ leave different depths is a definition the backend is right to refuse,
\ so generating them would test the refusal rather than the code.

: GPAD ( d -- )
   BEGIN GD @ OVER > WHILE ['] DROP G, -1 GD +! REPEAT
   BEGIN GD @ OVER < WHILE ['] LIT G, 7 G, 1 GD +! REPEAT
   DROP ;
: GNEEDFLAG ( -- )  GD @ 0= IF ['] LIT G, 1 G, 1 GD +! THEN ;
: GSTEPS ( n -- )  0 ?DO GSTEP LOOP ;

: GIF ( -- )
   GNEEDFLAG
   ['] 0BRANCH G,  GP @  0 G,  -1 GD +!         ( patch )
   GD @ >R  4 RND% 1+ GSTEPS  R> GPAD
   GP @ SWAP ! ;

: GIFELSE ( -- )
   GNEEDFLAG
   ['] 0BRANCH G,  GP @  0 G,  -1 GD +!         ( p1 )
   GD @ >R
   4 RND% 1+ GSTEPS  R@ GPAD
   ['] BRANCH G,  GP @  0 G,                    ( p1 p2 )
   SWAP GP @ SWAP !                             ( p2 )   \ p1 -> here
   4 RND% 1+ GSTEPS  R> GPAD
   GP @ SWAP ! ;

\ The loop's index and limit are literals, so the trip count is small and
\ known; I is allowed inside, since that is the point of having a loop.
: GLOOP ( -- )
   ['] LIT G,  3 RND% 1+ G,  ['] LIT G, 0 G,  ['] (DO) G,
   GP @ >R                                      ( start )
   GD @ >R
   3 RND% 1+ 0 ?DO
      3 RND% 0= IF ['] I G, 1 GD +! ELSE GSTEP THEN
   LOOP
   R> GPAD
   ['] (LOOP) G,  R> G, ;

: GCHUNK ( -- )
   10 RND%
   DUP 6 < IF DROP 3 RND% 1+ GSTEPS EXIT THEN
   DUP 8 < IF DROP GIF EXIT THEN
   9 < IF GIFELSE ELSE GLOOP THEN ;

\ Generate one definition of nargs arguments leaving exactly one value.
: GEN ( nargs len -- end )
   SWAP GD !  8 GMAX !  GBODY GP !
   0 DO GCHUNK LOOP
   BEGIN GD @ 1 > WHILE ['] DROP G, -1 GD +! REPEAT
   GD @ 0= IF ['] LIT G, 1 G, 1 GD ! THEN
   ['] EXIT G,
   GP @ ;

\ -- the differential --------------------------------------------------

VARIABLE XA  VARIABLE XB  VARIABLE XC
VARIABLE NRUN  VARIABLE NDECL  VARIABLE NMIS
: .BODY ( end -- )
   GBODY BEGIN 2DUP > WHILE
      DUP @ DUP 2 CELLS + @ SWAP 3 CELLS + @ TYPE SPACE
      DUP @ DUP ['] LIT = OVER ['] BRANCH = OR OVER ['] 0BRANCH = OR
                SWAP ['] (LOOP) = OR
      IF 1 CELLS + DUP @ . THEN
      1 CELLS +
   REPEAT 2DROP ;

\ Three arities, each with its own call shape. Written out rather than
\ tabled because INVOKE1/2/3 are distinct primitives -- C4 has no way to
\ build a call of a computed arity.
: TRY-N ( nargs -- )
   >R  R@ 8 RND% 3 + GEN            ( end )
   1 NRUN +!
   1000 RND% 500 - XA !  1000 RND% 500 - XB !  1000 RND% 500 - XC !
   0 SCR !
   R@ 0 = IF GW ELSE
   R@ 1 = IF XA @ GW ELSE
   R@ 2 = IF XA @ XB @ GW ELSE
            XA @ XB @ XC @ GW THEN THEN THEN     ( end threaded )
   SWAP GBODY SWAP R@ NCOMPILE-N                 ( threaded ok? )
   IF
      0 SCR !
      R@ 0 = IF ASMBUF INVOKE ELSE
      R@ 1 = IF XA @ ASMBUF INVOKE1 ELSE
      R@ 2 = IF XA @ XB @ ASMBUF INVOKE2 ELSE
               XA @ XB @ XC @ ASMBUF INVOKE3 THEN THEN THEN
      <> IF 1 NMIS +!
           ." MISMATCH nargs " R@ . ."  args " XA @ . XB @ . XC @ . CR
           ."   body: " GP @ .BODY CR THEN
   ELSE DROP 1 NDECL +! THEN
   R> DROP ;

: FUZZ ( n -- )
   0 NRUN !  0 NDECL !  0 NMIS !
   0 DO  4 RND% TRY-N  LOOP
   ." fuzz: " NRUN @ . ." definitions, " NDECL @ . ." declined, "
   NMIS @ . ." mismatches" CR ;

\ Deterministic, so a failure is the same failure tomorrow and on the
\ other host. For a deeper run, append
\   -e '999 SEED ! 25000 FUZZ'
12345 SEED !
2000 FUZZ
