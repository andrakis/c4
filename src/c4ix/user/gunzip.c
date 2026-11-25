// C4IX userland: gunzip -- inflate a .gz file.
//
//   gunzip FILE.gz         writes FILE, keeps FILE.gz (there is no undo)
//   gunzip -c FILE.gz      writes to standard output
//
// The CRC in the gzip trailer is checked before anything is written.

#include "c4ix_user.h"
#include "archive.h"

int main(int argc, char **argv) {
    char *in, *out, *file;
    char name[128];
    int inlen, outlen, tostdout, n, fd, i;

    tostdout = 0; file = 0;
    i = 1;
    while (i < argc) {
        if (!ustrcmp(argv[i], "-c")) tostdout = 1;
        else file = argv[i];
        ++i;
    }
    if (!file) { ufprintf(STDERR, "usage: gunzip [-c] FILE.gz\n"); return 2; }

    in = arc_readfile(file, &inlen);
    if (!in) { ufprintf(STDERR, "gunzip: cannot read %s\n", file); return 1; }
    if (arc_gunzip(in, inlen, &out, &outlen) < 0) {
        ufprintf(STDERR, "gunzip: %s: %s\n", file, arc_error);
        return 1;
    }

    if (tostdout) {
        fd = STDOUT;
    } else {
        n = ustrlen(file);
        if (n < 4 || n > 120 || file[n - 3] != '.' || file[n - 2] != 'g' || file[n - 1] != 'z') {
            ufprintf(STDERR, "gunzip: %s: no .gz suffix; use -c\n", file);
            return 1;
        }
        memcpy(name, file, n - 3);
        name[n - 3] = 0;
        fd = uopen(name, O_WR + O_CREATE + O_TRUNCATE);
        if (fd < 0) { ufprintf(STDERR, "gunzip: cannot write %s\n", name); return 1; }
    }
    n = write(fd, out, outlen);
    if (!tostdout) uclose(fd);
    return n == outlen ? 0 : 1;
}
