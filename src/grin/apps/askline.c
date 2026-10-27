// askline.c -- a DOS program, not a grin application: it asks for a line
// and answers it, with libc's stdio over bare.c, so through C4DOS's
// console slots (16 and 17). test-grin runs it in a DOS Prompt window,
// where its read pumps grin's loop while it waits, and at A> given
// < IN.TXT. (Linked without import.c, so it never says hello to grin.)

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    char line[200];
    int n;
    printf("askline: what is your name?\n");
    fflush(stdout);
    if (!fgets(line, 200, stdin)) { printf("askline: no answer (end of input)\n"); return 1; }
    n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) { --n; line[n] = 0; }
    printf("askline: hello, %s (%d letters)\n", line, n);
    return 0;
}
