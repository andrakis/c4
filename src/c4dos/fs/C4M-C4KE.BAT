@ECHO OFF
REM Boot C4KE under c4m, from inside C4DOS.
REM
REM This is the host's `run-c4` target moved inside the machine: there,
REM ./c4 runs c4m.c from source and c4m runs the kernel image. Here
REM C4DOS runs c4m.c4r -- the C4DOS build of the same c4m.c -- and c4m
REM runs the same kernel image.
REM
REM It matters because c4m.c4r is STOCK C4: it uses no opcode beyond
REM EXIT, so it runs on a machine that has only the original c4 set,
REM and every c4m opcode C4KE needs (MCPY, OPCD, C4IV, TLEV, the trap
REM machinery) is then provided by c4m's own interpreter loop rather
REM than by the CPU underneath. That is why this works under plain c4
REM where RUN c4ke.c4r cannot.
REM
REM c4ke.c4r has to be on the disk already -- BUILD.BAT makes one, or
REM the host does. Once it is here, init.c4r, c4sh.c4r and vfsload.c4r
REM are what the kernel goes looking for.
ECHO ---------------------------------------------
ECHO  C4KE under c4m, inside C4DOS
ECHO ---------------------------------------------
RUN c4m.c4r load-c4r.c -- c4ke.c4r
