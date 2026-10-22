// smpwin.c -- the CPU-local window (libjs's, docs/c4ix-smp.md 3.4): two
// processors write the same addresses at 0x800 and each reads back its own
enum { C4I_SMP = 0x100, LOCAL = 0x800 };
int seen1, done1;
void second() {
    int *w;
    w = (int *)LOCAL;
    w[0] = 111; w[63] = 222;
    // CPU 0 writes the same words meanwhile; this CPU must still see its own
    while (!done1) { if (w[0] != 111) seen1 = -1; __c4_fadd((int)&done1, 0); if (seen1 == 0 && w[63] == 222) { seen1 = w[0]; done1 = 1; } }
}
int main() {
    int *w, *stack;
    w = (int *)LOCAL;
    if (!(__c4_info() & C4I_SMP)) { printf("smpwin: no SMP\n"); return 1; }
    printf("smpwin: %d cpus\n", __c4_cpu_count());
    w[0] = 7; w[63] = 9;
    stack = (int *)malloc(4096);
    __c4_cpu_start(1, (int)second, (int)stack + 4096);
    while (!done1) w[0] = w[0] + 1;
    printf("smpwin: cpu 1 saw %d, cpu 0 sees %d and %d\n", seen1, w[0] > 7, w[63]);
    return 0;
}
