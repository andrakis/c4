\ dos.f -- producing a file on C4DOS.
\
\ The C4 VM has no write syscall. Natively c4th has SAVE-FILE, which
\ opens and writes with the host's own calls; under c4m, c4mp or c4bb
\ there is no such thing, and the ONLY way a program running there can
\ put bytes on a disk is to ask C4DOS to do it.
\
\ C4DOS's loader patches the address of its API table into any image
\ carrying the symbol __c4dos_api (include/c4dos.h), and c4th carries
\ it, so C4DOS-API is that address or zero. The table's slots hold
\ ordinary function addresses, so INVOKE1/2/3 call them directly -- the
\ invoke stub c4dos.h describes is for C, which cannot call through a
\ variable; Forth can.
\
\ Slot numbers are include/c4dos.h's and are v1, so every DOS that has
\ an API table at all has these three.

2 CONSTANT DOS-CREATE   3 CONSTANT DOS-WRITE   4 CONSTANT DOS-CLOSE

: DOS? ( -- f )  C4DOS-API 0<> ;
: DOS-SLOT ( n -- addr )  CELLS C4DOS-API + @ ;

\ The name has to be NUL-terminated for DOS, which takes char *; Forth
\ strings are address and count, so this is where the two meet.
CREATE DOSNAME 128 ALLOT
: >DOSZ ( a u -- z )
   DUP 127 > IF ." dos.f: name too long" CR ABORT THEN
   DUP >R  DOSNAME SWAP MOVE  0 DOSNAME R@ + C!  R> DROP  DOSNAME ;

\ ( addr len a u -- ok )  Write a block to a named file on the RAM disk.
: DOS-SAVE {: a n na nu | h w -- ok :}
   DOS? 0= IF 0 EXIT THEN
   na nu >DOSZ  DOS-CREATE DOS-SLOT INVOKE1 TO h
   h 0< IF 0 EXIT THEN
   \ write(handle, buf, len) -- c4dos.h's order, and INVOKE3 passes the
   \ three in the order they are on the stack.
   h a n  DOS-WRITE DOS-SLOT INVOKE3 TO w
   h DOS-CLOSE DOS-SLOT INVOKE1 DROP
   w n = ;

\ One name for "put this block in this file", whichever machine we are
\ on: DOS if there is a DOS, the host's own open/write if we are native,
\ and an honest failure on a bare VM where neither exists.
: SAVE-BLOCK ( addr len a u -- ok )
   DOS? IF DOS-SAVE EXIT THEN
   SAVE-FILE ;
