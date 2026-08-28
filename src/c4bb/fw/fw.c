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
// emitting a byte at a time to the UART register instead of into a
// buffer -- see __fw_putc.
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
    DEV_INTERVAL = 0x154,
    // Drives, for the BIOS at the bottom of this file.
    DEV_DRIVE = 0x13c, DEV_DCOUNT = 0x188, DEV_DRO = 0x18c,
    DEV_EJECT = 0x190, DEV_RESCAN = 0x194, DEV_TIME_MS = 0x10c,
    DEV_UART_TX = 0x100
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
// A failed allocation used to be silent: fw_malc returned 0, the caller
// said "out of memory", and nothing said whether the heap was full or
// merely broken. It is worth one line on the way out, because those two
// have nothing in common as bugs -- a heap eaten by a task that overran
// its stack reports megabytes free and a free list of nonsense.
void __fw_oom (int size) {
    int *blk, n, total, largest;
    n = 0; total = 0; largest = 0;
    blk = __fw_free;
    while (blk) {
        ++n; total = total + blk[0];
        if (blk[0] > largest) largest = blk[0];
        blk = (int *)blk[1];
    }
    printf("fw: malloc(%d) failed: %d free blocks, %d bytes, largest %d\n",
           size, n, total, largest);
}

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
    if (!result) __fw_oom(size);
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

// One byte to the serial line.
//
// NOT putchar: that is PUTC, opcode 39, which is c4m's and not c4's --
// and this file is the firmware, the thing that PROVIDES the syscalls
// to everything above it. It cannot need one. A store to the UART's
// transmit register is what the hardware actually does, and it is a
// plain SI. See src/c4bb/tools/opscan.mjs, which is the pin.
void __fw_putc (int c) { *(int *)DEV_UART_TX = c; }

int __fw_strlen (char *s) { char *t; t = s; while (*t) ++t; return t - s; }

// The formatting core: emits a byte at a time, returns the count.
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
        if (*f != '%') { __fw_putc(*f); ++count; ++f; }
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
                    while (pad > 0) { __fw_putc(' '); ++count; --pad; }
                i = 0;
                while (i < plen) { __fw_putc(prefix[i]); ++count; ++i; }
                if (!left && zero)
                    while (pad > 0) { __fw_putc('0'); ++count; --pad; }
                i = 0;
                while (i < len) { __fw_putc(s[i]); ++count; ++i; }
                if (left)
                    while (pad > 0) { __fw_putc(' '); ++count; --pad; }
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

//
// The BIOS.
//
// Everything above this line is what the CPU's microcode reaches for
// when a syscall opcode needs a subroutine. This part is the machine
// switching on: a banner, a look at how much memory is really there, a
// look at what is in the drives, and then it hands over. It runs only
// when the machine is started with no program -- `cli.js -d disk` with
// nothing after it -- because then the firmware IS the program.
//
// Why bother, when the host could just be told which image to run: on
// the breadboard, changing what boots means changing what is in a
// drive, and that is a thing a person does with their hands. Eject the
// C4DOS floppy, and the next thing round the loop boots what is in
// drive 1. See docs/c4bb-storage.md.
//
// A medium is bootable if it has boot.c4r, or a boot.cfg naming the
// image to load. The second is there so a disk can boot a kernel it
// already carries under its own name without a second 200 KB copy.
//
enum { BIOS_CHUNK = 65536 };

// Calling into a freshly loaded image, WITHOUT an indirect call.
//
// c4lc would happily emit JSRS for a variable holding an address, and
// the board has it -- but JSRS is c4m's, not c4's. The BIOS and C4DOS
// are the rungs a player reaches before they have extended the CPU, so
// nothing here may use an opcode above EXIT. (`./c4 c4l.c fw.c4r`
// refuses an image that does, which is the pin.)
//
// So: c4l.c's trick, verbatim in intent. A function locates its own
// entry by walking back from its caller's return address to the ENT
// that starts it, overwrites that ENT with JMP, and remembers where
// the operand went. Every later call to it is a jump to whatever was
// last written into that slot -- an indirect call built out of a
// direct one and a store.
enum { OP_JMP = 2, OP_ENT = 6, OP_ADJ = 7 };   // base c4 numbering
enum { BIOS_SEARCH = 512 };

int *__bios_slot;      // operand slot of the rewritten stub

// Return pc of our caller, found on the stack relative to a local.
int *__bios_caller (int dummy) {
    int *addr, *next, i;
    addr = (int *)(*(&addr + 2));
    i = 0;
    next = addr;
    while (++i <= BIOS_SEARCH) {
        --next;
        if (*addr == OP_ENT) {
            if (*next > OP_ADJ) return addr;
        }
        addr = next;
    }
    return 0;
}

// First call: locate self, become "JMP <slot>". Later calls: jump.
int __bios_stub () {
    int *self;
    if (!(self = __bios_caller(0))) { printf("bios: cannot arm the call stub\n"); exit(1); }
    *self = OP_JMP;
    __bios_slot = self + 1;
    return 0;
}

// The result is read back through a local so that -O cannot turn these
// into tail calls: a tail call replaces the frame the stub walks.
int __bios_call (int *f, int a) {
    int r;
    *__bios_slot = (int)f;
    r = __bios_stub(a);
    return r;
}
int __bios_call2 (int *f, int a, int b) {
    int r;
    *__bios_slot = (int)f;
    r = __bios_stub(a, b);
    return r;
}

// Half a second, measured off the machine's own millisecond counter --
// no opcode, and the wait is real time rather than a cycle count, so a
// person has a chance to put a disk in.
void __bios_sleep () {
    int t;
    t = *(int *)DEV_TIME_MS;
    while (*(int *)DEV_TIME_MS - t < 500) ;
}

char *__bios_buf;
int   __bios_len, __bios_cap;

int __bios_grow () {
    char *n; int i;
    if (!(n = (char *)fw_malc(__bios_cap + __bios_cap))) return 0;
    i = 0;
    while (i < __bios_len) { n[i] = __bios_buf[i]; ++i; }
    fw_free((int)__bios_buf);
    __bios_buf = n;
    __bios_cap = __bios_cap + __bios_cap;
    return 1;
}

// Read a whole file off the selected drive. Returns 0 if it is not
// there; the buffer is __bios_buf / __bios_len.
int __bios_slurp (char *name) {
    int fd, n;
    if ((fd = open(name, 0)) < 0) return 0;
    __bios_cap = BIOS_CHUNK + BIOS_CHUNK;
    if (!(__bios_buf = (char *)fw_malc(__bios_cap))) { close(fd); return 0; }
    __bios_len = 0;
    n = 1;
    while (n > 0) {
        if (__bios_len + BIOS_CHUNK > __bios_cap) {
            if (!__bios_grow()) { close(fd); return 0; }
        }
        if ((n = read(fd, __bios_buf + __bios_len, BIOS_CHUNK)) > 0)
            __bios_len = __bios_len + n;
    }
    close(fd);
    return 1;
}

int __bios_wordat (char *p) { return *(int *)p; }

// Load the image in __bios_buf and jump into it. Returns only if the
// image is unusable -- if it runs, whatever it does is the rest of the
// machine's life. A port of c4l.c's loader, which is the same job in
// plain c4; the differences are the word size and that both segments
// are copied out so nothing depends on where the read buffer landed.
int __bios_exec (char *name) {
    char *p, *data;
    int  *code, *cons, *des, **av;
    int   entry, codelen, datalen, memsz, patchlen, conslen, deslen;
    int   i, w, ptype, paddr, pvalu;

    p = __bios_buf;
    if (__bios_len < 13) { printf("bios: %s is too short to be an image\n", name); return 0; }
    if (!(p[0] == 'C' && p[1] == '4' && p[2] == 'R')) {
        printf("bios: %s is not a .c4r\n", name); return 0;
    }
    if (p[4] != 32) { printf("bios: %s is %d-bit; this machine is 32\n", name, p[4]); return 0; }
    // v3 records the data segment's in-memory size at byte 5; anything
    // past DATALEN is BSS and has to exist, and be zero, before the
    // image runs.
    memsz = p[3] >= 3 ? __bios_wordat(p + 5) : 0;

    w = sizeof(int);
    p = p + 13;
    entry    = __bios_wordat(p); p = p + w;
    codelen  = __bios_wordat(p); p = p + w;
    datalen  = __bios_wordat(p); p = p + w;
    patchlen = __bios_wordat(p); p = p + w;
    p = p + w;                                    // symbol count
    conslen  = __bios_wordat(p); p = p + w;
    deslen   = __bios_wordat(p); p = p + w;

    if (memsz < datalen) memsz = datalen;

    p = p + w;                                    // 'C' marker
    if (!(code = (int *)fw_malc(codelen * w + w))) { printf("bios: out of memory\n"); return 0; }
    i = 0;
    while (i < codelen) { code[i] = __bios_wordat(p + i * w); ++i; }
    p = p + codelen * w;

    p = p + w;                                    // 'D' marker
    if (!(data = (char *)fw_malc(memsz + w))) { printf("bios: out of memory\n"); return 0; }
    i = 0;
    while (i < memsz + w) { data[i] = 0; ++i; }
    i = 0;
    while (i < datalen) { data[i] = p[i]; ++i; }
    p = p + datalen;

    p = p + w;                                    // 'P' marker
    i = 0;
    while (i < patchlen) {
        ptype = __bios_wordat(p);
        paddr = __bios_wordat(p + w);
        pvalu = __bios_wordat(p + w + w);
        p = p + w + w + w;
        if      (ptype == 0 - 1) code[paddr] = (int)(code + pvalu);
        else if (ptype == 0 - 2) code[paddr] = (int)(data + pvalu);
        else if (ptype == 0 - 3) *(int *)(data + paddr) = (int)(code + pvalu);
        else if (ptype == 0 - 4) *(int *)(data + paddr) = (int)(data + pvalu);
        ++i;
    }

    p = p + w;                                    // 'c' marker
    cons = (int *)p; p = p + conslen * w;
    p = p + w;                                    // 'd' marker
    des = (int *)p;

    // The image's own name is its argv[0]: a kernel skips it when it
    // parses options, so it has to be there.
    if (!(av = (int **)fw_malc(w + w))) { printf("bios: out of memory\n"); return 0; }
    av[0] = (int *)name;
    av[1] = 0;

    printf("bios: booting %s\n", name);
    i = 0;
    while (i < conslen) { __bios_call(code + __bios_wordat((char *)(cons + i)), 0); ++i; }
    __bios_call2(code + entry, 1, (int)av);
    i = 0;
    while (i < deslen) { __bios_call(code + __bios_wordat((char *)(des + i)), 0); ++i; }
    return 1;
}

// Is there a medium in this drive worth booting? Leaves the drive
// selected and returns the name to load, or 0.
char *__bios_bootname (int d) {
    char *n;
    int   i;
    *(int *)DEV_DRIVE = d;
    if (__bios_slurp("boot.c4r")) return "boot.c4r";
    if (__bios_slurp("boot.cfg")) {
        // One line, the image's name. Trim at the first control char so
        // a file written by anything at all still reads.
        i = 0;
        while (i < __bios_len && __bios_buf[i] > 32) ++i;
        __bios_buf[i] = 0;
        if (__bios_buf[0]) {
            n = __bios_buf;
            if (!__bios_slurp(n)) { printf("bios: drive %d: boot.cfg names %s, which is not there\n", d, n); return 0; }
            return n;
        }
    }
    return 0;
}

// A real look at the memory, not just a report of what the loader was
// told: write a pattern near each end of the heap and read it back, so
// a machine configured with more memory than it has says so here
// rather than three minutes into a compile.
void __bios_ram () {
    int base, end, mb, *lo, *hi;
    base = *(int *)DEV_HEAP_BASE;
    end  = *(int *)DEV_HEAP_END;
    mb = (end - base) / 1048576;
    if (end <= base) { printf("bios: no usable memory\n"); return; }
    lo = (int *)base;
    hi = (int *)(end - 16);
    *lo = 1234567;  *hi = 7654321;
    if (*lo != 1234567 || *hi != 7654321) {
        printf("bios: RAM test FAILED between 0x%x and 0x%x\n", base, end);
        return;
    }
    *lo = 0; *hi = 0;
    printf("bios: %d MB RAM ok (0x%x-0x%x)\n", mb, base, end);
}

int main (int argc, char **argv) {
    char *name;
    int   d, drives, waited;

    // Arm the call stub before anything else needs it: the first call
    // is the one that rewrites it, and it must not be a real one.
    __bios_stub();

    printf("\n");
    printf("c4bb -- the breadboard computer\n");
    printf("firmware: malloc, free, realloc, printf, %d drives\n", *(int *)DEV_DCOUNT);
    __bios_ram();

    waited = 0;
    while (1) {
        drives = *(int *)DEV_DCOUNT;
        d = 0;
        while (d < drives) {
            if ((name = __bios_bootname(d))) {
                printf("bios: drive %d has %s\n", d, name);
                if (__bios_exec(name)) return 0;
            }
            ++d;
        }
        // Nothing to boot. Say so once, then keep looking: a disk put
        // in while we wait is the intended way to answer this.
        if (!waited) {
            d = 0;
            while (d < drives) { printf("bios: drive %d: no boot media\n", d); ++d; }
        }
        printf("bios: insert a bootable disk\n");
        __bios_sleep();
        d = 0;
        while (d < drives) { *(int *)DEV_RESCAN = d; ++d; }
        ++waited;
        if (waited > 600) { printf("bios: giving up\n"); return 1; }
    }
}

