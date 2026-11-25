@ECHO OFF
REM BUILD.BAT, for a machine that has only the ORIGINAL c4 opcodes.
REM
REM BUILD.BAT runs c4cc.c4r directly, and c4cc needs six instructions
REM the stock set does not have -- MCPY, STRC, OPCD, INFO, C4IV and
REM JSRS, the last of which is a call through a function pointer held
REM in a local, which is how c4cc's emit-handler table works. On a
REM machine that has them (c4m, c4bb) that is free. On plain c4 it is
REM `unknown instruction = 62`.
REM
REM The fix is the same trick C4M-C4KE.BAT uses, applied one step
REM earlier: put c4m underneath the COMPILER as well. c4m.c4r is stock
REM c4, and it provides those six opcodes to anything it runs.
REM
REM The cost is real. Every instruction of the compile is now
REM interpreted by c4m, which is itself being interpreted -- so this is
REM slower than BUILD.BAT by the whole width of another VM. Use
REM BUILD.BAT wherever the machine can run it; this is for the rung
REM that cannot.
ECHO ---------------------------------------------
ECHO  Building C4KE with the compiler under c4m
ECHO  (for a stock-c4 machine -- this is SLOW)
ECHO ---------------------------------------------
ECHO Unpacking sources onto the RAM disk...
RUN dostar.c4r x c4ke-src.tar
ECHO Preprocessing the kernel...
RUN cpp.c4r -Iinclude -I. -DC4CC=1 -D__c4__=1 -D__C4CC__=1 -D__c4cc__=1 -o c4ke.i c4ke.c
ECHO Compiling the kernel under c4m (the very slow part)...
RUN c4m.c4r load-c4r.c -- c4cc.c4r -o c4ke.c4r c4ke.i
ECHO Compiling the init process under c4m...
RUN c4m.c4r load-c4r.c -- c4cc.c4r -o init.c4r include/u0.h ps.c eshell.c init.c
ECHO ---------------------------------------------
ECHO  Built. Type C4M-C4KE to boot it under c4m,
ECHO  which is also what a stock-c4 machine needs.
ECHO ---------------------------------------------
