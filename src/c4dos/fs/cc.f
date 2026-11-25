\ cc.f -- the C4DOS front end for c4fc.
\
\ C4DOS's command line cannot carry a quoted argument and c4th takes its
\ work from files, so the compile is written HERE rather than passed on
\ the command line. Edit the last two lines and RUN CC again.
\
\ The output goes through the C4DOS API (src/c4th/forth/dos.f), which is
\ the only way anything running on this VM can produce a file -- there is
\ no write syscall and no '>' redirection.

: CC ( src-a src-u out-a out-u -- )
   C4FC-INIT
   1 OPTIMIZE !
   S" ." -I  S" C4CC=1" -D  S" __c4__=1" -D
   S" __C4CC__=1" -D  S" __c4cc__=1" -D
   -P
   OUT>FILE
   ." compiling " 2DUP TYPE CR
   C4FC
   ." wrote the object" CR ;

\ Compile one unit to an object. c4rlink turns objects into a program:
\    RUN c4rlink.c4r a.c4o b.c4o -o prog.c4r
: OBJ ( src-a src-u out-a out-u -- )  -c CC ;

\ IF/THEN are compile-only, so the top level of a file cannot branch --
\ it has to be a definition that is then run. Getting that wrong ran
\ both arms.
: MAIN
   DOS? 0= IF
      ." cc.f: not under C4DOS -- there is nowhere to write the output" CR
      EXIT THEN
   ." c4fc under C4DOS, api at " C4DOS-API . CR
   S" hello.c" S" hello.c4r" CC ;
MAIN
