// C4IX userland: ls. Asks the kernel for one directory entry at a
// time, since the directory tree is kernel memory. Directories are
// marked with a trailing '/'.

#include "c4ix_user.h"

int main(int argc, char **argv) {
    char name[32];
    char cwd[128];
    char *path;
    int i, kind, n;

    path = (argc > 1) ? argv[1] : ".";
    n = 0;
    i = 0;
    while ((kind = ureaddir(path, i, name)) >= 0) {
        uprintf("%s%s\n", name, kind ? "/" : "");
        ++n;
        ++i;
    }
    if (!n) {
        if (ugetcwd(cwd, 128) > 0) uprintf("(%s is empty)\n", cwd);
        else uprintf("(empty)\n");
    }
    return 0;
}
