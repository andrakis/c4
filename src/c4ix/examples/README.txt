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

cc compiles with c4lc, the optimising C compiler, which has a real
preprocessor. It runs c4sc -- c4lc compiled to an image -- so it is
many times faster than c4sp running c4lc's Lisp:

  cc -o /ram/hello2.c4r /usr/src/examples/hello.c
  /ram/hello2.c4r

-O optimises, -I and -D go to the preprocessor (/usr/include is always
searched), and -conforming reads escapes such as "\033" the way C
does. -l c4ix links in C4IX's own calls, the ones c4ix_user.h declares,
so the userland's programs build the same way:

  cc -O -l c4ix -o /ram/echo.c4r /usr/src/c4/src/c4ix/user/echo.c
  /ram/echo.c4r it works

  cc -O -conforming -I /usr/src/c4/src/c4tui -o /ram/demo.c4r /usr/src/c4/src/c4tui/demo.c
  /ram/demo.c4r

The repos own programs build as they are: cc also searches the repos
headers (/usr/src/c4/include: u0.h, and C versions of stdio.h and the
rest), so

  cd /usr/src/c4/src/tests
  cc -O mandel.c
  ./mandel.c4r

-c leaves an object for c4rlink, and -v shows the commands cc runs.
Interpreted, the same compiler is still there:

  c4sp c4lc.lisp /usr/src/examples/hello.c /ram/hello3.c4r

Also here: c4rlink (joins objects), c4rdump (shows what is in an image),
cpp (a preprocessor: cpp file.c > /ram/file.i) and c4sp, the Lisp that
runs the c4lc compiler.
