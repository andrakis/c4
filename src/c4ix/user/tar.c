// C4IX userland: tar -- list or extract a tar archive, gzipped or not.
//
//   tar t[v] FILE            list
//   tar x[v] [-C DIR] FILE   extract into DIR (default: here)
//
// The letters may carry a leading '-', and 'z' and 'f' are accepted
// for habit's sake: gzip is recognised by its magic number, and the
// file is always the last argument. Reads ustar archives (what GNU tar
// --format=ustar writes) and this repo's own C4TAR1 kits.

#include "c4ix_user.h"
#include "archive.h"

static int usage() {
    ufprintf(STDERR, "usage: tar t[v] FILE | tar x[v] [-C DIR] FILE\n");
    return 2;
}

int main(int argc, char **argv) {
    char *ops, *file, *dest;
    char cwd[128];
    int i, mode, verbose, files;

    if (argc < 3) return usage();
    ops = argv[1];
    if (ops[0] == '-') ++ops;
    mode = -1; verbose = 0;
    while (*ops) {
        if (*ops == 't') mode = ARC_LIST;
        else if (*ops == 'x') mode = ARC_EXTRACT;
        else if (*ops == 'v') verbose = 1;
        else if (*ops != 'z' && *ops != 'f') return usage();
        ++ops;
    }
    if (mode < 0) return usage();

    dest = 0; file = 0;
    i = 2;
    while (i < argc) {
        if (!ustrcmp(argv[i], "-C") && i + 1 < argc) { dest = argv[i + 1]; i = i + 2; }
        else { file = argv[i]; ++i; }
    }
    if (!file) return usage();
    if (!dest) {
        if (ugetcwd(cwd, 128) < 0) { cwd[0] = '/'; cwd[1] = 0; }
        dest = cwd;
    } else if (mode == ARC_EXTRACT && arc_mkdirs(dest) < 0) {
        ufprintf(STDERR, "tar: cannot make %s\n", dest);
        return 1;
    }

    files = arc_extract(file, dest, mode, verbose);
    if (files < 0) {
        ufprintf(STDERR, "tar: %s: %s\n", file, arc_error ? arc_error : "failed");
        return 1;
    }
    if (mode == ARC_EXTRACT && verbose) uprintf("tar: %d files\n", files);
    return 0;
}
