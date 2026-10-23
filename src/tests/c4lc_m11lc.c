// c4lc_m11lc.c -- M11, step 6 of docs/ports-feasibility.md §8, which only
// c4lc takes (c4fc has no typedef): a typedef of an array type. doomgeneric's
// sha1.h, `typedef byte sha1_digest_t[20];`, used as a global, an extern,
// a struct member, a parameter and in sizeof; and the same of a 2-D array,
// and a pointer to one. The golden (src/c4sp/tests/expected/c4lc-m11lc.txt)
// is gcc's, the same at 32 and 64 bits.

typedef unsigned char byte;
typedef byte digest_t[20];
typedef short grid_t[3][4];
typedef short pair_t[2];

digest_t gd;
extern digest_t xd;
digest_t xd;

struct conn {
    short id;
    digest_t wad;
    digest_t deh;
    short tail;
};

struct conn conns[2];

// a parameter of an array type is a pointer to its element
static void fill(digest_t d, int seed) {
    int i;
    for (i = 0; i < (int) sizeof(digest_t); i++)
        d[i] = (byte) (seed * 7 + i * 13);
}

static unsigned sum(digest_t d) {
    unsigned s = 0;
    int i;
    for (i = 0; i < 20; i++) s = s * 31 + d[i];
    return s;
}

static int gsum(grid_t g) {
    int r, c, s = 0;
    for (r = 0; r < 3; r++)
        for (c = 0; c < 4; c++)
            s += g[r][c] * (r + 1);
    return s;
}

int main() {
    digest_t local;
    grid_t g;
    pair_t pairs[3];
    pair_t *pp;
    int r, c;

    printf("sizes %d %d %d %d\n", (int) sizeof(digest_t), (int) sizeof(grid_t),
           (int) sizeof(pair_t), (int) sizeof(struct conn));
    printf("sizeof local %d, member %d\n", (int) sizeof(local), (int) sizeof(conns[0].wad));

    fill(gd, 1);
    fill(xd, 2);
    fill(local, 3);
    fill(conns[1].wad, 4);
    fill(conns[1].deh, 5);
    conns[1].id = 9;
    conns[1].tail = -3;
    printf("sums %08x %08x %08x %08x %08x\n", sum(gd), sum(xd), sum(local),
           sum(conns[1].wad), sum(conns[1].deh));
    printf("member after %d %d\n", conns[1].id, conns[1].tail);

    memcpy(local, gd, sizeof(digest_t));
    printf("copied %s\n", memcmp(local, gd, sizeof(local)) == 0 ? "same" : "differs");
    printf("offsets %d %d %d\n", (int) ((char *) conns[1].wad - (char *) &conns[1]),
           (int) ((char *) conns[1].deh - (char *) &conns[1]),
           (int) ((char *) &conns[1].tail - (char *) &conns[1]));

    for (r = 0; r < 3; r++)
        for (c = 0; c < 4; c++)
            g[r][c] = r * 10 + c;
    printf("grid %d %d\n", g[2][3], gsum(g));

    for (r = 0; r < 3; r++) { pairs[r][0] = (short) (r + 1); pairs[r][1] = (short) (-r); }
    pp = pairs;
    pp++;
    printf("pairs %d %d %d\n", pp[0][0], pp[0][1], pp[1][0]);
    return 0;
}
