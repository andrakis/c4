// C4IX userland: free -- where the memory is.
//
// /proc/meminfo, made when it is read (vfs.c), is the kernel's count in
// KB, a "name value" line each. The heap is all of it: the kernel's own
// blocks, RAM files, the cache and, under the MMU, the blocks programs'
// pages are in. What free says of it:
//
//   - its size, what is in use and what is free;
//   - the biggest block that could be had at once. One allocation bigger
//     than that fails however much is free, when what is free is in
//     pieces: a program asking for a big array then says it is out of
//     memory with megabytes free;
//   - RAM files written and not saved, which only persist save (sync, or
//     the desktop's save each minute) gives back: saved, they are read
//     from drive 1;
//   - files kept from the drive as a cache, which memory takes back when
//     it is short;
//   - under the MMU, the blocks programs' pages are in, what of them is
//     in use, and how much is in blocks less than an eighth in use: a
//     block goes back to the heap only with its last page, so a few
//     pages left in a big block -- one a program that has exited had to
//     itself -- keep all of it;
//   - and swap.
//
//   free         in KB
//   free -m      in MB

#include "c4ix_user.h"

enum { BUF = 1024, NV = 32 };

static char buf[BUF];
static char *names[NV];
static int vals[NV];
static int nv;
static int mb;

// /proc/meminfo's value for NAME, or -1 where it has none
static int get(char *name) {
    int i;
    i = 0;
    while (i < nv) { if (!ustrcmp(names[i], name)) return vals[i]; ++i; }
    return 0 - 1;
}

// K kilobytes, right-aligned in ten columns, "  12345 K" or "   12.3 M"
// (libc4ix's printf takes no widths); "-" for none
static void amt(int k) {
    char s[16];
    int n, v;
    n = 15;
    s[n] = 0;
    if (k < 0) s[--n] = '-';
    else {
        s[--n] = mb ? 'M' : 'K';
        s[--n] = ' ';
        v = k;
        if (mb) { s[--n] = '0' + k % 1024 * 10 / 1024; s[--n] = '.'; v = k / 1024; }
        s[--n] = '0' + v % 10;
        v = v / 10;
        while (v) { s[--n] = '0' + v % 10; v = v / 10; }
    }
    while (n > 5) s[--n] = ' ';
    write(STDOUT, s + n, 15 - n);
}

int main(int argc, char **argv) {
    int fd, n, got, i, heap, used, fr, big, files, cached, blocks, inuse, sparse, swap, sfree, rest;
    char *p;

    mb = argc > 1 && !ustrcmp(argv[1], "-m");
    if (argc > 2 || (argc > 1 && !mb)) {
        ufprintf(STDERR, "usage: free [-m]\n");
        return 2;
    }
    if ((fd = uopen("/proc/meminfo", O_RD)) < 0) {
        ufprintf(STDERR, "free: cannot open /proc/meminfo\n");
        return 1;
    }
    got = 0;
    while (got < BUF - 1 && (n = uread(fd, buf + got, BUF - 1 - got)) > 0) got = got + n;
    uclose(fd);
    buf[got] = 0;
    // "name value" lines
    p = buf;
    nv = 0;
    while (*p && nv < NV) {
        names[nv] = p;
        while (*p && *p != ' ') ++p;
        if (!*p) break;
        *p++ = 0;
        vals[nv] = 0;
        while (*p >= '0' && *p <= '9') { vals[nv] = vals[nv] * 10 + *p - '0'; ++p; }
        while (*p && *p != '\n') ++p;
        if (*p) ++p;
        ++nv;
    }

    heap = get("heap");
    used = get("used");
    fr = get("free");
    big = get("largest");
    files = get("files");
    cached = get("cached");
    blocks = get("blocks");
    inuse = get("inuse");
    sparse = get("sparse");
    swap = get("swap");
    sfree = get("swapfree");

    uprintf("             total      used      free   largest\n");
    uprintf("memory: ");
    amt(heap); amt(used); amt(fr); amt(big);
    uprintf("\n");
    if (files >= 0) {
        uprintf("  files:");
        amt(files);
        uprintf("  in %d RAM files, written and not saved (sync saves them)\n", get("nfiles"));
    }
    if (cached >= 0) {
        uprintf("  cache:");
        amt(cached);
        uprintf("  in %d files read from the drive, given back when short\n", get("ncached"));
    }
    if (blocks >= 0) {
        uprintf("  pages:");
        amt(blocks);
        uprintf("  programs' memory, in %d blocks:", get("nblocks"));
        amt(inuse);
        uprintf(" of pages in use\n");
    }
    if (used >= 0) {
        rest = used - (files > 0 ? files : 0) - (cached > 0 ? cached : 0) - (blocks > 0 ? blocks : 0);
        uprintf("  other:");
        amt(rest < 0 ? 0 : rest);
        uprintf("  the kernel's, and programs' without the MMU\n");
    }
    if (swap >= 0) {
        uprintf("swap:   ");
        amt(swap); amt(swap - sfree); amt(sfree);
        uprintf("    %d pages out, %d in\n", get("swapouts"), get("swapins"));
    }
    // blocks a few pages keep: a block goes back with its last page
    if (sparse >= 1024) {
        uprintf("Of programs' memory,");
        amt(sparse);
        uprintf(" is in blocks less than an eighth in use, kept by the pages in them.\n");
    }
    // free memory in pieces: say how small they are
    if (fr > 0 && big >= 0 && big < fr / 2) {
        uprintf("The free memory is in pieces: nothing bigger than");
        amt(big);
        uprintf(" can be had at once.\n");
    }
    return 0;
}
