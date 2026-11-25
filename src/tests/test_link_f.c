// Linking test, extern DATA half B: declares test_link_e.c's globals
// extern and exercises every reference shape -- scalar load/store,
// array indexing and element store, char array as a string, struct
// member read/write, & of an extern struct, and a function address
// carried through an extern global and compared against &fn taken
// via an extern PROTOTYPE (both sides resolve through symbol
// patches; equality proves they agree). No sizeof prints: int is
// 4 bytes under the gcc oracle and 8 here, and no indirect call
// through the slot: gcc will not call through an int.

struct Node { int value; struct Node *next; };

extern int counter;
extern int totals[4];
extern char tag[8];
extern struct Node origin;
extern int hook;

int bump(int n);

int main() {
    int i, sum;
    struct Node second;

    printf("counter %d\n", counter);
    counter = counter + 1;
    printf("bump %d\n", bump(10));

    sum = 0;
    i = 0;
    while (i < 4) { sum = sum + totals[i]; ++i; }
    totals[2] = 99;
    printf("sum %d totals2 %d tag %s\n", sum, totals[2], tag);

    second.value = 1;
    second.next = &origin;
    origin.value = origin.value + 1;
    printf("origin %d via %d\n", origin.value, second.next->value);

    printf("hook %d\n", hook == (int)&bump);
    return 0;
}
