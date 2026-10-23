// scantest.c -- libc's sscanf, fseek, ftell and fread, against gcc's: make
// test-libc-bare runs it on the bare machine (libc's bare.c, whose files are
// read whole into memory) and compares what it prints with gcc's build.
// Its own source is the file it seeks in.
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    int a, b, c, n, r;
    unsigned u;
    short h;
    char s1[32], s2[32], ch[4];
    float f;
    double d;
    FILE *fp;
    char buf[64];

    r = sscanf("  42 -17 0x1F", "%d %i %x", &a, &b, &c);
    printf("1: %d %d %d %d\n", r, a, b, c);
    r = sscanf("0x1f 017 -0", "%i %i %i", &a, &b, &c);
    printf("2: %d %d %d %d\n", r, a, b, c);
    r = sscanf("hello world", "%s %s", s1, s2);
    printf("3: %d %s %s\n", r, s1, s2);
    r = sscanf("abc123def", "%[a-z]%d%n", s1, &a, &n);
    printf("4: %d %s %d %d\n", r, s1, a, n);
    r = sscanf("key = 99;", "%[^ =] = %d", s1, &a);
    printf("5: %d %s %d\n", r, s1, a);
    r = sscanf("12345", "%3d%d", &a, &b);
    printf("6: %d %d %d\n", r, a, b);
    r = sscanf("x:7", "x:%*d%n", &n);
    printf("7: %d %d\n", r, n);
    r = sscanf("", "%d", &a);
    printf("8: %d\n", r);
    r = sscanf("abc", "%d", &a);
    printf("9: %d\n", r);
    r = sscanf("3.5 -2.25e1 4294967295 -7", "%f %lf %u %hd", &f, &d, &u, &h);
    printf("10: %d %d %d %u %d\n", r, (int) (f * 100), (int) (d * 100), u, h);
    r = sscanf("ab", "%c%c", &ch[0], &ch[1]);
    printf("11: %d %c%c\n", r, ch[0], ch[1]);
    r = sscanf("100%", "%d%%", &a);
    printf("12: %d %d\n", r, a);

    fp = fopen(argc > 1 ? argv[1] : "scantest.c", "r");
    if (!fp) { printf("no file\n"); return 1; }
    fseek(fp, 0, SEEK_END);
    a = (int) ftell(fp);
    fseek(fp, 3, SEEK_SET);
    n = (int) fread(buf, 1, 10, fp);
    buf[n] = 0;
    printf("13: %d %ld [%s]\n", a > 100, ftell(fp), buf);
    fseek(fp, -5, SEEK_CUR);
    n = (int) fread(buf, 1, 5, fp);
    buf[n] = 0;
    printf("14: %ld [%s]\n", ftell(fp), buf);
    c = fgetc(fp);
    fseek(fp, -1, SEEK_CUR);
    b = fgetc(fp);
    printf("15: %d %d %ld\n", c, b, ftell(fp));
    rewind(fp);
    n = (int) fread(buf, 1, 2, fp);
    buf[n] = 0;
    printf("16: [%s] %ld\n", buf, ftell(fp));
    fclose(fp);
    return 0;
}
