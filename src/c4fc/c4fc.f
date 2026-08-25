\ c4fc.f -- the driver.

: BUILTINS
   S" open"   c_builtin oOPEN t_int ST,   S" read"   c_builtin oREAD t_int ST,
   S" close"  c_builtin oCLOS t_int ST,   S" printf" c_builtin oPRTF t_int ST,
   S" malloc" c_builtin oMALC t_int ST,   S" free"   c_builtin oFREE t_int ST,
   S" memset" c_builtin oMSET t_int ST,   S" memcmp" c_builtin oMCMP t_int ST,
   S" exit"   c_builtin oEXIT t_int ST, ;

: C4FC ( a u -- )                       \ compile that file, image to stdout
   67108864 ARENA-INIT
   EMIT-INIT
   NSYM SYMR * ALLOCATE STAB !  0 STN !  0 NGLO !  0 GPN !
   BUILTINS
   LEX-FILE
   0 TP !
   PROGRAM
   ENTRY @ 0< IF ." c4fc: no main" CR ABORT THEN
   WRITE-IMAGE ;
