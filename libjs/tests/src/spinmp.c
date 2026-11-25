// spinmp.c -- every CPU does the same work: the time should not grow with
// the CPUs on threads, and does in the round robin
enum { C4I_SMP = 0x100, WORK = 20000000 };
int done, sums[16];
int work() {
    int i, s;
    s = 0;
    for (i = 0; i < WORK; i++) s = s + (i & 7);
    return s;
}
void cpu() {
    sums[__c4_cpu_id()] = work();
    __c4_fadd((int)&done, 1);
    __c4_wake((int)&done, 0);
}
int main() {
    int n, k, d, *stack;
    n = (__c4_info() & C4I_SMP) ? __c4_cpu_count() : 1;
    k = 1;
    while (k < n) {
        stack = (int *)malloc(16384);
        __c4_cpu_start(k, (int)cpu, (int)stack + 16384);
        k = k + 1;
    }
    sums[0] = work();
    __c4_fadd((int)&done, 1);
    while ((d = done) < n) __c4_wait((int)&done, d);
    k = 0;
    while (k < n) { if (sums[k] != sums[0]) printf("spinmp: cpu %d differs\n", k); k = k + 1; }
    printf("spinmp: %d cpus each summed %d\n", n, sums[0]);
    return 0;
}
