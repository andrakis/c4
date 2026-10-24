// savers.c -- the screen savers. docs/c4ix-desktop.md.
//
// After the desktop has had no input for a while (Settings: which saver,
// and how many minutes), one of these takes the whole display until the
// next key or mouse movement. The first four follow the user's
// RetroSavers (D3D ports of the classics) closely, in integer
// arithmetic, drawn with the display's line, rectangle and text
// commands rather than shaders:
//
//   Starfield     stars stream out of the centre, nearer ones bigger and brighter
//   Polygons      two four-cornered shapes bounce about, each leaving a
//                 trail of its last few outlines in cycling colours
//   Beziers       a closed loop of cubic curves, its control points bouncing,
//                 a fading trail in cycling colours
//   Marquee       a line of text crawls across at a random height
//   Digital Rain  columns of falling characters, bright at the head, fading behind
//
// wm.c runs them: saver_start, then saver_frame every SAVER_MS, then
// saver_stop on input (and a full redraw of the desktop).

#include "desktop.h"

enum { N_SAVERS = 6 };       // 0 is None

static char *saver_names[N_SAVERS];
static int saver_cur;        // the one running, or 0
static int saver_t;          // frames since it started
static int rnd_s;

static int rnd() {
    rnd_s = rnd_s * 1103515245 + 12345;
    return (rnd_s >> 8) & 0x7fffff;
}
static int rnd_range(int lo, int hi) { return lo + rnd() % (hi - lo + 1); }

// hue 0..1535, full saturation and value
static int hue_rgb(int h, int v) {
    int r, g, b, f;
    h = h % 1536;
    if (h < 0) h = h + 1536;
    f = h % 256;
    if (h < 256)       { r = 255; g = f; b = 0; }
    else if (h < 512)  { r = 255 - f; g = 255; b = 0; }
    else if (h < 768)  { r = 0; g = 255; b = f; }
    else if (h < 1024) { r = 0; g = 255 - f; b = 255; }
    else if (h < 1280) { r = f; g = 0; b = 255; }
    else               { r = 255; g = 0; b = 255 - f; }
    r = r * v / 255; g = g * v / 255; b = b * v / 255;
    return (r << 16) | (g << 8) | b;
}

static void names() {
    saver_names[0] = "(None)";
    saver_names[1] = "Starfield";
    saver_names[2] = "Polygons";
    saver_names[3] = "Beziers";
    saver_names[4] = "Marquee";
    saver_names[5] = "Digital Rain";
}
int saver_count() { return N_SAVERS; }
char *saver_name(int i) { names(); return i >= 0 && i < N_SAVERS ? saver_names[i] : ""; }
int saver_active() { return saver_cur; }

// ---- Starfield ----
// x, y in -1024..1024, z 1024 (far) to 20 (here), as the original's
// -1..1 and 1.0..0.02; both axes scale by half the width.
enum { N_STARS = 120 };
static int st_x[N_STARS], st_y[N_STARS], st_z[N_STARS];
static void star_spawn(int i, int anywhere) {
    st_x[i] = rnd_range(0 - 1024, 1024);
    st_y[i] = rnd_range(0 - 1024, 1024);
    st_z[i] = anywhere ? rnd_range(52, 1024) : 1024;
}
static void star_start() { int i; i = 0; while (i < N_STARS) { star_spawn(i, 1); ++i; } }
static void star_frame() {
    int i, sx, sy, d, sz, br;
    d_rect(0, 0, SW, SH, 0);
    i = 0;
    while (i < N_STARS) {
        st_z[i] = st_z[i] - 12;                       // speed 5: across the field in ~2.5 s at 30 fps
        if (st_z[i] <= 20) { star_spawn(i, 0); ++i; continue; }
        sx = SW / 2 + st_x[i] * (SW / 2) / st_z[i];
        sy = SH / 2 + st_y[i] * (SW / 2) / st_z[i];
        if (sx < 0 - 4 || sy < 0 - 4 || sx > SW + 4 || sy > SH + 4) { star_spawn(i, 0); ++i; continue; }
        d = 1024 - st_z[i];                           // 0 far, 1024 near
        sz = 1 + 3 * d / 1024 * d / 1024;
        br = 90 + 165 * d / 1024;
        d_rect(sx, sy, sz, sz, (br << 16) | (br << 8) | br);
        ++i;
    }
}

// ---- Polygons ----
// Velocities authored for 640x480, as the original's, scaled to the height.
enum { P_V = 4, P_TRAIL = 7 };
static int pg_x[2 * P_V], pg_y[2 * P_V], pg_vx[2 * P_V], pg_vy[2 * P_V];
static int pg_tx[2 * P_TRAIL * P_V], pg_ty[2 * P_TRAIL * P_V], pg_tc[2 * P_TRAIL];
static int pg_n, pg_hue[2], pg_step[2];
static int bounce(int *p, int *v, int max) {
    *p = *p + *v;
    if (*p < 0) { *p = 0 - *p; if (*v < 0) *v = 0 - *v; }
    if (*p > max) { *p = 2 * max - *p; if (*v > 0) *v = 0 - *v; }
    return *p;
}
static void poly_start() {
    int i, sp;
    i = 0;
    while (i < 2 * P_V) {
        pg_x[i] = rnd_range(0, SW - 1) * 16;
        pg_y[i] = rnd_range(0, SH - 1) * 16;
        sp = rnd_range(48, 144) * SH / 480;           // 3..9 pixels, in sixteenths
        pg_vx[i] = (rnd() & 1 ? sp : 0 - sp) * rnd_range(60, 100) / 100;
        pg_vy[i] = (rnd() & 1 ? sp : 0 - sp) * rnd_range(60, 100) / 100;
        ++i;
    }
    pg_n = 0;
    pg_hue[0] = rnd_range(0, 1535); pg_hue[1] = rnd_range(0, 1535);
    pg_step[0] = rnd_range(6, 12); pg_step[1] = rnd_range(6, 12);
}
static void poly_frame() {
    int s, v, k, t, a, b, slot;
    // one tick: move, then keep this outline in the trail
    slot = pg_n % P_TRAIL;
    s = 0;
    while (s < 2) {
        v = 0;
        while (v < P_V) {
            k = s * P_V + v;
            bounce(&pg_x[k], &pg_vx[k], (SW - 1) * 16);
            bounce(&pg_y[k], &pg_vy[k], (SH - 1) * 16);
            pg_tx[(s * P_TRAIL + slot) * P_V + v] = pg_x[k] / 16;
            pg_ty[(s * P_TRAIL + slot) * P_V + v] = pg_y[k] / 16;
            ++v;
        }
        pg_hue[s] = pg_hue[s] + pg_step[s];
        pg_tc[s * P_TRAIL + slot] = hue_rgb(pg_hue[s], 255);
        ++s;
    }
    ++pg_n;
    d_rect(0, 0, SW, SH, 0);
    s = 0;
    while (s < 2) {
        t = 0;
        while (t < P_TRAIL && t < pg_n) {
            k = (s * P_TRAIL + t) * P_V;
            v = 0;
            while (v < P_V) {
                a = k + v; b = k + (v + 1) % P_V;
                d_line(pg_tx[a], pg_ty[a], pg_tx[b], pg_ty[b], pg_tc[s * P_TRAIL + t]);
                ++v;
            }
            ++t;
        }
        ++s;
    }
}

// ---- Beziers ----
// N curves, each anchor, handle, handle, then the next one's anchor: a
// closed loop. Points in sixteenths; the curve in BZ_SUB steps (t in 64ths).
enum { BZ_N = 3, BZ_SUB = 12, BZ_TRAIL = 14, BZ_PTS = BZ_N * BZ_SUB };
static int bz_x[BZ_N * 3], bz_y[BZ_N * 3], bz_vx[BZ_N * 3], bz_vy[BZ_N * 3];
static int bz_px[BZ_TRAIL * BZ_PTS], bz_py[BZ_TRAIL * BZ_PTS], bz_c[BZ_TRAIL];
static int bz_n, bz_hue;
static void bez_start() {
    int i, sp;
    i = 0;
    while (i < BZ_N * 3) {
        bz_x[i] = rnd_range(0, SW - 1) * 16;
        bz_y[i] = rnd_range(0, SH - 1) * 16;
        sp = rnd_range(32, 112) * SH / 480;
        bz_vx[i] = (rnd() & 1 ? sp : 0 - sp) * rnd_range(50, 100) / 100;
        bz_vy[i] = (rnd() & 1 ? sp : 0 - sp) * rnd_range(50, 100) / 100;
        ++i;
    }
    bz_n = 0;
    bz_hue = rnd_range(0, 1535);
}
static void bez_frame() {
    int i, k, t, it, b0, b1, b2, b3, p0, p1, p2, p3, slot, base, a, c, age, r, g, bl, cnt;
    i = 0;
    while (i < BZ_N * 3) { bounce(&bz_x[i], &bz_vx[i], (SW - 1) * 16); bounce(&bz_y[i], &bz_vy[i], (SH - 1) * 16); ++i; }
    slot = bz_n % BZ_TRAIL;
    base = slot * BZ_PTS;
    i = 0;
    while (i < BZ_N) {
        p0 = i * 3; p1 = p0 + 1; p2 = p0 + 2; p3 = ((i + 1) % BZ_N) * 3;
        k = 0;
        while (k < BZ_SUB) {
            t = k * 64 / BZ_SUB; it = 64 - t;
            // Bernstein weights in 64^3ths
            b0 = it * it * it; b1 = 3 * t * it * it; b2 = 3 * t * t * it; b3 = t * t * t;
            bz_px[base + i * BZ_SUB + k] = (b0 / 16 * (bz_x[p0] / 16) + b1 / 16 * (bz_x[p1] / 16) + b2 / 16 * (bz_x[p2] / 16) + b3 / 16 * (bz_x[p3] / 16)) / 16384;
            bz_py[base + i * BZ_SUB + k] = (b0 / 16 * (bz_y[p0] / 16) + b1 / 16 * (bz_y[p1] / 16) + b2 / 16 * (bz_y[p2] / 16) + b3 / 16 * (bz_y[p3] / 16)) / 16384;
            ++k;
        }
        ++i;
    }
    bz_hue = bz_hue + 12;
    bz_c[slot] = hue_rgb(bz_hue, 255);
    ++bz_n;
    d_rect(0, 0, SW, SH, 0);
    cnt = bz_n < BZ_TRAIL ? bz_n : BZ_TRAIL;
    age = 0;
    while (age < cnt) {
        // oldest first, dimmed: the head of the trail reads as the current curve
        slot = (bz_n - cnt + age) % BZ_TRAIL;
        k = cnt > 1 ? 45 + 55 * age / (cnt - 1) : 100;
        c = bz_c[slot];
        r = ((c >> 16) & 255) * k / 100; g = ((c >> 8) & 255) * k / 100; bl = (c & 255) * k / 100;
        c = (r << 16) | (g << 8) | bl;
        base = slot * BZ_PTS;
        i = 0;
        while (i < BZ_PTS) {
            a = base + i;
            k = base + (i + 1) % BZ_PTS;
            d_line(bz_px[a], bz_py[a], bz_px[k], bz_py[k], c);
            ++i;
        }
        ++age;
    }
}

// ---- Marquee ----
static int mq_x, mq_y, mq_hue;
static char *mq_text;
static void mq_place() { mq_x = SW; mq_y = rnd_range(40, SH - 100); }
static void mq_start() { mq_text = "C4IX -- built upon Robert Swierczek's C4"; mq_place(); mq_hue = rnd_range(0, 1535); }
static void mq_frame() {
    d_rect(0, 0, SW, SH, 0);
    d_textn(mq_x, mq_y, hue_rgb(mq_hue, 255), 48, F_BOLD, 0, mq_text, 0 - 1);
    mq_x = mq_x - 6;
    mq_hue = mq_hue + 4;
    // about 27 pixels a character at 48 points: gone off the left, round again
    if (mq_x < 0 - ui_len(mq_text) * 27) mq_place();
}

// ---- Digital Rain ----
// A column every CW pixels; each has a head falling a row a frame (or two),
// drawn bright, the cell above it green, and the cell its tail has left
// cleared -- so a frame draws three cells a column, not the screen.
enum { CW = 14, CH = 16, R_COLS = SW / CW, R_ROWS = SH / CH };
static int rn_head[R_COLS], rn_len[R_COLS], rn_wait[R_COLS], rn_speed[R_COLS];
static char rn_ch[2];
static char *rn_glyphs;
static void rain_start() {
    int c;
    rn_glyphs = "0123456789ABCDEFabcdef<>=+*#$%&@!?:;{}[]|/\\~^C4IX";
    d_rect(0, 0, SW, SH, 0);
    c = 0;
    while (c < R_COLS) {
        rn_head[c] = 0 - rnd_range(0, R_ROWS);
        rn_len[c] = rnd_range(6, 22);
        rn_speed[c] = rnd_range(1, 2);
        rn_wait[c] = 0;
        ++c;
    }
    rn_ch[1] = 0;
}
static void rain_cell(int c, int r, int rgb) {
    if (r < 0 || r >= R_ROWS) return;
    d_rect(c * CW, r * CH, CW, CH, 0);
    if (rgb < 0) return;
    rn_ch[0] = rn_glyphs[rnd() % ui_len(rn_glyphs)];
    d_textn(c * CW + 2, r * CH + 1, rgb, 13, F_MONO, 0, rn_ch, 1);
}
static void rain_frame() {
    int c;
    c = 0;
    while (c < R_COLS) {
        if (++rn_wait[c] >= rn_speed[c]) {
            rn_wait[c] = 0;
            rain_cell(c, rn_head[c] - 1, 0x00c040);           // last frame's head, green now
            rain_cell(c, rn_head[c] - rn_len[c], 0 - 1);       // the tail's end, cleared
            rain_cell(c, rn_head[c] - rn_len[c] / 2, 0x006020); // half way, dimmer
            rain_cell(c, rn_head[c], 0xd0ffd0);                // the head
            ++rn_head[c];
            if (rn_head[c] - rn_len[c] > R_ROWS) {
                rn_head[c] = 0 - rnd_range(0, R_ROWS / 2);
                rn_len[c] = rnd_range(6, 22);
                rn_speed[c] = rnd_range(1, 2);
            }
        }
        ++c;
    }
}

// ---- running them ----

void saver_start(int which) {
    if (which <= 0 || which >= N_SAVERS) return;
    if (!rnd_s) rnd_s = __time() | 1;
    saver_cur = which;
    saver_t = 0;
    d_rect(0, 0, SW, SH, 0);
    if (which == 1) star_start();
    else if (which == 2) poly_start();
    else if (which == 3) bez_start();
    else if (which == 4) mq_start();
    else rain_start();
}

void saver_frame() {
    if (saver_cur == 1) star_frame();
    else if (saver_cur == 2) poly_frame();
    else if (saver_cur == 3) bez_frame();
    else if (saver_cur == 4) mq_frame();
    else if (saver_cur == 5) rain_frame();
    ++saver_t;
}

int saver_frames() { return saver_t; }

void saver_stop() { saver_cur = 0; }
