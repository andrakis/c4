\ c4fc.f -- the driver.

: BUILTINS
   S" open"   c_builtin oOPEN t_int 0 0 ST,   S" read"   c_builtin oREAD t_int 0 0 ST,
   S" close"  c_builtin oCLOS t_int 0 0 ST,   S" printf" c_builtin oPRTF t_int 0 0 ST,
   S" malloc" c_builtin oMALC t_int 0 0 ST,   S" free"   c_builtin oFREE t_int 0 0 ST,
   S" memset" c_builtin oMSET t_int 0 0 ST,   S" memcmp" c_builtin oMCMP t_int 0 0 ST,
   S" exit"   c_builtin oEXIT t_int 0 0 ST, ;

VARIABLE OPTIMIZE   0 OPTIMIZE !

\ The arena has to exist before -I and -D can be recorded, so the setup
\ is its own word and C4FC falls back to it -- which keeps the bare
\ `S" f.c" C4FC` that every spike test uses working unchanged.
\ -P is c4lc's flag and c4lc's default: without it the LEXER skips '#'
\ lines, which is what a source that has already been through gcc -E
\ needs. With it c4fc does the job itself. Keeping the default the same
\ as the oracle's is what lets every F2-F8 differential stay a straight
\ byte comparison.
VARIABLE PREPROCESS   0 PREPROCESS !
: -P ( -- )  1 PREPROCESS ! ;
VARIABLE C4FC-READY   0 C4FC-READY !
: C4FC-INIT ( -- )  67108864 ARENA-INIT  PP-RESET  1 C4FC-READY ! ;
: -I ( a u -- )  PP-PATH ;
: -D ( a u -- )  PP-DEFINE ;

: C4FC ( a u -- )                       \ compile that file, image to stdout
   C4FC-READY @ 0= IF C4FC-INIT THEN
   EMIT-INIT
   NSYM SYMR * ALLOCATE STAB !  0 STN !  0 NGLO !  0 GPN !
   BUILTINS
   PREPROCESS @ IF PP-FILE ELSE LEX-FILE THEN
   0 TP !
   PROGRAM
   ENTRY @ 0< IF ." c4fc: no main" CR ABORT THEN
   OPTIMIZE @ IF OPT-RUN THEN
   WRITE-IMAGE ;
