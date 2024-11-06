C4KE - C4 Kernel Experiment
=============================================

c4 was "An exercise in minimalism."

C4KE is "an exploration in extending minimalism." It provides a multitasking kernel, shell, compiler, and utilities.

C4KE runs under a modified `c4` interpreter called `c4m`, which itself can be run be an unmodified `c4` interpreter.
It does however run much faster under a natively compiled `c4m`.


Architecture
------------

C4KE is a pre-emptive multitasking operating system. It uses a cycle-based trap to switch between tasks automatically,
or custom opcodes that can manually switch tasks (cooperative multitasking.)

`c4m` can trap on unknown opcodes, and this feature is used to implement C4KE's extended opcodes. All registers, including
return addresses, can be modified inside these trap handlers. By swapping register values from one task with those from another,
task switching is accomplished transparently to running tasks.

C4KE uses a custom executable format, the "C4 Relocatable." This format contains symbols and runtime patch information.


Running
-------

To run under the extended `c4m` interpreter, use:

	make run

To run under the original `c4` interpreter, use:

	make run-c4


Once in C4KE, type `help` for a command listing, or `ls` to display runnable programs.

Try `bench` or `innerbench` to test out how fast C4KE runs on your system.


Interesting files
-----------------

* [c4ke.c](blob/c4ke/c4ke.c) - The C4 Kernel Experiment, main source file.

* [load-c4r.c](blob/c4ke/load-c4r.c) - The "C4 Relocatable" executable loader and runner.

* [c4cc.c](blob/c4ke/c4cc.c) - A compiler for C4.

* [c4le.c](blob/c4ke/c4le.c) - A C4 Line Editor.

* [c4m.c](blob/c4ke/c4m.c) - An updated C4 interpreter with new opcodes and support for traps. Can be run under C4.

* [c4.c](blob/c4ke/c4.c) - The original C4 interpreter, with only enough modifications to compile portably.

* [u0.c](blob/c4ke/u0.c) - User runtime for C4KE programs, interfaces with C4KE via extended opcodes.

