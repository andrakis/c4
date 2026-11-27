// dirtest.c: opendir, readdir and stat of directories (test-libc-bare,
// against gcc's build). Run in a directory holding ab.txt, Cd.TXT and
// sub/ with x.dat in it; on the bare machine each directory's listing is
// its c4dos.dir (bare.c), which is left out here as the host has its own.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>

static char names[16][64];
static int count;

static int cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void list(char *path) {
    DIR *d;
    struct dirent *e;
    struct stat st;
    char full[200];
    int i;
    count = 0;
    if (!(d = opendir(path))) { printf("%s: no listing\n", path); return; }
    while ((e = readdir(d)) && count < 16) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, "c4dos.dir")) continue;
        strcpy(names[count++], e->d_name);
    }
    closedir(d);
    qsort(names, count, 64, cmp);
    printf("%s:", path);
    for (i = 0; i < count; i++) {
        snprintf(full, sizeof full, path[strlen(path) - 1] == '/' ? "%s%s" : "%s/%s", path, names[i]);
        if (stat(full, &st) < 0) printf(" %s(?)", names[i]);
        else printf(" %s%s", names[i], S_ISDIR(st.st_mode) ? "/" : S_ISREG(st.st_mode) ? "" : "(!)");
    }
    printf("\n");
}

static void one(char *path) {
    struct stat st;
    if (stat(path, &st) < 0) printf("stat %s: none\n", path);
    else printf("stat %s: dir %d reg %d size %d\n", path, S_ISDIR(st.st_mode) != 0, S_ISREG(st.st_mode) != 0,
                S_ISDIR(st.st_mode) ? 0 : (int)st.st_size);
}

int main() {
    list(".");
    list("./");
    list("sub");
    list("sub/");
    list("./sub");
    list("missing");
    list("sub/missing");
    list("ab.txt");
    one("sub");
    one("sub/");
    one("ab.txt");
    one("sub/x.dat");
    one("missing");
    one("missing/sub");
    one("sub/sub");
    return 0;
}
