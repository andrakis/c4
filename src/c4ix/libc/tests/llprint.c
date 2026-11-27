// llprint.c: printf's long long conversions on a 32-bit machine, which
// get it as two words (test-libc-bare, against gcc -m32's output)
#include <stdio.h>

int main() {
    unsigned long long u = 0x1122334455667788ull;
    unsigned long long big = 18446744073709551615ull;
    long long n = -5;
    long long lo = -9223372036854775807ll - 1;
    long long hi = 9223372036854775807ll;
    unsigned long long ms = 1234567;
    char buf[64];
    printf("llu [%llu] [%llu] [%llu] [%llu]\n", u, big, ms, (unsigned long long)0);
    printf("lld [%lld] [%lld] [%lld] [%lld] [%lld]\n", n, lo, hi, (long long)4294967296ull, (long long)-4294967296ll);
    printf("llx [%llx] [%llX] [%#llx] [%llo] [%#llo]\n", u, big, u, u, (unsigned long long)8);
    printf("width [%20llu] [%-20lld|] [%020lld] [%+lld] [% lld] [%.12llu]\n", ms, n, n, hi, ms, ms);
    printf("after [%llu] %d [%lld] %s %x\n", u, 42, n, "ok", 0xbeef);
    printf("t=%llums frame %ld\n", ms, 7l);
    snprintf(buf, sizeof buf, "%lld:%llu", lo, big);
    printf("snprintf [%s]\n", buf);
    printf("long [%lu] [%ld] [%lx] [%hd] [%zu]\n", 4000000000ul, -4l, 255ul, (short)-3, (unsigned)5);
    return 0;
}
