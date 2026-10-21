Programs to compile inside C4IX
===============================

c4cc is a C compiler for this machine. It has no preprocessor, so a
header is given to it as one more source file, ahead of the program.
It writes what it builds anywhere in the filesystem; /tmp is the place
for scratch, and /usr is the repo, sources and all.

In a Terminal:

  c4cc -o /tmp/hello.c4r /usr/src/c4ix/examples/hello.c
  /tmp/hello.c4r one two

  c4cc -o /tmp/bounce.c4r /usr/include/window.h /usr/src/c4ix/examples/bounce.c
  /tmp/bounce.c4r

bounce makes its Terminal into a window of its own: see
/usr/include/window.h for the calls. q or Esc ends it, and so does
Ctrl-C.

c4th is a Forth that compiles images to its standard output, so the
shell's redirection is where they are written:

  c4th /usr/src/c4th/forth/core.f /usr/src/c4th/forth/self.f /usr/src/c4th/tests/self1-c4r.f > /tmp/self1.c4r
  /tmp/self1.c4r

cc compiles with c4lc, the optimising C compiler, which has a real
preprocessor. It runs c4sc -- c4lc compiled to an image -- so it is
many times faster than c4sp running c4lc's Lisp:

  cc -o /tmp/hello2.c4r /usr/src/c4ix/examples/hello.c
  /tmp/hello2.c4r

-O optimises, -I and -D go to the preprocessor (/usr/include is always
searched), and -conforming reads escapes such as "\033" the way C
does. -l c4ix links in C4IX's own calls, the ones c4ix_user.h declares,
so the userland's programs build the same way:

  cc -O -l c4ix -o /tmp/echo.c4r /usr/src/c4ix/user/echo.c
  /tmp/echo.c4r it works

  cc -O -conforming -I /usr/src/c4tui -o /tmp/demo.c4r /usr/src/c4tui/demo.c
  /tmp/demo.c4r

The repos own programs build as they are: cc also searches the repos
headers (/usr/include: u0.h, and C versions of stdio.h and the
rest), so

  cd /usr/src/tests
  cc -O mandel.c
  ./mandel.c4r

-c leaves an object for c4rlink, and -v shows the commands cc runs.
Interpreted, the same compiler is still there:

  c4sp c4lc.lisp /usr/src/c4ix/examples/hello.c /tmp/hello3.c4r

Also here: c4rlink (joins objects), c4rdump (shows what is in an image),
cpp (a preprocessor: cpp file.c > /tmp/file.i) and c4sp, the Lisp that
runs the c4lc compiler.
