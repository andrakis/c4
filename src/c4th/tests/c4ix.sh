# c4th as a C4IX program, driven by C4IX's own shell.
#
# Every line here is also a test of spawn, argv, the fd layer and
# redirection -- c4th has never heard of C4IX and asks the kernel for
# nothing it would not ask a bare VM for.
#
# The image is named in full. A bare name would be tried as
# c4ix-c4th.c4r, then as "c4th" -- which in this tree is the gcc build,
# a real file that is not an image, and the loader says so.

echo c4th: the selftest
c4th.c4r -selftest

echo c4th: interpreting a program
c4th.c4r src/c4th/forth/core.f src/c4th/tests/self1.f src/c4th/tests/run-main.f

echo c4th: compiling that program, and running what it built
c4th.c4r src/c4th/forth/core.f src/c4th/forth/self.f src/c4th/tests/self1-c4r.f > /ram/self1.c4r
wc < /ram/self1.c4r
/ram/self1.c4r

# The fixed point, inside the kernel. c4th compiles self.f into a RAM
# file; that image is then a compiler, and it compiles self.f again.
# Nothing here touches the host filesystem except to READ source: the
# VM has no write syscall, and the shell's redirection is standing in
# for one.
echo c4th: compiling the compiler
c4th.c4r src/c4th/forth/core.f src/c4th/forth/self.f src/c4th/tests/run-main.f > /ram/gen.c4r
wc < /ram/gen.c4r

echo c4th: and the compiler it built, compiling itself
/ram/gen.c4r > /ram/gen2.c4r
wc < /ram/gen2.c4r

echo c4th: the second image follows
cat /ram/gen2.c4r
exit 0
