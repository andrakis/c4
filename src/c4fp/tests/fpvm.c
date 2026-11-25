// fpvm.c -- softfp.c compiled by c4lc, against vectors the host's SSE
// made (fpgen.c): `fpvm FILE` prints each case it gets wrong and a count.
// Run at both word sizes; the same file must pass on both.
#include "softfp.c"

char *buf;
int pos, len;

unsigned int hexword() {
    unsigned int v;
    int c;
    while (pos < len && (buf[pos] == ' ' || buf[pos] == 10)) ++pos;
    v = 0;
    while (pos < len) {
        c = buf[pos];
        if (c >= '0' && c <= '9') v = (v << 4) | (c - '0');
        else if (c >= 'a' && c <= 'f') v = (v << 4) | (c - 'a' + 10);
        else break;
        ++pos;
    }
    return v;
}

unsigned long long pair() {
    unsigned int hi;
    hi = hexword();
    return ((unsigned long long)hi << 32) | hexword();
}

int main(int argc, char **argv) {
    int fd, n, op, bad, cases;
    unsigned long long a, b, want, got;
    unsigned int fa, fb2;
    if (argc < 2) { printf("usage: fpvm FILE\n"); return 2; }
    fd = open(argv[1], 0);
    if (fd < 0) { printf("fpvm: cannot open %s\n", argv[1]); return 2; }
    buf = malloc(2000000);
    len = 0;
    while ((n = read(fd, buf + len, 65536)) > 0) len = len + n;
    close(fd);
    pos = 0; bad = 0; cases = 0;
    while (1) {
        while (pos < len && (buf[pos] == ' ' || buf[pos] == 10)) ++pos;
        if (pos >= len) break;
        op = hexword();
        a = pair(); b = pair(); want = pair();
        fa = (unsigned int)a; fb2 = (unsigned int)b;
        switch (op) {
        case 1: got = __c4f_add(fa, fb2); break;
        case 2: got = __c4f_sub(fa, fb2); break;
        case 3: got = __c4f_mul(fa, fb2); break;
        case 4: got = __c4f_div(fa, fb2); break;
        case 5: case 29: got = __c4f_sqrt(fa); break;
        case 6: got = (unsigned long long)(long long)__c4f_cmp(fa, fb2); break;
        case 7: got = __c4f_to_d(fa); break;
        case 8: got = (unsigned long long)__c4f_to_ll(fa); break;
        case 9: got = (unsigned long long)(long long)__c4f_to_i(fa); break;
        case 10: got = __c4f_to_ull(fa); break;
        case 11: got = __c4d_add(a, b); break;
        case 12: got = __c4d_sub(a, b); break;
        case 13: got = __c4d_mul(a, b); break;
        case 14: got = __c4d_div(a, b); break;
        case 15: case 30: got = __c4d_sqrt(a); break;
        case 16: got = (unsigned long long)(long long)__c4d_cmp(a, b); break;
        case 17: got = __c4d_to_f(a); break;
        case 18: got = (unsigned long long)__c4d_to_ll(a); break;
        case 19: got = (unsigned long long)(long long)__c4d_to_i(a); break;
        case 20: got = __c4d_to_ull(a); break;
        case 21: got = __c4f_from_ll((long long)a); break;
        case 22: got = __c4f_from_ull(a); break;
        case 23: got = __c4f_from_i((int)(long long)a); break;
        case 24: got = __c4f_from_u((unsigned int)a); break;
        case 25: got = __c4d_from_ll((long long)a); break;
        case 26: got = __c4d_from_ull(a); break;
        case 27: got = __c4d_from_i((int)(long long)a); break;
        case 28: got = __c4d_from_u((unsigned int)a); break;
        default: got = want + 1;
        }
        ++cases;
        if (got != want) {
            if (bad < 20)
                printf("op %d %x %x %x %x: want %x %x got %x %x\n", op, (unsigned int)(a >> 32), (unsigned int)a,
                       (unsigned int)(b >> 32), (unsigned int)b, (unsigned int)(want >> 32), (unsigned int)want,
                       (unsigned int)(got >> 32), (unsigned int)got);
            ++bad;
        }
    }
    printf("fpvm: %d cases, %d wrong\n", cases, bad);
    return bad != 0;
}
