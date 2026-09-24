Programs to compile inside C4IX
===============================

c4cc is a C compiler for this machine. It has no preprocessor, so a
header is given to it as one more source file, ahead of the program.
It writes what it builds into the RAM filesystem; /ram is a good place.

In a Command Prompt:

  c4cc -o /ram/hello.c4r /usr/src/examples/hello.c
  /ram/hello.c4r one two

  c4cc -o /ram/bounce.c4r /usr/include/window.h /usr/src/examples/bounce.c
  /ram/bounce.c4r

bounce makes its Command Prompt into a window of its own: see
/usr/include/window.h for the calls. q or Esc ends it, and so does
Ctrl-C.

c4th is a Forth that compiles images to its standard output, so the
shell's redirection is where they are written:

  c4th /usr/src/forth/core.f /usr/src/forth/self.f /usr/src/forth/self1-c4r.f > /ram/self1.c4r
  /ram/self1.c4r

c4sp runs c4lc, the optimising C compiler written in Lisp. It is slower
to compile with than c4cc, and has a real preprocessor:

  c4sp c4lc.lisp /usr/src/examples/hello.c /ram/hello2.c4r
  /ram/hello2.c4r

Also here: c4rlink (joins objects), c4rdump (shows what is in an image),
cpp (a preprocessor: cpp file.c > /ram/file.i) and c4sp, the Lisp that
runs the c4lc compiler.
