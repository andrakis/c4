// fw.c - c4bb boot firmware.
//
// Provides in software what native c4m borrows from the host libc:
// malloc/free/realloc and the printf formatter. The CPU microcode
// reaches these routines by a synthesized JSR through the vector
// latches at 0x20 (see hw/microcode.uc): a syscall opcode's in-place
// stack arguments are indistinguishable from ordinary function
// arguments after a JSR, so these are plain functions and the trap
// machinery is never involved.
//
// Compiled with c4cc32 (no preprocessor: c4cc skips # lines). The
// formatter is adapted from src/c4lm/include/stdio.h's vsnprintf,
// emitting through putchar (the PUTC opcode) instead of a buffer.
//
// Heap bounds come from the HEAP_BASE/HEAP_END device registers,
// set by the loader after all images are placed.
//
// Blocks: [size][next] when free (address-ordered list, coalescing),
// [size] when allocated; user pointers sit one word after the header.

enum {
    VEC_MALC = 0x20, VEC_FREE = 0x24, VEC_RALC = 0x28,
    VEC_PRTF = 0x2c, VEC_STRC = 0x30,
    DEV_HEAP_BASE = 0x144, DEV_HEAP_END = 0x148,
    DEV_INTERVAL = 0x154
};
enum { FW_TMP_SZ = 80 };

int *__fw_free;          // head of the free list (address ordered)
int  __fw_heap_ready;
char *__fw_tmp;          // formatter scratch (static, not reentrant)

void __fw_heap_init () {
    int base, end, *blk;
    base = *(int *)DEV_HEAP_BASE;
    end  = *(int *)DEV_HEAP_END;
    if (base && end > base + 16) {
        blk = (int *)base;
        blk[0] = (end - base) / 8 * 8;   // block size includes header
        blk[1] = 0;
        __fw_free = blk;
    }
    __fw_heap_ready = 1;
}

// fw_malc/fw_free mutate __fw_free - global, shared by every task on
// this VM regardless of which kernel or protection mode it runs
// under - across many ordinary instructions, not atomically. C4KE
// never preempts, so it never noticed; C4IX does, aggressively
// (PREEMPT_INTERVAL=10000), and a hard IRQ landing mid-mutation,
// followed by a DIFFERENT task (or this one, rescheduled) calling
// malloc/free before the first call finishes, corrupts the free
// list - reproduced directly: an early allocation's contents got
// silently overwritten once ~30 more allocations followed under
// C4IX's preemption, never under C4KE's. The device register at
// DEV_INTERVAL is the same interrupt mask the trap microcode itself
// uses (hw/microcode.uc); writing it directly from firmware works
// identically, because the machine does not care whether a write to
// a memory-mapped register came from microcode or ordinary code.
// Save/restore (not set/clear) so nested calls - PRTF allocating
// __fw_tmp while already inside another malloc, say - stay correct:
// the inner call's restore leaves the outer call's mask in place.
int fw_malc (int size) {
    int *blk, *prev, *rest, need, saved, result;
    saved = *(int *)DEV_INTERVAL;
    *(int *)DEV_INTERVAL = 0;
    if (!__fw_heap_ready) __fw_heap_init();
    if (size <= 0) size = 1;
    need = (size + 4 + 7) / 8 * 8;       // header word + 8-byte rounding
    prev = 0; blk = __fw_free;
    result = 0;
    while (blk) {
        if (blk[0] >= need) {
            if (blk[0] - need >= 16) {   // split, keep the tail free
                rest = blk + need / 4;
                rest[0] = blk[0] - need;
                rest[1] = blk[1];
                blk[0] = need;
                if (prev) prev[1] = (int)rest; else __fw_free = rest;
            } else {
                if (prev) prev[1] = blk[1]; else __fw_free = (int *)blk[1];
            }
            result = (int)(blk + 1);
            blk = 0;
        } else {
            prev = blk; blk = (int *)blk[1];
        }
    }
    *(int *)DEV_INTERVAL = saved;
    return result;
}

void fw_free (int ptr) {
    int *blk, *cur, *prev, saved;
    if (!ptr) return;
    saved = *(int *)DEV_INTERVAL;
    *(int *)DEV_INTERVAL = 0;
    blk = (int *)ptr - 1;
    // insert address-ordered, coalesce with neighbours
    prev = 0; cur = __fw_free;
    while (cur && cur < blk) { prev = cur; cur = (int *)cur[1]; }
    blk[1] = (int)cur;
    if (prev) prev[1] = (int)blk; else __fw_free = blk;
    // merge forward
    if (cur && blk + blk[0] / 4 == cur) {
        blk[0] = blk[0] + cur[0];
        blk[1] = cur[1];
    }
    // merge backward
    if (prev && prev + prev[0] / 4 == blk) {
        prev[0] = prev[0] + blk[0];
        prev[1] = blk[1];
    }
    *(int *)DEV_INTERVAL = saved;
}

int fw_ralc (int ptr, int size) {
    int *old, n, i, nptr;
    char *s, *d;
    if (!ptr) return fw_malc(size);
    if (size <= 0) { fw_free(ptr); return 0; }
    old = (int *)ptr - 1;
    n = old[0] - 4;                      // old usable bytes
    if (n >= size) return ptr;           // still fits
    if (!(nptr = fw_malc(size))) return 0;
    s = (char *)ptr; d = (char *)nptr; i = 0;
    while (i < n) { d[i] = s[i]; ++i; }
    fw_free(ptr);
    return nptr;
}

int __fw_strlen (char *s) { char *t; t = s; while (*t) ++t; return t - s; }

// The formatting core: emits through putchar, returns the count.
// argp walks DOWNWARD: printf args were pushed left to right, so the
// stack holds them at descending addresses after the format string.
int __fw_vformat (char *f, int *argp) {
    char *s, *dig, *tmp, *prefix;
    int count;
    int c, v, base, uc, neg;
    int left, zero, plus, space, alt, run;
    int width, prec, haveprec;
    int i, j, len, pad, plen;
    int smask, d, q, r;

    if (!__fw_tmp) {
        if (!(__fw_tmp = (char *)fw_malc(FW_TMP_SZ))) return 0;
    }
    tmp = __fw_tmp;
    count = 0;
    smask = 2147483647;                  // 0x7fffffff

    while (*f) {
        if (*f != '%') { putchar(*f); ++count; ++f; }
        else {
            ++f;
            left = 0; zero = 0; plus = 0; space = 0; alt = 0;
            run = 1;
            while (run) {
                if      (*f == '-') { left  = 1; ++f; }
                else if (*f == '0') { zero  = 1; ++f; }
                else if (*f == '+') { plus  = 1; ++f; }
                else if (*f == ' ') { space = 1; ++f; }
                else if (*f == '#') { alt   = 1; ++f; }
                else run = 0;
            }
            width = 0;
            if (*f == '*') {
                width = *argp; --argp; ++f;
                if (width < 0) { left = 1; width = 0 - width; }
            }
            else while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); ++f; }
            haveprec = 0; prec = 0;
            if (*f == '.') {
                ++f; haveprec = 1;
                if (*f == '*') {
                    prec = *argp; --argp; ++f;
                    if (prec < 0) haveprec = 0;
                }
                else while (*f >= '0' && *f <= '9') { prec = prec * 10 + (*f - '0'); ++f; }
            }
            while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 't' || *f == 'j') ++f;
            c = *f;
            if (c) ++f;

            s = 0; len = 0; prefix = ""; plen = 0; base = 0;

            if (c == '%') { tmp[0] = '%'; s = tmp; len = 1; zero = 0; }
            else if (c == 'c') {
                tmp[0] = *argp; --argp; s = tmp; len = 1; zero = 0;
            }
            else if (c == 's') {
                s = (char *)*argp; --argp;
                if (!s) s = "(null)";
                len = __fw_strlen(s);
                if (haveprec && prec < len) len = prec;
                zero = 0;
            }
            else if (c == 'd' || c == 'i' || c == 'u' ||
                     c == 'x' || c == 'X' || c == 'o' || c == 'p') {
                v = *argp; --argp;
                uc = (c == 'X');
                neg = 0;
                if (c == 'd' || c == 'i') { base = 10; if (v < 0) neg = 1; }
                else if (c == 'u') base = 10;
                else if (c == 'o') base = 8;
                else base = 16;

                if (c == 'p') { prefix = "0x"; plen = 2; }
                else if (alt && base == 16 && v != 0) {
                    if (uc) prefix = "0X"; else prefix = "0x";
                    plen = 2;
                }
                else if (neg)   { prefix = "-"; plen = 1; }
                else if (plus)  { prefix = "+"; plen = 1; }
                else if (space) { prefix = " "; plen = 1; }

                if (uc) dig = "0123456789ABCDEF"; else dig = "0123456789abcdef";

                i = 0;
                if (v == 0) {
                    if (!(haveprec && prec == 0)) { tmp[i] = '0'; ++i; }
                }
                else if (base == 10 && c == 'u') {
                    while (v != 0) {
                        q = ((v >> 1) & smask) / 5;
                        r = v - q * 10;
                        if (r > 9) { q = q + 1; r = r - 10; }
                        tmp[i] = dig[r]; ++i;
                        v = q;
                    }
                }
                else if (base == 10) {
                    while (v != 0) {
                        d = v % 10; if (d < 0) d = 0 - d;
                        tmp[i] = dig[d]; ++i;
                        v = v / 10;
                    }
                }
                else if (base == 16) {
                    while (v != 0) { tmp[i] = dig[v & 15]; ++i; v = (v >> 4) & (smask >> 3); }
                }
                else {
                    while (v != 0) { tmp[i] = dig[v & 7]; ++i; v = (v >> 3) & (smask >> 2); }
                }
                while (haveprec && i < prec) { tmp[i] = '0'; ++i; }
                if (haveprec) zero = 0;

                len = i;
                i = 0; j = len - 1;
                while (i < j) {
                    d = tmp[i]; tmp[i] = tmp[j]; tmp[j] = d;
                    ++i; --j;
                }
                s = tmp;
            }

            if (c) {
                pad = width - len - plen;
                if (pad < 0) pad = 0;
                if (!left && !zero)
                    while (pad > 0) { putchar(' '); ++count; --pad; }
                i = 0;
                while (i < plen) { putchar(prefix[i]); ++count; ++i; }
                if (!left && zero)
                    while (pad > 0) { putchar('0'); ++count; --pad; }
                i = 0;
                while (i < len) { putchar(s[i]); ++count; ++i; }
                if (left)
                    while (pad > 0) { putchar(' '); ++count; --pad; }
            }
        }
    }
    return count;
}

// PRTF lands here via the vector JSR. The synthesized frame makes the
// caller's in-place args ordinary parameters: bp+1 holds the return
// pc, which points at the ADJ following PRTF, whose operand is the
// argument count (the same pc[1] trick c4m uses at c4m.c:1663).
int fw_prtf (int dummy) {
    int *ret, n;
    char *fmt;
    ret = (int *)*(&dummy - 1);          // return pc = address of ADJ
    n = ret[1];                          // ADJ operand: total args
    if (n > 7) {
        // c4m.c:1673 refuses more than 7 printf arguments
        __fw_vformat("Too many arguments to printf!\n", 0);
        exit(0 - 1);
    }
    fmt = (char *)*(&dummy + (n - 1));   // deepest push = format string
    return __fw_vformat(fmt, &dummy + (n - 2));
}

int fw_strc (int dummy) { return 0; }    // stacktraces need symbols (M2+)

void __attribute__((constructor)) fw_init (int c4r) {
    *(int *)VEC_MALC = (int)&fw_malc;
    *(int *)VEC_FREE = (int)&fw_free;
    *(int *)VEC_RALC = (int)&fw_ralc;
    *(int *)VEC_PRTF = (int)&fw_prtf;
    *(int *)VEC_STRC = (int)&fw_strc;
    __fw_heap_init();
}

int main (int argc, char **argv) { return 0; }   // never the entry image
