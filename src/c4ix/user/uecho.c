// C4IX userland: write each argument to fd 1, one per line. Knows
// nothing about where fd 1 goes -- console, RAM file, or the write
// end of a pipe, whichever its parent handed it.

#include "c4ix_user.h"

int main(int argc, char **argv) {
    int i;
    i = 1;
    while (i < argc) {
        uprintf("%s\n", argv[i]);
        ++i;
    }
    return 0;
}
