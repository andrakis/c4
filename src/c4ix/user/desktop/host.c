// host.c -- what the window kit asks of its host (winkit.h), from C4IX:
// memory from the kernel, and files in its filesystem.

#include "desktop.h"

// libc's fp.c (the Calculator's conversions) sets errno; the desktop has
// no libc to own one
int errno;

char *wk_alloc(int n) {
    char *p;
    if (n <= 0) n = 1;
    if ((p = (char *)ualloc(n))) memset(p, 0, n);
    return p;
}
void wk_free(char *p) { if (p) free(p); }

int wk_fopen(char *path, int wr) { return uopen(path, wr ? O_WR | O_CREATE | O_TRUNCATE : O_RD); }
int wk_fread(int fd, char *buf, int n) { return uread(fd, buf, n); }
int wk_fwrite(int fd, char *buf, int n) { return write(fd, buf, n); }
void wk_fclose(int fd) { uclose(fd); }
int wk_readdir(char *dir, int i, char *name) { return ureaddir(dir, i, name); }
