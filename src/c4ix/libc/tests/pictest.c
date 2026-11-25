// pictest -- two tasks, one copy of libc's code, each its own data.
//
//   pictest TAG N
//
// libc is -fpic: its code is loaded once and shared, and the scheduler
// points __c4_dbase at the running task's copy of its data at every
// switch (src/c4ix/loader.c). Run two of these at once and each keeps
// setting errno, an environment variable and the stdio buffer -- all in
// libc's data -- then sleeps so the other runs, then checks that what it
// left is still there. If the switch did not repoint the data, one
// task's errno or TAG would be the other's, and it says so.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int i, n;
    char *tag;
    char want[16];
    if (argc < 3) { fprintf(stderr, "usage: pictest TAG N\n"); return 2; }
    tag = argv[1];
    n = atoi(argv[2]);
    setenv("TAG", tag, 1);
    i = 0;
    while (i < n) {
        errno = 1000 + i;
        snprintf(want, 16, "%s", tag);
        usleep(2000);
        if (errno != 1000 + i || strcmp(getenv("TAG"), want)) {
            printf("%s: libc's data crossed tasks at %d (errno %d, TAG %s)\n", tag, i, errno, getenv("TAG"));
            return 1;
        }
        printf("%s %d\n", tag, i);
        ++i;
    }
    printf("%s: ok\n", tag);
    return 0;
}
