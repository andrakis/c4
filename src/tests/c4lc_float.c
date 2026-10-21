// float and double (docs/c4-float.md): c4lc's against gcc's, bit for
// bit. Everything is printed as integers or bit patterns, so the golden
// output (src/c4sp/tests/expected/c4lc-float.txt) is gcc's own; the
// battery runs at both word sizes, plain, -O and -minline.

unsigned int fb(float f) { return *(unsigned int *)&f; }
void pd(char *what, double d) {
    unsigned long long u;
    u = *(unsigned long long *)&d;
    printf("%s %x %x\n", what, (unsigned int)(u >> 32), (unsigned int)u);
}
void pf(char *what, float f) { printf("%s %x\n", what, fb(f)); }

// ---- globals: literals, negatives, integers made floats, aggregates ----
float g1 = 0.1f;
float g2 = -2.5f;
float g3 = 7;
double g4 = 0.1;
double g5 = -1e300;
double g6 = 12345;
float ga[5] = { 1, 2.5f, -0.25f, 1e-45f, 3.4028235e38f };
double gda[4] = { 0.5, -3, 1e-300, 4.9e-324 };
struct body { char tag; float m; double v[2]; int n; };
struct body gb = { 'b', 1.25f, { 2.5, -0.125 }, 42 };

float scale(float x, int k) { return x * k; }
double hyp2(double a, double b) { return a * a + b * b; }
float fneg(float x) { return -x; }
double dmix(char c, short s, int i, long long ll, float f) { return c + s + i + ll + f; }
int ftoi(float f) { return f; }
int score(double d, float f) { return d * 10 + f; }
unsigned int dtou(double d) { return d; }
double itod(int i) { return i; }
float avg(float *v, int n) {
    float s;
    int i;
    s = 0;
    for (i = 0; i < n; i++) s += v[i];
    return s / n;
}

int main() {
    float a, b, c, z, nan, inf;
    double d, e, f;
    float la[3] = { 1.5f, 2, -3.25f };
    double lda[2] = { 0.75, 9 };
    static float sf = 2.5f;
    float *pa;
    int (*fps)(double, float);
    int i, n;
    unsigned int u;
    long long ll;
    unsigned long long ull;
    char ch;
    short sh;
    unsigned char uc;

    // literals, rounded by the compiler
    pf("lit1", 0.1f); pd("lit2", 0.1); pf("lit3", 1e-45f); pf("lit4", 1.4e-45f);
    pd("lit5", 4.9e-324); pd("lit6", 2.2250738585072014e-308); pd("lit7", 1.7976931348623157e308);
    pf("lit8", 3.4028235e38f); pf("lit9", 1e39f); pf("lit10", 1e-50f); pf("lit11", 123456789.0f);
    pd("lit12", 0.3); pd("lit13", 2.5e-3); pd("lit14", .5); pd("lit15", 5.); pd("lit16", 1E+2);
    pd("lit17", 1e-2L); pf("lit18", 16777217.0f); pd("lit19", 9007199254740993.0);
    pd("lit20", 0.000000000000000000000000000000000000001); pd("lit21", 2.4703282292062327e-324);
    pf("lit22", 1.17549435e-38f); pf("lit23", 5.877472e-39f); pd("lit24", -0.0);

    // globals
    pf("g1", g1); pf("g2", g2); pf("g3", g3); pd("g4", g4); pd("g5", g5); pd("g6", g6);
    for (i = 0; i < 5; i++) pf("ga", ga[i]);
    for (i = 0; i < 4; i++) pd("gda", gda[i]);
    printf("gb %d %x %d\n", gb.tag, fb(gb.m), gb.n); pd("gbv0", gb.v[0]); pd("gbv1", gb.v[1]);

    // arithmetic
    a = 3.0f; b = 0.1f;
    pf("add", a + b); pf("sub", a - b); pf("mul", a * b); pf("div", a / b);
    d = 1.0 / 3.0; e = 2.0 / 3.0;
    pd("dadd", d + e); pd("dsub", d - e); pd("dmul", d * e); pd("ddiv", d / e);
    pd("mix1", a + d); pd("mix2", 1 + b); pd("mix3", b * 3); pd("mix4", 7 / 2.0); pd("mix5", 7 / 2);
    z = 0.0f;
    inf = 1.0f / z; nan = z / z;
    pf("inf", inf); pf("ninf", -1 / z); pf("nan", nan); pf("infsub", inf - inf);
    pd("dinf", 1e300 * 1e10); pd("dunder", 1e-300 * 1e-300); pd("dsubn", 1e-310 / 3);
    pf("fsubn", 1e-38f / 1000); pf("negz", -z); pd("dnegz", -(double)z);

    // comparisons, NaN and -0 among them
    printf("cmp1 %d %d %d %d %d %d\n", a < b, a > b, a <= 3, a >= 3.0, a == 3, a != b);
    printf("cmp2 %d %d %d %d %d %d\n", nan < 1, nan > 1, nan == nan, nan != nan, nan <= nan, nan >= 0);
    printf("cmp3 %d %d %d\n", z == -z, -0.0 < 0.0, inf > 1e38f);
    printf("cmp4 %d %d %d\n", 1 < 1.5, 2 > 1.5f, d < e);
    printf("cmp5 %d %d %d\n", (a > 2) + (b < 1), !nan, !z);
    if (nan) printf("nan is true\n");
    if (z) printf("bad\n"); else printf("zero is false\n");
    n = 0; d = 0.0; while (d < 1) { d = d + 0.25; n++; } printf("while %d\n", n);
    printf("andor %d %d %d\n", a && b, z || b, z && nan);

    // conversions
    printf("f2i %d %d %d %d\n", (int)2.9f, (int)-2.9f, (int)1e9f, ftoi(-7.5f));
    printf("d2i %d %d\n", (int)-1e9, (int)123456.789);
    u = (unsigned int)3e9; printf("d2u %u %u\n", u, dtou(4294967295.0));
    ll = (long long)-1e18; printf("d2ll %d %d\n", (int)(ll / 1000000000000ll), (int)(ll % 1000));
    ull = (unsigned long long)1.8e19; printf("d2ull %u %u\n", (unsigned int)(ull >> 32), (unsigned int)ull);
    ch = (char)65.7; sh = (short)-1234.5; uc = (unsigned char)200.9f;
    printf("narrow %d %d %d\n", ch, sh, uc);
    pf("i2f", (float)16777217); pf("i2f2", (float)-2147483647); pd("i2d", (double)-2147483647);
    pf("u2f", (float)4294967295u); pd("u2d", (double)4294967295u);
    pd("ll2d", (double)-9007199254740993ll); pf("ll2f", (float)0x7FFFFFFFFFFFFFFFll);
    pd("ull2d", (double)18446744073709551615ull); pf("ull2f", (float)18446744073709551615ull);
    pd("itod", itod(-5)); pd("c2d", (double)'A'); pd("s2f", (float)(short)-3);
    pd("f2d", (double)0.1f); pf("d2f", (float)0.1); pf("d2f2", (float)1e300); pf("d2f3", (float)1e-50);
    pf("d2f4", (float)4.9e-324); pd("f2d2", (double)1e-45f);
    pd("dmix", dmix(-1, 300, 2000000000, 5000000000ll, 0.5f));

    // assignment, compound ops, ++ and --
    c = 1.0f; c += 2; c *= 3.0f; c -= 0.5; c /= 2; pf("cops", c);
    pf("pre", ++c); pf("post", c++); pf("after", c); pf("predec", --c); pf("postdec", c--); pf("end", c);
    d = 5; d /= 2; d--; pd("dops", d); d++; pd("dinc", d);
    i = 10; i += 2.75; printf("iadd %d\n", i);
    i = 7; i *= 1.5f; printf("imul %d\n", i);
    n = a; printf("assignint %d\n", n);

    // arrays, pointers, structs, locals
    for (i = 0; i < 3; i++) pf("la", la[i]);
    pd("lda0", lda[0]); pd("lda1", lda[1]);
    pa = la; pa++; pf("ptr", *pa); pf("ptr2", pa[1]);
    *pa = 8; pf("ptrset", la[1]);
    gb.m = gb.m * 2; gb.v[1] -= 1; pf("gbm", gb.m); pd("gbv", gb.v[1]);
    pf("avg", avg(la, 3));
    sf = sf + 1; pf("static", sf);

    // calls: arguments converted, returns converted, through pointers
    pf("scale", scale(1.5f, 3)); pf("scale2", scale(2, 2.9f));
    pd("hyp", hyp2(3, 4.0f)); pf("fneg", fneg(2.5f));
    // through a pointer: the arguments go as written (it returns a word)
    fps = score; printf("fps %d %d\n", fps(2.25, 0.5f), fps(-1.0, 3.0f));

    // ?: with mixed arms
    pd("cond1", a > 2 ? 1.5 : 2); pd("cond2", a < 2 ? 1.5 : 2); pf("cond3", b < 1 ? b : 0);

    // float -> double -> float survives; a double does not, as a float
    b = 0.1f; d = b; c = d; printf("roundtrip %d\n", b == c);
    d = 0.1; c = d; printf("narrowed %d\n", c == d);
    printf("sizeof %d %d\n", (int)sizeof(double), (int)sizeof(1.5));
    return 0;
}
