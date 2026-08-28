@ECHO OFF
REM Build C4IX, inside the machine, with the compiled c4lc.
REM
REM Twelve modules and a link. Each module is one c4sc run: preprocess,
REM parse, tree passes, code generation, peephole passes, and a .c4o
REM object written to the RAM disk. Then c4rlink joins them into a
REM kernel and dosload boots it. About four and a half minutes of
REM simulated hardware -- docs/compiler-on-the-board.md has the cycles.
ECHO ---------------------------------------------
ECHO  Building C4IX with the compiled compiler
ECHO ---------------------------------------------
RUN dostar.c4r x c4ix-src.tar
ECHO [1/12] boot
RUN c4sc.c4r -c 200000 compile -O -c -I src/c4ix src/c4ix/boot.c boot.c4o
ECHO [2/12] console
RUN c4sc.c4r -c 200000 compile -O -c -I src/c4ix src/c4ix/console.c console.c4o
ECHO [3/12] va
RUN c4sc.c4r -c 200000 compile -O -c -I src/c4ix src/c4ix/va.c va.c4o
ECHO [4/12] host
RUN c4sc.c4r -c 200000 compile -O -c -I src/c4ix src/c4ix/host.c host.c4o
ECHO [5/12] sl4b
RUN c4sc.c4r -c 200000 compile -O -c -I src/c4ix src/c4ix/sl4b.c sl4b.c4o
ECHO [6/12] task
RUN c4sc.c4r -c 200000 compile -O -c -I src/c4ix src/c4ix/task.c task.c4o
ECHO [7/12] sched
RUN c4sc.c4r -c 200000 compile -O -c -I src/c4ix src/c4ix/sched.c sched.c4o
ECHO [8/12] vfs
RUN c4sc.c4r -c 200000 compile -O -c -I src/c4ix src/c4ix/vfs.c vfs.c4o
ECHO [9/12] sys
RUN c4sc.c4r -c 200000 compile -O -c -I src/c4ix src/c4ix/sys.c sys.c4o
ECHO [10/12] c4ke
RUN c4sc.c4r -c 200000 compile -O -c -I src/c4ix src/c4ix/c4ke.c c4ke.c4o
ECHO [11/12] loader
RUN c4sc.c4r -c 200000 compile -O -c -I src/c4ix src/c4ix/loader.c loader.c4o
ECHO [12/12] init
RUN c4sc.c4r -c 200000 compile -O -c -I src/c4ix src/c4ix/init.c init.c4o
ECHO Linking...
RUN c4rlink.c4r @c4ix.objs -o c4ix.c4r
ECHO ---------------------------------------------
ECHO  Built. Type RUN dosload.c4r c4ix.c4r to boot it.
ECHO ---------------------------------------------
