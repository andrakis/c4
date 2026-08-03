// C4IX userland: cat. With arguments it copies each named file to
// fd 1; with none it copies fd 0, which is what makes it useful in a
// pipeline. Names resolve through the kernel's VFS, so a "file" may
// be a RAM file that never existed on the host.

#include "c4ix_user.h"

enum { CAT_BUF = 256 };

static int cat_fd(int fd) {
    char buf[CAT_BUF];
    int n, total;
    total = 0;
    while ((n = uread(fd, buf, CAT_BUF)) > 0) {
        write(STDOUT, buf, n);
        total = total + n;
    }
    return total;
}

int main(int argc, char **argv) {
    int i, fd, bad;

    if (argc < 2) { cat_fd(STDIN); return 0; }

    bad = 0;
    i = 1;
    while (i < argc) {
        if ((fd = uopen(argv[i], O_RD)) < 0) {
            ufprintf(STDERR, "cat: cannot open %s\n", argv[i]);
            bad = 1;
        } else {
            cat_fd(fd);
            uclose(fd);
        }
        ++i;
    }
    return bad;
}
