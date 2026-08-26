\ c4fc.f -- the driver.

\ c4cc's library table plus c4's intrinsics, in c4lc's order. Every one
\ compiles to its opcode where the call would be, so `read(fd, b, n)` is
\ three pushes and one instruction.
: BUILTIN ( a u op -- )  c_builtin SWAP t_int 0 0 ST, ;
: BUILTINS
   S" open"   oOPEN BUILTIN   S" read"    oREAD BUILTIN
   S" close"  oCLOS BUILTIN   S" printf"  oPRTF BUILTIN
   S" malloc" oMALC BUILTIN   S" free"    oFREE BUILTIN
   S" memset" oMSET BUILTIN   S" memcmp"  oMCMP BUILTIN
   S" exit"   oEXIT BUILTIN   S" putchar" oPUTC BUILTIN
   S" puts"   oPUTS BUILTIN   S" realloc" oRALC BUILTIN
   S" memcpy" oMCPY BUILTIN   S" stacktrace" oSTRC BUILTIN
   S" install_trap_handler" oITH BUILTIN
   S" __opcode" o_OPC BUILTIN  S" __builtin" o_BLT BUILTIN
   S" __c4_trap" o_TRP BUILTIN S" __c4_opcode" oOPCD BUILTIN
   S" __c4_jmp" o_JMP BUILTIN  S" __c4_adjust" o_ADJ BUILTIN
   S" __c4_configure" oC4CF BUILTIN  S" __c4_cycles" oC4CY BUILTIN
   S" __time" oTIME BUILTIN    S" __c4_signal" oSIGH BUILTIN
   S" __c4_sigint" oSIGI BUILTIN     S" __c4_usleep" oUSLP BUILTIN
   S" __c4_info" oINFO BUILTIN S" __c4_ops_list" oOPSL BUILTIN
   S" __c4_invoke" oC4IV BUILTIN     S" __c4_float" oFLT BUILTIN
   S" __c4_cpu_id" oCPUI BUILTIN     S" __c4_cpu_count" oCPUN BUILTIN
   S" __c4_cpu_start" oCPUS BUILTIN  S" __c4_cpu_halt" oCPUH BUILTIN
   S" __c4_cas" oCAS BUILTIN   S" __c4_xchg" oXCHG BUILTIN
   S" __c4_fadd" oFADD BUILTIN S" __c4_wait" oCWAI BUILTIN
   S" __c4_wake" oCWAK BUILTIN S" __c4_ipi" oIPI BUILTIN
   S" __c4_termraw" oTRAW BUILTIN ;

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
\ -c: compile one unit to an OBJECT. What it cannot resolve it names,
\ and c4rlink resolves it later against the units that can.
: -c ( -- )  1 OBJECT ! ;
\ -mcisc needs c4mp; -mfuse takes plain c4 away as well, and implies -O
\ because the pass it turns on lives in the optimizer.
: -mcisc ( -- )  1 CISC ! ;
: -mfuse ( -- )  1 FUSE !  1 OPTIMIZE ! ;

\ Everything a compile of one unit starts from. -O runs it twice: the
\ first pass exists only to learn which functions are reachable, and
\ throws its buffers away.
: UNIT-RESET ( -- )
   EMIT-RESET
   NSYM SYMR * ALLOCATE STAB !  0 STN !  0 NGLO !
   0 #STRUCTS !  0 #MEMS !  0 VA-MAKE !
   0 TP !
   BUILTINS ;

: C4FC ( a u -- )                       \ compile that file, image to stdout
   C4FC-READY @ 0= IF C4FC-INIT THEN
   PREPROCESS @ IF PP-FILE ELSE LEX-FILE THEN
   \ The discovery pass, which runs whatever the flags say. -O wants the
   \ call graph and -c wants to know which names this unit defines, but
   \ the plain compile wants something from it too: a call to a function
   \ defined further down with no prototype anywhere resolves only if
   \ something has read ahead. Making that depend on -O would mean
   \ `c4fc f.c` rejecting a file `c4fc -O f.c` compiles.
   T2-RESET  DECL-RESET  0 EXTN !  1 COLLECT !
   UNIT-RESET PROGRAM
   T2-CLOSE  MAKE-EXTERNS  0 COLLECT !
   UNIT-RESET PREREGISTER PROGRAM
   OBJECT @ 0= ENTRY @ 0< AND IF ." c4fc: no main" CR ABORT THEN
   OPTIMIZE @ IF OPT-RUN THEN
   OBJECT @ IF SN @ FIX-EXTERNS  EXT-SYMS THEN
   WRITE-IMAGE ;
