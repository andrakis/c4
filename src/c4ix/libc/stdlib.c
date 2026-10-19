// libc: stdlib.h. libc.h has the interface.
//
// malloc is the VM's allocator (the MALC opcode, which C4IX services for
// a protected task), with one word in front of each block holding its
// size: that is all realloc needs to copy, since the VM's own realloc
// opcode is not one a protected task may use.

#include "sys.h"
#include "libc.h"

char *malloc(int n) {
    int *p;
    if (n < 0) { errno = ENOMEM; return 0; }
    p = (int *)__libc_malloc(n + sizeof(int));
    if (!p) { errno = ENOMEM; return 0; }
    p[0] = n;
    return (char *)(p + 1);
}

void free(char *p) {
    if (p) __libc_free((char *)((int *)p - 1));
}

char *calloc(int n, int size) {
    char *p;
    if (n < 0 || size < 0) { errno = ENOMEM; return 0; }
    if ((p = malloc(n * size))) memset(p, 0, n * size);
    return p;
}

char *realloc(char *p, int n) {
    char *q;
    int old;
    if (!p) return malloc(n);
    if (n == 0) { free(p); return 0; }
    old = ((int *)p)[-1];
    if (n <= old) return p;
    if (!(q = malloc(n))) return 0;
    memcpy(q, p, old);
    free(p);
    return q;
}

// ---- exit ----

static int *atexit_fns;
static int atexit_n;

int atexit(int fn) {
    if (!atexit_fns) atexit_fns = (int *)malloc(32 * sizeof(int));
    if (!atexit_fns || atexit_n >= 32) return -1;
    atexit_fns[atexit_n++] = fn;
    return 0;
}

void exit(int code) {
    int fn;
    while (atexit_n > 0) { fn = atexit_fns[--atexit_n]; fn(); }
    fflush(stdout);
    fflush(stderr);
    uexit(code);
}

void _exit(int code) { uexit(code); }

void abort() {
    fflush(stdout);
    write(2, "abort\n", 6);
    uexit(134);
}

int __assert_fail(char *e, char *file, int line) {
    fprintf(stderr, "assertion failed: %s\n", e);
    abort();
    return 0;
}

// ---- numbers ----

int abs(int v) { return v < 0 ? -v : v; }
int labs(int v) { return v < 0 ? -v : v; }

static int digit_of(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

// the value of S in BASE (0: by its prefix), its sign in *NEG; *END past it
static unsigned int parse_num(char *s, char **end, int base, int *neg) {
    unsigned int v;
    int d, any;
    char *p;
    p = s;
    while (isspace(*p)) ++p;
    *neg = 0;
    if (*p == '-') { *neg = 1; ++p; } else if (*p == '+') ++p;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && digit_of(p[2]) < 16) { p = p + 2; base = 16; }
    else if (base == 0 && p[0] == '0') base = 8;
    else if (base == 0) base = 10;
    v = 0; any = 0;
    while ((d = digit_of(*p)) < base) { v = v * base + d; ++p; any = 1; }
    if (end) *end = any ? p : s;
    return v;
}

int strtol(char *s, char **end, int base) {
    unsigned int v;
    int neg;
    v = parse_num(s, end, base, &neg);
    return neg ? -(int)v : (int)v;
}

unsigned int strtoul(char *s, char **end, int base) {
    unsigned int v;
    int neg;
    v = parse_num(s, end, base, &neg);
    return neg ? -v : v;
}

int atoi(char *s) { return strtol(s, 0, 10); }
int atol(char *s) { return strtol(s, 0, 10); }

// ---- sorting ----

static void swap_bytes(char *a, char *b, int n) {
    int t;
    while (n > 0) { t = *a; *a = *b; *b = t; ++a; ++b; --n; }
}

// Shell sort: no recursion, a few lines, and fine at this machine's sizes.
// CMP is a function (c4lc calls any variable): cmp(a, b) < 0 when a sorts first.
void qsort(char *base, int n, int size, int cmp) {
    int gap, i, j;
    char *a, *b;
    gap = n / 2;
    while (gap > 0) {
        i = gap;
        while (i < n) {
            j = i - gap;
            while (j >= 0) {
                a = base + j * size;
                b = base + (j + gap) * size;
                if (cmp(a, b) <= 0) break;
                swap_bytes(a, b, size);
                j = j - gap;
            }
            ++i;
        }
        gap = gap / 2;
    }
}

char *bsearch(char *key, char *base, int n, int size, int cmp) {
    int lo, hi, mid, r;
    lo = 0; hi = n - 1;
    while (lo <= hi) {
        mid = (lo + hi) / 2;
        r = cmp(key, base + mid * size);
        if (r == 0) return base + mid * size;
        if (r < 0) hi = mid - 1; else lo = mid + 1;
    }
    return 0;
}

// ---- the environment: this task's own, empty at start (C4IX passes none) ----

static char **env_names;
static char **env_values;
static int env_n;

static int env_find(char *name) {
    int i;
    i = 0;
    while (i < env_n) { if (!strcmp(env_names[i], name)) return i; ++i; }
    return -1;
}

char *getenv(char *name) {
    int i;
    if ((i = env_find(name)) < 0) return 0;
    return env_values[i];
}

int setenv(char *name, char *value, int overwrite) {
    int i;
    if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
    if (!env_names) {
        env_names = (char **)malloc(64 * sizeof(int));
        env_values = (char **)malloc(64 * sizeof(int));
    }
    if ((i = env_find(name)) >= 0) {
        if (!overwrite) return 0;
        free(env_values[i]);
        env_values[i] = strdup(value);
        return 0;
    }
    if (env_n >= 64) { errno = ENOMEM; return -1; }
    env_names[env_n] = strdup(name);
    env_values[env_n] = strdup(value);
    ++env_n;
    return 0;
}

int unsetenv(char *name) {
    int i;
    if ((i = env_find(name)) < 0) return 0;
    free(env_names[i]); free(env_values[i]);
    --env_n;
    env_names[i] = env_names[env_n];
    env_values[i] = env_values[env_n];
    return 0;
}

// ---- rand: the C standard's example generator ----

static unsigned int rand_next = 1;

int rand() {
    rand_next = rand_next * 1103515245 + 12345;
    return (int)((rand_next / 65536) % 32768);
}

void srand(unsigned int seed) { rand_next = seed; }
