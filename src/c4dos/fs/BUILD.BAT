@ECHO OFF
ECHO ---------------------------------------------
ECHO  Building C4KE from source, inside the machine
ECHO ---------------------------------------------
ECHO Unpacking sources onto the RAM disk...
RUN dostar.c4r x c4ke-src.tar
ECHO Preprocessing the kernel...
RUN cpp.c4r -Iinclude -I. -DC4CC=1 -D__c4__=1 -D__C4CC__=1 -D__c4cc__=1 -o c4ke.i c4ke.c
ECHO Compiling the kernel (this is the slow part)...
RUN c4cc.c4r -o c4ke.c4r c4ke.i
ECHO Compiling the init process...
RUN c4cc.c4r -o init.c4r include/u0.h ps.c eshell.c init.c
ECHO ---------------------------------------------
ECHO  Built. Type RUN c4ke.c4r to boot it,
ECHO  or RUN dosload.c4r c4ke.c4r to boot it with
ECHO  DOS's memory handed back and a command line.
ECHO ---------------------------------------------
