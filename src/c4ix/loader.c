//
// C4IX .c4r loader: read an image from the RAM filesystem or the host,
// relocate it, and hand back an entry address -- the in-kernel port of c4l.c's
// parser. No invoke-stub gymnastics here: the kernel is c4lc code,
// so calling loaded code is an ordinary indirect call.
//
// task_spawn wraps it into the task machinery: the entry becomes a
// task through the normal shim, and the image segments are freed when
// the task is reaped. Constructors and destructors are resolved here
// but RUN BY THE TASK, in task_shim -- this function executes in the
// spawning task's context, and a constructor that asks who it is must
// not be told the parent.
//

#include "c4ix.h"

enum { C4R_BUF_MAX = 4194304 };   // 4MB image limit

// Set while a caller is TRYING candidate paths and a miss is expected
// -- a bare C4KE program name may match a real file that is not an
// image at all. Complaints about those are noise, not diagnosis.
int loader_quiet;

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
    int *code, *cons, *des;
    int fd, n, total;
    int entry, codelen, datalen, patchlen, symlen, conslen, deslen, memsz;
    int i, ptype, paddr, pvalu;
    struct vnode *vn;

    if (!(buf = (char *)malloc(C4R_BUF_MAX))) return 0;
    if ((fd = open(path, 0)) >= 0) {
        total = 0;
        while ((n = read(fd, buf + total, 65536)) > 0) total = total + n;
        close(fd);
    } else {
        vn = vfs_lookup(path);
        if (!vn || vn->type != VN_RAMFILE) {
            // Silent: callers try several candidate names (the shell
            // resolves "wc" against three), so a miss is routine and
            // the caller is the one that knows when to complain.
            free(buf);
            return 0;
        }
        // The host had no such file, so try the RAM filesystem -- and
        // this is not a convenience. A program the machine produced
        // ITSELF cannot be on the host: the C4 VM has no write syscall,
        // so nothing running under it can put a file there. What a
        // compiler running as a task CAN do is write to standard output
        // and let the shell redirect that into a RAM file, so
        // `c4th.c4r ... > /ram/prog.c4r` and then `/ram/prog.c4r` is
        // the whole compile-and-run loop, and this is what joins the
        // two halves. Without it the kernel can only ever run programs
        // that were built somewhere else.
        //
        // Second, not first, so that loading a host image costs exactly
        // what it always did. The order is invisible otherwise: a RAM
        // path and a host path cannot name the same file.
        total = vn->size;
        if (total > C4R_BUF_MAX) {
            kprintf("c4ix: loader: %s is %d bytes, over the %d limit\n",
                    path, total, C4R_BUF_MAX);
            free(buf);
            return 0;
        }
        i = 0;
        while (i < total) { buf[i] = vn->data[i]; ++i; }
    }
    if (total < 13) {
        if (!loader_quiet) kprintf("c4ix: loader: %s is not a .c4r\n", path);
        free(buf); return 0;
    }

    p = buf;
    if (!(p[0] == 'C' && p[1] == '4' && p[2] == 'R')) {
        if (!loader_quiet) kprintf("c4ix: loader: bad signature in %s\n", path);
        free(buf);
        return 0;
    }
    if (p[4] / 8 != sizeof(int)) {
        kprintf("c4ix: loader: %d-bit image, host is %d-bit\n", p[4], sizeof(int) * 8);
        free(buf);
        return 0;
    }
    // padding word (byte 5) = data MEMSZ in v3 (total in-memory size,
    // excess over datalen is zero-filled BSS); v2 has none.
    memsz = (p[3] >= 3) ? loader_word(p + 5) : 0;
    p = p + 13;   // signature, version, wordbits, padding

    entry    = loader_word(p); p = p + sizeof(int);
    codelen  = loader_word(p); p = p + sizeof(int);
    datalen  = loader_word(p); p = p + sizeof(int);
    patchlen = loader_word(p); p = p + sizeof(int);
    symlen   = loader_word(p); p = p + sizeof(int);
    conslen  = loader_word(p); p = p + sizeof(int);
    deslen   = loader_word(p); p = p + sizeof(int);
    if (memsz < datalen) memsz = datalen;

    // code: copy out of the read buffer into an exact-size allocation
    p = p + sizeof(int);   // 'C' marker word
    if (!(code = (int *)malloc(codelen * sizeof(int)))) { free(buf); return 0; }
    memcpy(code, p, codelen * sizeof(int));
    p = p + codelen * sizeof(int);

    // data: allocate the full in-memory size (memsz), zero-padded by
    // malloc+memset so the BSS tail [datalen, memsz) is ready; only
    // datalen bytes are copied from the image.
    p = p + sizeof(int);   // 'D' marker
    if (!(data = (char *)malloc(memsz + 8))) { free(code); free(buf); return 0; }
    memset(data, 0, memsz + 8);
    memcpy(data, p, datalen);
    p = p + datalen;

    // patches: byte-offset address rewrites into the fresh segments
    p = p + sizeof(int);   // 'P' marker
    i = 0;
    while (i < patchlen) {
        ptype = loader_word(p); paddr = loader_word(p + sizeof(int)); pvalu = loader_word(p + 2 * sizeof(int));
        p = p + 3 * sizeof(int);
        if (ptype == -1) code[paddr] = (int)(code + pvalu);
        else if (ptype == -2) code[paddr] = (int)(data + pvalu);
        else if (ptype == -3) *(int *)(data + paddr) = (int)(code + pvalu);
        else if (ptype == -4) *(int *)(data + paddr) = (int)(data + pvalu);
        // positive types are unresolved symbols: linker business
        ++i;
    }

    // Constructor and destructor lists ('c' and 'd' markers), then
    // the symbol section ('S').
    //
    // Neither list is RUN here. They are copied out and resolved to
    // absolute addresses so the new task can run them itself, because
    // this function executes in the SPAWNING task's context -- the
    // new task does not exist yet. A constructor that asks who it is
    // would be told the parent. That is fatal for C4KE programs: u0's
    // constructor caches its pid and its parent's, and installs eight
    // signal handlers, so every one of them would be wired to the
    // wrong task.
    //
    // The lists point INTO buf, which is freed below, so copying is
    // not an optimisation -- it is the only way to defer them.
    p = p + sizeof(int);   // 'c' marker
    cons = (int *)p;
    p = p + conslen * sizeof(int);
    des = (int *)p + 1;   // past the 'd' marker
    p = p + sizeof(int);   // 'd' marker
    p = p + deslen * sizeof(int);
    p = p + sizeof(int);   // 'S' marker
    // Only where traps do not exist: on c4m userland must go through
    // the real gateway so protected mode means something.
    if (host_type() == HOST_C4) loader_systable(p, symlen, (int)data);

    img->cons = 0;
    img->des = 0;
    if (conslen) {
        if (!(img->cons = (int)malloc(conslen * sizeof(int)))) {
            free((int *)code); free((char *)data); free(buf);
            return 0;
        }
        i = 0;
        while (i < conslen) { ((int *)img->cons)[i] = (int)(code + cons[i]); ++i; }
    }
    if (deslen) {
        if (!(img->des = (int)malloc(deslen * sizeof(int)))) {
            if (img->cons) free((int *)img->cons);
            free((int *)code); free((char *)data); free(buf);
            return 0;
        }
        i = 0;
        while (i < deslen) { ((int *)img->des)[i] = (int)(code + des[i]); ++i; }
    }

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
        if (img.cons) free((int *)img.cons);
        if (img.des) free((int *)img.des);
        return 0;
    }
    t->img_code = img.code;
    t->img_data = img.data;
    t->img_cons = img.cons;
    t->img_ncons = img.ncons;
    t->img_des = img.des;
    t->img_ndes = img.ndes;
    t->privs = (host_type() == HOST_C4M) ? privs : PRIV_KERNEL;
    return t;
}

struct task *task_spawn(char *path, int argc, int argv) {
    return task_spawn_priv(path, argc, argv, PRIV_USER);
}
