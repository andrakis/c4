// tests/continue.c: test the 'continue' keyword in c4cc
// We only have while loops currently.

#include <stdio.h>

int main (int argc, char **argv) {
    int i;

    i = 0;
    while (i < 10) {
        if (i++ % 2)
            continue;
        printf("%d is even\n", i - 1);
    }

    return 0;
}
