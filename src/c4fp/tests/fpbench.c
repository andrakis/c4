// fpbench.c -- floating point, timed (F6, docs/c4-float.md): the same
// image on a machine with the FPU opcode and without it (c4m -F, libjs
// --no-fpu), so the helpers run in hardware or in software.
//
//   mandel   single precision: the Mandelbrot set, 160 x 120, 64 deep
//   whet     double: a Whetstone-like mix of + - * / and sqrt
//   nbody    double: five bodies under gravity, 2000 steps
//
// Each prints a checksum (which must not depend on the path), the VM's
// instruction count (__c4_cycles) and the machine's milliseconds
// (__time). Compiled by c4lc:
//   ./c4sp src/c4sp/lisp/c4lc.lisp -O src/c4fp/tests/fpbench.c fpbench.c4r

int mandel() {
    int px, py, it, total;
    float x0, y0, x, y, xx, yy;
    total = 0;
    py = 0;
    while (py < 120) {
        y0 = (float)py * 0.025f - 1.5f;
        px = 0;
        while (px < 160) {
            x0 = (float)px * 0.025f - 2.25f;
            x = 0.0f; y = 0.0f;
            it = 0;
            xx = 0.0f; yy = 0.0f;
            while (it < 64 && xx + yy < 4.0f) {
                y = 2.0f * x * y + y0;
                x = xx - yy + x0;
                xx = x * x;
                yy = y * y;
                ++it;
            }
            total = total + it;
            ++px;
        }
        ++py;
    }
    return total;
}

// sqrt by the helper the compiler has, so as not to need libc
double sqrt_d(double v) {
    unsigned long long b;
    b = __c4d_sqrt(*(unsigned long long *)&v);
    return *(double *)&b;
}

double whet() {
    double x1, x2, x3, x4, t, acc;
    int i;
    // Whetstone's module 1: four values mixed and damped, so they stay
    // finite; and a sqrt and a division a round
    x1 = 1.0; x2 = -1.0; x3 = -1.0; x4 = -1.0; t = 0.499975; acc = 0.0;
    i = 0;
    while (i < 20000) {
        x1 = (x1 + x2 + x3 - x4) * t;
        x2 = (x1 + x2 - x3 + x4) * t;
        x3 = (x1 - x2 + x3 + x4) * t;
        x4 = (0.0 - x1 + x2 + x3 + x4) * t;
        acc = acc + sqrt_d(x1 * x1 + x2 * x2 + 1.0) / (1.0 + x3 * x3 + x4 * x4);
        ++i;
    }
    return acc;
}

double bx[5], by[5], bz[5], vx[5], vy[5], vz[5], bm[5];

double energy() {
    double e, dx, dy, dz;
    int i, j;
    e = 0.0;
    i = 0;
    while (i < 5) {
        e = e + 0.5 * bm[i] * (vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i]);
        j = i + 1;
        while (j < 5) {
            dx = bx[i] - bx[j]; dy = by[i] - by[j]; dz = bz[i] - bz[j];
            e = e - bm[i] * bm[j] / sqrt_d(dx * dx + dy * dy + dz * dz);
            ++j;
        }
        ++i;
    }
    return e;
}

double nbody() {
    int i, j, step;
    double dx, dy, dz, d2, mag, dt;
    // a sun and four planets, in made-up units
    bx[0] = 0.0; by[0] = 0.0; bz[0] = 0.0; vx[0] = 0.0; vy[0] = 0.0; vz[0] = 0.0; bm[0] = 39.47;
    i = 1;
    while (i < 5) {
        bx[i] = (double)(i * 3); by[i] = (double)i * 0.5; bz[i] = 0.1 * (double)i;
        vx[i] = 0.0; vy[i] = 3.0 / (double)i; vz[i] = 0.01;
        bm[i] = 0.001 * (double)i;
        ++i;
    }
    dt = 0.001;
    step = 0;
    while (step < 2000) {
        i = 0;
        while (i < 5) {
            j = i + 1;
            while (j < 5) {
                dx = bx[i] - bx[j]; dy = by[i] - by[j]; dz = bz[i] - bz[j];
                d2 = dx * dx + dy * dy + dz * dz;
                mag = dt / (d2 * sqrt_d(d2));
                vx[i] = vx[i] - dx * bm[j] * mag; vy[i] = vy[i] - dy * bm[j] * mag; vz[i] = vz[i] - dz * bm[j] * mag;
                vx[j] = vx[j] + dx * bm[i] * mag; vy[j] = vy[j] + dy * bm[i] * mag; vz[j] = vz[j] + dz * bm[i] * mag;
                ++j;
            }
            ++i;
        }
        i = 0;
        while (i < 5) { bx[i] = bx[i] + dt * vx[i]; by[i] = by[i] + dt * vy[i]; bz[i] = bz[i] + dt * vz[i]; ++i; }
        ++step;
    }
    return energy();
}

// a double's bits, for a checksum that does not depend on printf
int hi(double d) { return (int)(*(unsigned long long *)&d >> 32); }
int lo(double d) { return (int)*(unsigned long long *)&d; }

int main() {
    unsigned int c0;
    int t0, m;
    double d;
    printf("fpbench: FPU %s\n", (__c4_info() & 0x10000) ? "yes" : "no");
    c0 = __c4_cycles(); t0 = __time();
    m = mandel();
    printf("mandel: %d iterations, %u instructions, %d ms\n", m, __c4_cycles() - c0, __time() - t0);
    c0 = __c4_cycles(); t0 = __time();
    d = whet();
    printf("whet: %x %x, %u instructions, %d ms\n", hi(d), lo(d), __c4_cycles() - c0, __time() - t0);
    c0 = __c4_cycles(); t0 = __time();
    d = nbody();
    printf("nbody: %x %x, %u instructions, %d ms\n", hi(d), lo(d), __c4_cycles() - c0, __time() - t0);
    return 0;
}
