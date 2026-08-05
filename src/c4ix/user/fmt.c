// C4IX userland: fmt. A self-checking test of the two printf
// implementations that a C4IX program can reach.
//
// There are two, and they must agree:
//
//   uprintf()  formats in userland (libc4ix's uformat) and writes the
//              result through one write(fd) syscall.
//   printf()   is the PRTF opcode. Behind protected mode that traps,
//              and the KERNEL formats it (sys.c's sys_vprintf) before
//              putting it on the same fd.
//
// A program's output must not depend on which one it used, so this
// prints every case twice -- "u|" through the first, "p|" through the
// second -- and the test extracts the two sets and compares them. Any
// drift between the parsers shows up as a diff rather than as a
// mysteriously misaligned column three releases later.
//
// The cases are the ones C4KE's ps and top actually use, which is why
// they exist at all: %*s that takes its width from an argument, %3ld
// with a length modifier, %03d, a negative %*d width meaning
// left-justify (ps.c computes one), and %% after a width.

#include "c4ix_user.h"

int main(int argc, char **argv) {
    uprintf("u|[%*s]\n", 6, "ab");
    printf("p|[%*s]\n", 6, "ab");

    uprintf("u|[%3ld]\n", 42);
    printf("p|[%3ld]\n", 42);

    uprintf("u|[%03d]\n", 7);
    printf("p|[%03d]\n", 7);

    uprintf("u|[%-4d]\n", 5);
    printf("p|[%-4d]\n", 5);

    uprintf("u|[%4ld.%03d]\n", 12, 7);
    printf("p|[%4ld.%03d]\n", 12, 7);

    uprintf("u|[%2d]\n", 123);
    printf("p|[%2d]\n", 123);

    uprintf("u|[%3d%%]\n", 50);
    printf("p|[%3d%%]\n", 50);

    // a negative width arrives as an argument and means left-justify
    uprintf("u|[%*d]\n", -6, 42);
    printf("p|[%*d]\n", -6, 42);

    uprintf("u|[%05d]\n", -42);
    printf("p|[%05d]\n", -42);

    uprintf("u|[%.5d]\n", 42);
    printf("p|[%.5d]\n", 42);

    uprintf("u|[%.3s]\n", "abcdef");
    printf("p|[%.3s]\n", "abcdef");

    uprintf("u|[%8.3s]\n", "abcdef");
    printf("p|[%8.3s]\n", "abcdef");

    uprintf("u|[%x][%X]\n", 255, 255);
    printf("p|[%x][%X]\n", 255, 255);

    uprintf("u|[%c][%3c]\n", 'x', 'y');
    printf("p|[%c][%3c]\n", 'x', 'y');

    uprintf("u|[%s][%8s][%-8s]\n", "hi", "hi", "hi");
    printf("p|[%s][%8s][%-8s]\n", "hi", "hi", "hi");

    // An unknown conversion prints verbatim and consumes NOTHING, so
    // the argument after it still lands where it should.
    uprintf("u|[%q][%d]\n", 9);
    printf("p|[%q][%d]\n", 9);

    return 0;
}
