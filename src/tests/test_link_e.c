// Linking test, extern DATA half A: this module DEFINES the shared
// globals (scalar, array, char array, struct, function-pointer slot)
// and a constructor that fills them in. test_link_f.c declares them
// extern and has main; c4rlink resolves the references. Compiled by
// c4lc -c (c4cc has no extern-data support). gcc is the oracle.

struct Node { int value; struct Node *next; };

int counter;
int totals[4];
char tag[8];
struct Node origin;
int hook;

int bump(int n) {
    counter = counter + n;
    return counter;
}

static void __attribute__((constructor)) e_init() {
    int i;
    counter = 5;
    i = 0;
    while (i < 4) { totals[i] = (i + 1) * 10; ++i; }
    tag[0] = 'E'; tag[1] = '4'; tag[2] = 0;
    origin.value = 41;
    origin.next = 0;
    hook = (int)&bump;
}
