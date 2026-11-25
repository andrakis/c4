// C4IX userland: mkdir.
#include "c4ix_user.h"

int main(int argc, char **argv) {
    int i, bad;
    if (argc < 2) { ufprintf(STDERR, "usage: mkdir dir...\n"); return 1; }
    bad = 0;
    i = 1;
    while (i < argc) {
        if (umkdir(argv[i]) < 0) {
            ufprintf(STDERR, "mkdir: cannot create %s\n", argv[i]);
            bad = 1;
        }
        ++i;
    }
    return bad;
}
