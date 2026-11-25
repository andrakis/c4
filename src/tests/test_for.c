// tests/test_for.c: the for statement, which c4cc could not compile at
// all until 2026-08-26 -- the branch never consumed its own keyword, so
// every for-loop died on "open paren expected". Nothing in this tree
// writes one (it was all written against c4, which has no for), which
// is why nobody noticed. See src/c4cc/c4cc.c's For branch.
//
// Every section of the header is exercised, including the empty ones,
// plus break, continue and nesting. The expected output is gcc's.

#include <stdio.h>

int main (int argc, char **argv) {
    int i, j, s;

    s = 0;
    for (i = 0; i < 5; ++i) s = s + i;
    printf("sum %d\n", s);

    for (i = 0; i < 5; ++i) {
        if (i == 2) continue;
        if (i == 4) break;
        printf("i %d\n", i);
    }

    s = 0; i = 0;
    for (; i < 3; ) { s = s + 10; ++i; }
    printf("empty-sections %d\n", s);

    for (i = 0; i < 3; ++i)
        for (j = 0; j < 2; ++j)
            printf("n %d%d\n", i, j);

    // A loop that never runs: the condition has to be tested BEFORE the
    // step, which is the bug the old layout would have had even with the
    // keyword fixed.
    s = 0;
    for (i = 10; i < 3; ++i) s = s + 1;
    printf("never %d %d\n", s, i);

    return 0;
}
