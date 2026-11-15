// accel.c -- the drawing accelerators (libjs/accel.js) against the loops
// they stand for, each loop as the game has it (Duke's draw.c, Quake's
// d_scan.c and r_surf.c): the same pixels, the same values left behind, on
// made-up textures and steps. test-libjs runs it with the accelerators and
// without (cli.js --no-accel), where it says so and draws nothing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "accel.h"

typedef union { unsigned int i; float f; } f2i;

unsigned seed = 12345;
int rnd(int n) { seed = seed * 1103515245 + 12345; return (int)((seed >> 8) % (unsigned)n); }

unsigned char *tex, *pal, *bufc, *bufa;
int fails, cases;
enum { TEXN = 65536, PALN = 65536 + 512, BUFN = 320 * 240 };

void fill(unsigned char *p, int n, int holes) {
    int i;
    for (i = 0; i < n; i++) { p[i] = rnd(256); if (holes && rnd(8) == 0) p[i] = 255; }
}
void check(char *what, int k, int same) {
    cases++;
    if (memcmp(bufc, bufa, BUFN) != 0 || !same) {
        fails++;
        if (fails < 10) printf("accel: %s, case %d, differs\n", what, k);
    }
}
void clear() { memset(bufc, 7, BUFN); memset(bufa, 7, BUFN); }

// Duke's vlineasm1 and mvlineasm1, a column
unsigned colc(int n, unsigned v, unsigned inc, int sh, unsigned char *t, unsigned char *p, unsigned char *d, int bpl, int masked) {
    int c;
    for (; n > 0; n--) {
        c = t[v >> sh];
        if (!masked || c != 255) *d = p[c];
        v += inc; d += bpl;
    }
    return v;
}
void test_tcol() {
    int k, n, sh, masked, x;
    unsigned v, inc, va, vc;
    for (k = 0; k < 200; k++) {
        masked = k & 1;
        clear();
        n = rnd(240); sh = 16 + rnd(16); v = (unsigned)rnd(1 << 30) * 4 + rnd(4); inc = (unsigned)rnd(1 << 30) * 2;
        x = rnd(320);
        vc = colc(n, v, inc, sh, tex, pal, bufc + x, 320, masked);
        if (masked) va = accel_tcol_masked(n, v, inc, sh, tex, pal, bufa + x, 320);
        else va = accel_tcol(n, v, inc, sh, tex, pal, bufa + x, 320);
        check(masked ? "tcol_masked" : "tcol", k, va == vc);
    }
}

// Duke's hlineasm4: a row right to left
void rowc(int n, unsigned a, unsigned b, unsigned da, unsigned db, int sh1, int bits, int sh2, unsigned char *t, unsigned char *p, unsigned char *d) {
    for (; n > 0; n--) { *d = p[t[((a >> sh1) << bits) | (b >> sh2)]]; d--; a -= da; b -= db; }
}
void test_hline() {
    int k, n, bits, sh1, sh2, x;
    unsigned a, b, da, db;
    for (k = 0; k < 200; k++) {
        clear();
        bits = 4 + rnd(5); sh1 = 32 - (16 - bits) + rnd(2); sh2 = (32 - bits) & 31;
        if (sh1 > 31) sh1 = 31;
        a = (unsigned)rnd(1 << 30) * 4 + rnd(4); b = (unsigned)rnd(1 << 30) * 4 + rnd(4);
        da = (unsigned)rnd(1 << 28); db = (unsigned)rnd(1 << 28);
        n = rnd(320); x = 320 * 100 + 319;
        rowc(n, a, b, da, db, sh1, bits, sh2, tex, pal, bufc + x);
        accel_hline(n, a, b, da, db, sh1, bits, sh2, tex, pal, bufa + x);
        check("hline", k, 1);
    }
}

// Duke's rhlineasm4, its x and y with their sign bits flipped
void rrowc(int n, unsigned char *t, unsigned char *p, unsigned char *d, int c, int s, int x, int y, int da, int db, int ax, int bx) {
    for (; n > 0; n--) {
        *d = p[*t]; t -= c;
        if (y < bx) t--; if (x < ax) t -= s; x -= da; y -= db; d--;
    }
}
void test_rhline() {
    int k, n, c, s, x, y, da, db, ax, bx, at;
    for (k = 0; k < 200; k++) {
        clear();
        n = rnd(320); c = rnd(64); s = rnd(64);
        x = rnd(1 << 30) * 4; y = rnd(1 << 30) * 4; da = rnd(1 << 30); db = rnd(1 << 30);
        ax = rnd(1 << 30) * 4; bx = rnd(1 << 30) * 4;
        at = 320 * 50 + 319;
        rrowc(n, tex + TEXN - 1, pal, bufc + at, c, s, x, y, da, db, ax, bx);
        accel_rhline(n, tex + TEXN - 1, pal, bufa + at, c, s, x, y, da, db, ax, bx);
        check("rhline", k, 1);
    }
}

// Duke's slopevlin from its first eight on, as draw.c has it
int recip[2048];
int *pals;
int fpu_c, asm4_c, fpu_a, asm4_a;
void slopec(unsigned char *dest, unsigned z, int *ps, int left, unsigned esi, unsigned edi, int sh2, int sh1, int mask, int bpl,
            unsigned char *t, float a, float step, int gx3, int gy3) {
    f2i c;
    int u, sgn, k, n, bx, vy, pix;
    unsigned r, xs, yhi, ystep;
    unsigned char *p;
    do {
        c.f = a;
        u = (int)c.i;
        fpu_c = u;
        sgn = u >> 31;
        k = (((u >> 23) & 255) - 2) & 31;
        r = ((unsigned)recip[(u >> 12) & 0x7ff] >> k) ^ (unsigned)sgn;
        xs = (unsigned)gx3 * (r - z);
        ystep = (unsigned)gy3 * (r - z);
        z = r;
        a += step;
        asm4_c = left;
        n = left >= 8 ? 8 : (left & 0xff);
        xs &= ~0xffu;
        yhi = ystep & ~0xffu;
        while (n) {
            bx = ((int)esi >> sh2) & mask;
            esi += xs | (unsigned)n;
            vy = (int)(edi >> sh1);
            edi += ystep;
            p = (unsigned char *)*ps;
            ps--;
            pix = p[(vy & ~0xff) | t[bx + vy]];
            ystep = yhi | (unsigned)pix;
            dest += bpl;
            *dest = (unsigned char)pix;
            n--;
        }
        left = asm4_c - 8;
    } while (left > 0);
}
void test_slope() {
    int k, i, left, gx3, gy3, sh1, sh2, mask;
    unsigned z, esi, edi;
    float a, step;
    f2i fa, fs;
    int d;
    for (i = 0; i < 2048; i++) recip[i] = rnd(1 << 30) * 2 + rnd(2);
    for (i = 0; i < 256; i++) pals[i] = (int)(pal + rnd(256) * 256);
    for (k = 0; k < 200; k++) {
        clear();
        left = 1 + rnd(200); sh1 = 24 + rnd(8); sh2 = 20 + rnd(12); mask = (1 << (4 + rnd(8))) - 1;
        z = (unsigned)rnd(1 << 30) * 4; esi = (unsigned)rnd(1 << 30) * 4; edi = (unsigned)rnd(1 << 30) * 4;
        gx3 = rnd(1 << 30) - (1 << 29); gy3 = rnd(1 << 30) - (1 << 29);
        a = (float)(rnd(100000) + 1) * 0.37f; step = (float)(rnd(20000) - 10000) * 0.011f;
        if (k & 1) a = -a;
        d = 320 * 230 + rnd(320);
        slopec(bufc + d, z, pals + 255, left, esi, edi, sh2, sh1, mask, -320, tex, a, step, gx3, gy3);
        fa.f = a; fs.f = step;
        accel_slope(bufa + d, z, pals + 255, left, esi, edi, sh2, sh1, mask, -320, tex, fa.i, fs.i, gx3, gy3, recip, &fpu_a, &asm4_a);
        check("slope", k, fpu_a == fpu_c && asm4_a == asm4_c);
    }
}

// Quake's D_DrawSpans8 run, its D_DrawZSpans and R_DrawSurfaceBlock8
void spanc(int n, int s, int t, int ss, int ts, unsigned char *pb, int cw, unsigned char *d) {
    for (; n > 0; n--) { *d++ = *(pb + (s >> 16) + (t >> 16) * cw); s += ss; t += ts; }
}
void test_span() {
    int k, n, s, t, ss, ts, x;
    for (k = 0; k < 200; k++) {
        clear();
        n = 1 + rnd(16); s = (70 + rnd(115)) << 16 | rnd(65536); t = (70 + rnd(115)) << 16 | rnd(65536);
        ss = rnd(8 << 16) - (4 << 16); ts = rnd(8 << 16) - (4 << 16); x = rnd(320 * 200);
        spanc(n, s, t, ss, ts, tex, 256, bufc + x);
        accel_span(n, s, t, ss, ts, tex, 256, bufa + x);
        check("span", k, 1);
    }
}
void zspanc(int n, int izi, int step, short *d) {
    for (; n > 0; n--) { *d++ = (short)(izi >> 16); izi += step; }
}
void test_zspan() {
    int k, n, izi, step, x;
    for (k = 0; k < 200; k++) {
        clear();
        n = rnd(320); izi = rnd(1 << 30) * 4 + rnd(4); step = rnd(1 << 30) - (1 << 29); x = 2 * rnd(320 * 100);
        zspanc(n, izi, step, (short *)(bufc + x));
        accel_zspan(n, izi, step, bufa + x);
        check("zspan", k, 1);
    }
}
void blockc(int rows, int sh, int ll, int lr, int lls, int lrs, unsigned char *src, int ss, unsigned char *cm, unsigned char *d, int ds) {
    int i, b, step, light;
    for (i = 0; i < rows; i++) {
        step = (ll - lr) >> sh; light = lr;
        for (b = rows - 1; b >= 0; b--) { d[b] = cm[(light & 0xFF00) + src[b]]; light += step; }
        src += ss; lr += lrs; ll += lls; d += ds;
    }
}
void test_lblock() {
    int k, mip, rows, ll, lr, lls, lrs, x, at;
    for (k = 0; k < 200; k++) {
        clear();
        mip = rnd(4); rows = 16 >> mip;
        ll = rnd(0xc000) + 0x1000; lr = rnd(0xc000) + 0x1000; lls = rnd(512) - 256; lrs = rnd(512) - 256;
        x = rnd(320 * 200); at = rnd(1024);
        blockc(rows, 4 - mip, ll, lr, lls, lrs, tex + at, 64, pal, bufc + x, 256);
        accel_lblock(rows, 4 - mip, ll, lr, lls, lrs, tex + at, 64, pal, bufa + x, 256);
        check("lblock", k, 1);
    }
}

void sspanc(int n, int s, int t, int ss, int ts, unsigned char *pb, int cw, unsigned char *d, short *pz, int *izi, int step) {
    int c;
    for (; n > 0; n--) {
        c = *(pb + (s >> 16) + (t >> 16) * cw);
        if (c != 255 && *pz <= (*izi >> 16)) { *pz = *izi >> 16; *d = c; }
        *izi += step; d++; pz++; s += ss; t += ts;
    }
}
void test_sspan() {
    int k, n, s, t, ss, ts, x, z, izi, izc, iza, i;
    for (k = 0; k < 200; k++) {
        clear();
        // a z buffer of shorts after the pixels, the same in both
        for (i = 0; i < 640; i++) { z = rnd(65536) - 32768; ((short *)(bufc + 64000))[i] = z; ((short *)(bufa + 64000))[i] = z; }
        n = 1 + rnd(16); s = (70 + rnd(115)) << 16 | rnd(65536); t = (70 + rnd(115)) << 16 | rnd(65536);
        ss = rnd(8 << 16) - (4 << 16); ts = rnd(8 << 16) - (4 << 16); x = rnd(32000);
        izi = rnd(1 << 30) * 4 + rnd(4); izc = izi;
        z = 2 * rnd(600);
        i = rnd(1 << 22) - (1 << 21);
        sspanc(n, s, t, ss, ts, tex, 256, bufc + x, (short *)(bufc + 64000 + z), &izc, i);
        iza = accel_sspan(n, s, t, ss, ts, tex, 256, bufa + x, bufa + 64000 + z, izi, i);
        check("sspan", k, iza == izc);
    }
}

int main() {
    if (!accel_present()) { printf("accel: not fitted\n"); return 0; }
    tex = malloc(TEXN); pal = malloc(PALN); bufc = malloc(BUFN); bufa = malloc(BUFN);
    pals = malloc(256 * sizeof(int));
    fill(tex, TEXN, 1); fill(pal, PALN, 0);
    test_tcol(); test_hline(); test_rhline(); test_slope();
    test_span(); test_zspan(); test_lblock(); test_sspan();
    if (fails) printf("accel: %d of %d cases differ\n", fails, cases);
    else printf("accel: %d cases, each as its loop draws it\n", cases);
    return fails != 0;
}
