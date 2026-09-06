// bb_whowrote.c - the provenance port, asked and answered.
//
// Two functions each store to a word of their own, then both words are
// asked about. The pin is not "an answer came back" -- a constant would
// pass that. It is that the two answers DIFFER and are both non-zero:
// the port is reporting where the store happened, which is the only
// thing that makes it useful for chasing a write nobody owns up to.
//
// Under a machine without --whowrote these are unclaimed device
// addresses, so both reads return 0 and the program says so and exits
// 0. That is deliberate: announced, never probed (include/c4bb_info.h).
// The test runs it both ways.

enum { WW_QUERY = 420, WW_ANSWER = 424 };   // 0x1a4, 0x1a8

int target_a;
int target_b;

int ask (int *addr) {
    int *q, *ans;
    q = (int *)WW_QUERY;
    ans = (int *)WW_ANSWER;
    *q = (int)addr;
    return *ans;
}

void writer_one () { target_a = 11; }
void writer_two () { target_b = 22; }

int main () {
    int pc_a, pc_b;

    writer_one();
    writer_two();
    pc_a = ask(&target_a);
    pc_b = ask(&target_b);

    if (!pc_a && !pc_b) {
        printf("whowrote: port silent (no --whowrote), a=%d b=%d\n", target_a, target_b);
        return 0;
    }
    printf("whowrote: a written at 0x%X, b written at 0x%X\n", pc_a, pc_b);
    if (!pc_a || !pc_b) { printf("whowrote: FAIL one answer was zero\n"); return 1; }
    if (pc_a == pc_b)   { printf("whowrote: FAIL both stores blamed on one PC\n"); return 1; }
    printf("whowrote: two stores, two different PCs, OK\n");
    return 0;
}
