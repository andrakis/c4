// libc: string.h and ctype.h. libc.h has the interface.
//
// memcpy, memset and memcmp are the VM's own opcodes underneath
// (opcodes.c): this library is compiled with -fno-builtin, so the names
// are its to define, and the opcodes do the work at host speed.

#include "sys.h"
#include "libc.h"

char *memcpy(char *d, char *s, int n) {
    if (n > 0) __libc_memcpy(d, s, n);
    return d;
}

char *memset(char *d, int c, int n) {
    if (n > 0) __libc_memset(d, c, n);
    return d;
}

int memcmp(char *a, char *b, int n) {
    if (n <= 0) return 0;
    return __libc_memcmp(a, b, n);
}

char *memmove(char *d, char *s, int n) {
    int i;
    if (n <= 0 || d == s) return d;
    if (d < s || d >= s + n) return memcpy(d, s, n);
    // overlapping, destination above: copy from the end
    i = n;
    while (i > 0) { --i; d[i] = s[i]; }
    return d;
}

char *memchr(char *s, int c, int n) {
    int i;
    c = c & 255;
    i = 0;
    while (i < n) { if ((s[i] & 255) == c) return s + i; ++i; }
    return 0;
}

int strlen(char *s) {
    char *p;
    p = s;
    while (*p) ++p;
    return p - s;
}

int strnlen(char *s, int n) {
    int i;
    i = 0;
    while (i < n && s[i]) ++i;
    return i;
}

int strcmp(char *a, char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return (*a & 255) - (*b & 255);
}

int strncmp(char *a, char *b, int n) {
    while (n > 0 && *a && *a == *b) { ++a; ++b; --n; }
    if (n == 0) return 0;
    return (*a & 255) - (*b & 255);
}

int strcasecmp(char *a, char *b) {
    while (*a && tolower(*a) == tolower(*b)) { ++a; ++b; }
    return tolower(*a & 255) - tolower(*b & 255);
}

int strncasecmp(char *a, char *b, int n) {
    while (n > 0 && *a && tolower(*a) == tolower(*b)) { ++a; ++b; --n; }
    if (n == 0) return 0;
    return tolower(*a & 255) - tolower(*b & 255);
}

char *strcpy(char *d, char *s) {
    char *r;
    r = d;
    while ((*d = *s)) { ++d; ++s; }
    return r;
}

char *strncpy(char *d, char *s, int n) {
    int i;
    i = 0;
    while (i < n && s[i]) { d[i] = s[i]; ++i; }
    while (i < n) { d[i] = 0; ++i; }
    return d;
}

char *strcat(char *d, char *s) {
    strcpy(d + strlen(d), s);
    return d;
}

char *strncat(char *d, char *s, int n) {
    char *p;
    p = d + strlen(d);
    while (n > 0 && *s) { *p++ = *s++; --n; }
    *p = 0;
    return d;
}

char *strchr(char *s, int c) {
    c = c & 255;
    while (1) {
        if ((*s & 255) == c) return s;
        if (!*s) return 0;
        ++s;
    }
    return 0;
}

char *strrchr(char *s, int c) {
    char *r;
    c = c & 255;
    r = 0;
    while (1) {
        if ((*s & 255) == c) r = s;
        if (!*s) return r;
        ++s;
    }
    return r;
}

char *strstr(char *h, char *n) {
    int k;
    if (!*n) return h;
    k = strlen(n);
    while (*h) {
        if (*h == *n && !strncmp(h, n, k)) return h;
        ++h;
    }
    return 0;
}

char *strdup(char *s) {
    char *r;
    int n;
    n = strlen(s);
    if (!(r = malloc(n + 1))) return 0;
    memcpy(r, s, n + 1);
    return r;
}

char *strndup(char *s, int n) {
    char *r;
    n = strnlen(s, n);
    if (!(r = malloc(n + 1))) return 0;
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

int strspn(char *s, char *accept) {
    int n;
    n = 0;
    while (s[n] && strchr(accept, s[n])) ++n;
    return n;
}

int strcspn(char *s, char *reject) {
    int n;
    n = 0;
    while (s[n] && !strchr(reject, s[n])) ++n;
    return n;
}

char *strpbrk(char *s, char *accept) {
    while (*s) { if (strchr(accept, *s)) return s; ++s; }
    return 0;
}

char *strtok_r(char *s, char *delim, char **save) {
    char *t;
    if (!s) s = *save;
    if (!s) return 0;
    s = s + strspn(s, delim);
    if (!*s) { *save = 0; return 0; }
    t = s + strcspn(s, delim);
    if (*t) { *t = 0; *save = t + 1; }
    else *save = 0;
    return s;
}

static char *strtok_save;
char *strtok(char *s, char *delim) { return strtok_r(s, delim, &strtok_save); }

// ---- ctype: ASCII ----

int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
int isblank(int c) { return c == ' ' || c == 9; }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int isprint(int c) { return c >= 32 && c < 127; }
int isgraph(int c) { return c > 32 && c < 127; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }
