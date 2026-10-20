// A header's extern, used, and only defined further down the unit: one
// object, allocated where it is defined (c4lc's data pre-pass skips the
// extern), its symbol listed there too. Plain and initialised, a scalar
// and an array, between globals whose order shows where each landed.
extern int cnt;
int before;
extern int seeded;
extern char buf[];
int bump(void) {
    cnt = cnt + 1;
    before = before + 2;
    buf[1] = buf[1] + 3;
    return cnt + seeded + buf[1];
}
int middle;
int cnt;
int seeded = 5;
char buf[8];
int after;
int main(void) {
    bump();
    middle = after + 1;
    printf("%d %d %d %d %d\n", bump(), cnt, before, middle, buf[1]);
    return 0;
}
