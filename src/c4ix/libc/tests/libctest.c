// libctest -- C4IX's libc, exercised. Compiled with -fno-builtin against
// src/c4ix/libc/include and linked with -l libc.c4s; its output is
// compared with libctest.txt (libjs/tests/test-libjs.sh). Every line
// says what it checks, so a difference names the function.
//
//   libctest -v -n 3 one two

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <math.h>

int by_value(int *a, int *b) { return *a - *b; }
int by_name(char **a, char **b) { return strcmp(*a, *b); }

int main(int argc, char **argv) {
    int nums[8];
    char *names[5];
    char buf[64];
    char *s, *end, *p;
    FILE *f;
    DIR *d;
    struct dirent *e;
    int i, c, n, verbose, seen;
    double x;

    // printf: flags, widths, precisions, bases
    printf("printf: [%d] [%5d] [%-5d|] [%05d] [%+d] [% d]\n", 42, 42, 42, 42, 42, 42);
    printf("printf: [%x] [%X] [%#x] [%o] [%#o] [%u]\n", 255, 255, 255, 8, 8, 4000000000u);
    printf("printf: [%s] [%10s] [%-10s|] [%.3s] [%c%c]\n", "c4ix", "right", "left", "truncate", 'o', 'k');
    printf("printf: [%d] [%d] [%*d] [%.*s] [%%]\n", -2147483647 - 1, 0, 6, -7, 2, "precision");
    n = snprintf(buf, 8, "%s-%d", "overflowing", 123);
    printf("snprintf: \"%s\" wanted %d\n", buf, n);

    // floating point: printf's conversions, strtod, math.h
    printf("float: [%f] [%.2f] [%10.3f] [%-8.1f|] [%+.0f] [%08.2f]\n", 3.14159265358979, 2.675, -1.5, 0.25, 2.5, -3.14159);
    printf("float: [%e] [%.3E] [%g] [%g] [%g] [%#g]\n", 123456.789, 0.000123456, 100000.0, 1000000.0, 0.0001, 1.0);
    printf("float: [%f] [%F] [%5.1f] [%-6e|] [%G]\n", 1.0 / 0.0, -1.0 / 0.0, strtod("nan", 0), 0.0, 1e-10);
    x = strtod("  -12.5e2xyz", &end);
    printf("strtod: %g rest \"%s\" %g %.9g\n", x, end, atof("0x1p-3"), strtof("0.1", 0));
    errno = 0;
    x = strtod("1e400", 0);
    printf("strtod: %f erange %d %.17g\n", x, errno == ERANGE, strtod("2.2250738585072011e-308", 0));
    printf("math: %.17g %.17g %.17g %.17g\n", sqrt(2.0), sin(1.0), exp(1.0), log(10.0));
    printf("math: %g %g %g %g %.17g %.17g\n", floor(-2.5), ceil(-2.5), round(2.5), fmod(7.5, 2), pow(2, 0.5), atan2(1, 1) * 4);
    printf("math: %.17g %.17g %g %g\n", cos(1e22), tan(0.5), cbrt(-27.0), hypot(3, 4));

    // string.h
    strcpy(buf, "hello");
    strcat(buf, ", world");
    printf("string: %s len %d cmp %d %d %d\n", buf, strlen(buf), strcmp("a", "b") < 0, strcmp("b", "a") > 0, strcmp("x", "x"));
    printf("string: chr %s rchr %s str %s spn %d cspn %d\n", strchr(buf, 'o'), strrchr(buf, 'o'), strstr(buf, "wor"), strspn("aabc", "a"), strcspn("abc,d", ","));
    strcpy(buf, "one two  three");
    s = strtok(buf, " ");
    printf("strtok:");
    while (s) { printf(" <%s>", s); s = strtok(0, " "); }
    printf("\n");
    printf("strcasecmp: %d ncmp %d dup %s\n", strcasecmp("HeLLo", "hello"), strncmp("abcd", "abcz", 3), strdup("copied"));
    memset(buf, 'x', 5); buf[5] = 0;
    memmove(buf + 1, buf, 3);
    printf("mem: %s cmp %d chr %d\n", buf, memcmp("abc", "abd", 3) < 0, memchr("abc", 'c', 3) != 0);

    // ctype.h
    printf("ctype: %d%d%d%d%d %c%c\n", isalpha('a') != 0, isdigit('7') != 0, isspace(9) != 0, isupper('a') != 0, isxdigit('F') != 0, toupper('q'), tolower('Q'));

    // stdlib.h
    printf("strtol: %d %d %d %d", strtol("  -123xyz", &end, 10), strtol("0x1F", 0, 0), strtol("777", 0, 8), atoi("42"));
    printf(" rest \"%s\" ul %u\n", end, strtoul("4294967295", 0, 10));
    nums[0] = 5; nums[1] = 3; nums[2] = 9; nums[3] = 1; nums[4] = 7;
    qsort((char *)nums, 5, sizeof(int), (int)&by_value);
    printf("qsort: %d %d %d %d %d\n", nums[0], nums[1], nums[2], nums[3], nums[4]);
    names[0] = "pear"; names[1] = "apple"; names[2] = "fig"; names[3] = "banana";
    qsort((char *)names, 4, sizeof(int), (int)&by_name);
    printf("qsort: %s %s %s %s\n", names[0], names[1], names[2], names[3]);
    p = malloc(10);
    strcpy(p, "grow");
    p = realloc(p, 1000);
    strcat(p, "n");
    printf("malloc: %s calloc %d\n", p, ((int *)calloc(4, sizeof(int)))[3]);
    free(p);
    setenv("HOME", "/tmp", 1);
    printf("env: HOME=%s NONE=%s\n", getenv("HOME"), getenv("NONE") ? "set" : "unset");

    // files
    f = fopen("/tmp/libctest.txt", "w");
    fprintf(f, "line %d\n", 1);
    fputs("line 2\n", f);
    fclose(f);
    f = fopen("/tmp/libctest.txt", "a");
    fprintf(f, "line %d\n", 3);
    fclose(f);
    f = fopen("/tmp/libctest.txt", "r");
    n = 0;
    while (fgets(buf, 64, f)) { buf[strlen(buf) - 1] = 0; printf("file: %s\n", buf); ++n; }
    printf("file: %d lines, eof %d\n", n, feof(f));
    fclose(f);
    f = fopen("/tmp/no/such/file", "r");
    printf("errno: %s fopen %d: %s\n", f ? "opened?" : "null", errno == ENOENT, strerror(errno));

    // getopt
    verbose = 0; n = 0;
    while ((c = getopt(argc, argv, "vn:")) != -1) {
        if (c == 'v') verbose = 1;
        else if (c == 'n') n = atoi(optarg);
    }
    printf("getopt: verbose %d n %d rest", verbose, n);
    while (optind < argc) printf(" %s", argv[optind++]);
    printf("\n");

    // dirent
    d = opendir("/tmp");
    seen = 0;
    while ((e = readdir(d))) if (!strcmp(e->d_name, "libctest.txt")) seen = e->d_type == DT_REG;
    closedir(d);
    printf("dirent: libctest.txt %s\n", seen ? "found" : "missing");
    unlink("/tmp/libctest.txt");

    printf("libctest: done\n");
    return 0;
}
