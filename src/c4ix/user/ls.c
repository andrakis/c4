// C4IX userland: ls. Asks the kernel for one directory entry at a
// time, since the directory tree is kernel memory. Directories are
// marked with a trailing '/'.
//
// ls [PATH...]: the files named first, then each directory named, under
// its name when there are several -- the shell expands ls *.c into the
// names, as sh does.

#include "c4ix_user.h"

static void ls_dir(char *path, char *shown) {
    char name[64];             // VN_NAME_MAX
    char cwd[128];
    int i, kind, n;

    n = 0;
    i = 0;
    while ((kind = ureaddir(path, i, name)) >= 0) {
        uprintf("%s%s\n", name, kind ? "/" : "");
        ++n;
        ++i;
    }
    if (!n) {
        if (shown) uprintf("(%s is empty)\n", shown);
        else if (ugetcwd(cwd, 128) > 0) uprintf("(%s is empty)\n", cwd);
        else uprintf("(empty)\n");
    }
}

int main(int argc, char **argv) {
    int k, st[4], many;

    if (argc < 2) { ls_dir(".", 0); return 0; }
    k = 1;
    while (k < argc) {
        if (ustat(argv[k], st) < 0) uprintf("ls: %s: no such file or directory\n", argv[k]);
        else if (st[0] != 1) uprintf("%s\n", argv[k]);
        ++k;
    }
    many = argc > 2;
    k = 1;
    while (k < argc) {
        if (ustat(argv[k], st) == 0 && st[0] == 1) {
            if (many) uprintf("%s:\n", argv[k]);
            ls_dir(argv[k], argv[k]);
        }
        ++k;
    }
    return 0;
}
