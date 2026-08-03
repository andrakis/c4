//
// C4IX .c4r loader: read an image from the host filesystem, relocate
// it, and hand back an entry address -- the in-kernel port of c4l.c's
// parser. No invoke-stub gymnastics here: the kernel is c4lc code,
// so calling loaded code is an ordinary indirect call.
//
// task_spawn wraps it into the task machinery: constructors run at
// load time in the spawning task's context, the entry becomes a task
// through the normal shim, and the image segments are freed when the
// task is reaped. Destructors are parsed but NOT run at X1 --
// documented debt, they need an exit path that still has the image
// mapped.
//

#include "c4ix.h"

enum { C4R_BUF_MAX = 4194304 };   // 4MB image limit

static int loader_word(char *p) {
    return *(int *)p;
}

static int loader_nameis(char *p, int len, char *want) {
    int i;
    i = 0;
    while (i < len) {
        if (!want[i]) return 0;
        if (p[i] != want[i]) return 0;
        ++i;
    }
    return want[i] == 0;
}

// Walk the symbol section looking for __c4ix_systable, a global the
// image declares so the kernel can hand it the syscall entry point.
// That is how userland reaches the kernel on plain c4, which has no
// trap machinery: libc4ix calls through the slot instead of raising
// an interrupt. On c4m the slot is still filled but libc4ix prefers
// the real trap gateway, so protected mode does its job.
//
// Section layout per symbol (load-c4r.c): id, type, class, attrs as
// words, then a 1-byte name length, the name bytes, then the value.
static void loader_systable(char *p, int nsyms, int database) {
    int i, cls, namelen, value;

    i = 0;
    while (i < nsyms) {
        p = p + 8;                       // id
        p = p + 8;                       // type
        cls = loader_word(p); p = p + 8; // class
        p = p + 8;                       // attrs
        namelen = *p; p = p + 1;
        if (loader_nameis(p, namelen, "__c4ix_systable")) {
            p = p + namelen;
            value = loader_word(p);
            if (cls == 131)              // Glo: value is a data offset
                *(int *)(database + value) = (int)&sys_dispatch;
            return;
        }
        p = p + namelen;
        p = p + 8;                       // value
        ++i;
    }
}

// Load and relocate. Returns 1 and fills img on success.
int c4r_load(char *path, struct c4r_image *img) {
    char *buf, *p, *data;
    int *code, *cons;
    int fd, n, total;
    int entry, codelen, datalen, patchlen, symlen, conslen, deslen;
    int i, ptype, paddr, pvalu, e;

    if (!(buf = (char *)malloc(C4R_BUF_MAX))) return 0;
    if ((fd = open(path, 0)) < 0) {
        kprintf("c4ix: loader: cannot open %s\n", path);
        free(buf);
        return 0;
    }
    total = 0;
    while ((n = read(fd, buf + total, 65536)) > 0) total = total + n;
    close(fd);
    if (total < 13) { kprintf("c4ix: loader: %s is not a .c4r\n", path); free(buf); return 0; }

    p = buf;
    if (!(p[0] == 'C' && p[1] == '4' && p[2] == 'R')) {
        kprintf("c4ix: loader: bad signature in %s\n", path);
        free(buf);
        return 0;
    }
    if (p[4] / 8 != sizeof(int)) {
        kprintf("c4ix: loader: %d-bit image, host is %d-bit\n", p[4], sizeof(int) * 8);
        free(buf);
        return 0;
    }
    p = p + 13;   // signature, version, wordbits, padding

    entry    = loader_word(p); p = p + 8;
    codelen  = loader_word(p); p = p + 8;
    datalen  = loader_word(p); p = p + 8;
    patchlen = loader_word(p); p = p + 8;
    symlen   = loader_word(p); p = p + 8;
    conslen  = loader_word(p); p = p + 8;
    deslen   = loader_word(p); p = p + 8;

    // code: copy out of the read buffer into an exact-size allocation
    p = p + 8;   // 'C' marker word
    if (!(code = (int *)malloc(codelen * 8))) { free(buf); return 0; }
    memcpy(code, p, codelen * 8);
    p = p + codelen * 8;

    // data: fresh zero-padded allocation, word-aligned by malloc
    p = p + 8;   // 'D' marker
    if (!(data = (char *)malloc(datalen + 8))) { free(code); free(buf); return 0; }
    memset(data, 0, datalen + 8);
    memcpy(data, p, datalen);
    p = p + datalen;

    // patches: byte-offset address rewrites into the fresh segments
    p = p + 8;   // 'P' marker
    i = 0;
    while (i < patchlen) {
        ptype = loader_word(p); paddr = loader_word(p + 8); pvalu = loader_word(p + 16);
        p = p + 24;
        if (ptype == -1) code[paddr] = (int)(code + pvalu);
        else if (ptype == -2) code[paddr] = (int)(data + pvalu);
        else if (ptype == -3) *(int *)(data + paddr) = (int)(code + pvalu);
        else if (ptype == -4) *(int *)(data + paddr) = (int)(data + pvalu);
        // positive types are unresolved symbols: linker business
        ++i;
    }

    // constructor and destructor lists ('c' and 'd' markers), then
    // the symbol section ('S'). Symbols are read BEFORE constructors
    // run: libc4ix's constructor needs the systable already injected.
    p = p + 8;   // 'c' marker
    cons = (int *)p;
    p = p + conslen * 8;
    p = p + 8;   // 'd' marker
    p = p + deslen * 8;
    p = p + 8;   // 'S' marker
    // Only where traps do not exist: on c4m userland must go through
    // the real gateway so protected mode means something.
    if (host_type() == HOST_C4) loader_systable(p, symlen, (int)data);

    // constructors run now, in the loading task's context
    i = 0;
    while (i < conslen) {
        e = (int)(code + cons[i]);
        e();
        ++i;
    }
    // destructors are counted but not run (X1 debt): they need an
    // exit path that still has the image mapped.

    img->code = (int)code;
    img->data = (int)data;
    img->entry = (int)(code + entry);
    img->ncons = conslen;
    img->ndes = deslen;
    free(buf);
    return 1;
}

// Load an image and run it as a task. argv is passed through to the
// image's main untouched. Spawned programs are userland: on c4m they
// run behind the protected-mode boundary, on plain c4 (no such
// hardware) the privilege level is recorded but unenforced.
struct task *task_spawn_priv(char *path, int argc, int argv, int privs) {
    struct c4r_image img;
    struct task *t;

    if (!c4r_load(path, &img)) return 0;
    if (!(t = task_create(path, img.entry, argc, argv))) {
        free((int *)img.code);
        free((char *)img.data);
        return 0;
    }
    t->img_code = img.code;
    t->img_data = img.data;
    t->privs = (host_type() == HOST_C4M) ? privs : PRIV_KERNEL;
    return t;
}

struct task *task_spawn(char *path, int argc, int argv) {
    return task_spawn_priv(path, argc, argv, PRIV_USER);
}
