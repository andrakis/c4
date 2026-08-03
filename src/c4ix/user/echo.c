// C4IX userland: echo. Joins its arguments with spaces and ends
// with a newline -- and knows nothing about where fd 1 goes, which
// may be the console, a RAM file, or the write end of a pipe.

#include "c4ix_user.h"

int main(int argc, char **argv) {
    int i;
    i = 1;
    while (i < argc) {
        write(STDOUT, argv[i], ustrlen(argv[i]));
        if (i + 1 < argc) write(STDOUT, " ", 1);
        ++i;
    }
    write(STDOUT, "\n", 1);
    return 0;
}
