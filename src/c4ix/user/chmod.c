// C4IX userland: chmod -- a file's permission bits.
//
//   chmod MODE FILE...    MODE in octal (644, 755, 4755 ...)
//
// Kept with the file: on a disk in its inode (ext2), in memory otherwise
// (until the next boot, or persist's save, which keeps no modes yet).
// stat and ls show them (docs/c4ix-ext2.md 5).

#include "c4ix_user.h"

enum { SYS_CHMOD = 253 };

static int ksys(int num, int a, int b, int c) { return __c4_opcode(c, b, a, num); }

int main(int argc, char **argv) {
    char *p;
    int mode, k, bad;
    if (argc < 3) { ufprintf(STDERR, "usage: chmod MODE FILE...   (MODE in octal: 644, 755)\n"); return 1; }
    mode = 0;
    p = argv[1];
    if (!*p) { ufprintf(STDERR, "chmod: a mode in octal, as 644\n"); return 1; }
    while (*p) {
        if (*p < '0' || *p > '7') { ufprintf(STDERR, "chmod: %s: a mode in octal, as 644\n", argv[1]); return 1; }
        mode = mode * 8 + *p - '0';
        ++p;
    }
    bad = 0;
    k = 2;
    while (k < argc) {
        if (ksys(SYS_CHMOD, (int)argv[k], mode, 0) < 0) { ufprintf(STDERR, "chmod: %s: cannot\n", argv[k]); bad = 1; }
        ++k;
    }
    return bad;
}
