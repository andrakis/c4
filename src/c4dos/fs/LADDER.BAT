@ECHO OFF
REM The bootstrap, inside the machine: the compiler on the disk builds
REM a compiler from source, that one builds the preprocessor AND builds
REM itself again (the two images must be the same size -- that is the
REM fixed point), and only then does anything touch the kernel. Nothing
REM here is a shortcut: every image after step 2 was made by an image
REM this machine compiled. See docs/compiler-on-the-board.md.
ECHO =============================================
ECHO  THE LADDER, INSIDE THE MACHINE
ECHO =============================================
ECHO [1] unpacking the compiler's own source...
RUN dostar.c4r x tools-src.tar
ECHO [2] the seed compiler builds a compiler...
RUN c4cc.c4r -o c4cc2.c4r c4dos.h load-c4r.c c4cc.c asm-c4r.c
ECHO [3] that compiler builds the preprocessor...
RUN c4cc2.c4r -o cpp2.c4r c4dos.h cpp.c
ECHO [4] and builds itself again -- the fixed point...
RUN c4cc2.c4r -o c4cc3.c4r c4dos.h load-c4r.c c4cc.c asm-c4r.c
ECHO [5] unpacking the kernel's source...
RUN dostar.c4r x c4ke-src.tar
ECHO [6] the new preprocessor reads the kernel...
RUN cpp2.c4r -Iinclude -I. -DC4CC=1 -D__c4__=1 -D__C4CC__=1 -D__c4cc__=1 -o c4ke.i c4ke.c
ECHO [7] the new compiler builds the kernel...
RUN c4cc2.c4r -o c4ke.c4r c4ke.i
ECHO [8] and the init process...
RUN c4cc2.c4r -o init.c4r include/u0.h ps.c eshell.c init.c
ECHO [9] the SHIPPED compiler builds the kernel too, to compare...
RUN c4cc.c4r -o c4ke0.c4r c4ke.i
ECHO =============================================
ECHO  Type RUN dosload.c4r c4ke.c4r to boot it.
ECHO =============================================
