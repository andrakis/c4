// C4 Test: tend set at start-up a second time (src/c4sp/tests/common-link.sh):
// common_a.c's __c4lc_init sets it too, so c4rlink stops on the multiple
// definition, as ld does for gcc's two initialized tends

extern int table[4];
int *tend = &table[1];
