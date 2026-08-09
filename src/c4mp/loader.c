//
// c4mp .c4r loader: read an image, relocate it, hand back an entry
// address. A close port of src/c4ix/loader.c, which is the compact
// modern reader (load-c4r.c is the same job in 1154 lines, written
// against a much older format revision).
//
// The difference from C4IX is what happens next. C4IX resolves the
// constructor and destructor lists and hands them to a task shim,
// because it is a kernel and the shim is guest code. c4mp is the
// machine, so it cannot call guest code at all -- main.c builds a
// bootstrap trampoline out of real instructions instead, and these
// lists become JSR targets in it.
//

#include "c4mp.h"

enum { C4R_BUF_MAX = 4194304 };   // 4MB image limit

static int loader_word(char *p) {
    return *(int *)p;
}

// Load and relocate. Returns 1 and fills img on success.
//
// Every segment is copied out of the read buffer into its own exact
// allocation, because the buffer is freed before returning: the
// image's code and data become ordinary host memory that the
// interpreter runs directly out of.
int c4r_load(char *path, struct c4r_image *img) {
    char *buf, *p, *data;
    int *code, *cons, *des;
    int fd, n, total, i;
    int entry, codelen, datalen, patchlen, symlen, conslen, deslen, memsz;
    int ptype, paddr, pvalu;

    img->code = 0; img->data = 0; img->entry = 0;
    img->cons = 0; img->ncons = 0; img->des = 0; img->ndes = 0;

    if (!(buf = (char *)malloc(C4R_BUF_MAX))) {
        printf("c4mp: out of memory reading %s\n", path);
        return 0;
    }
    if ((fd = open(path, 0)) < 0) {
        printf("c4mp: cannot open %s\n", path);
        free(buf);
        return 0;
    }
    total = 0;
    while ((n = read(fd, buf + total, 65536)) > 0) total = total + n;
    close(fd);
    if (total < 13) {
        printf("c4mp: %s is too short to be a .c4r\n", path);
        free(buf);
        return 0;
    }

    p = buf;
    if (!(p[0] == 'C' && p[1] == '4' && p[2] == 'R')) {
        printf("c4mp: bad signature in %s\n", path);
        free(buf);
        return 0;
    }
    // The word size is baked into every offset in the file, so a
    // mismatch is unrecoverable rather than merely inconvenient.
    if (p[4] / 8 != sizeof(int)) {
        printf("c4mp: %d-bit image, host is %d-bit\n", p[4], sizeof(int) * 8);
        free(buf);
        return 0;
    }
    // The padding word (byte offset 5) is the data MEMSZ in format v3:
    // the total in-memory data size, whose excess over datalen is
    // zero-filled BSS. v2 has no such field (memsz = datalen).
    memsz = (p[3] >= 3) ? loader_word(p + 5) : 0;
    p = p + 13;   // signature(3), version(1), wordbits(1), padding(8)

    entry    = loader_word(p); p = p + 8;
    codelen  = loader_word(p); p = p + 8;
    datalen  = loader_word(p); p = p + 8;
    patchlen = loader_word(p); p = p + 8;
    symlen   = loader_word(p); p = p + 8;
    conslen  = loader_word(p); p = p + 8;
    deslen   = loader_word(p); p = p + 8;
    if (memsz < datalen) memsz = datalen;

    // code
    p = p + 8;   // 'C' marker word
    if (!(code = (int *)malloc(codelen * 8))) { free(buf); return 0; }
    memcpy(code, p, codelen * 8);
    p = p + codelen * 8;

    // data: allocate the full in-memory size (memsz >= datalen), plus
    // one word of zero padding past the end because a guest may address
    // the last word by pointer arithmetic. Everything is zeroed, so the
    // BSS tail [datalen, memsz) is ready; only datalen bytes are copied.
    p = p + 8;   // 'D' marker
    if (!(data = (char *)malloc(memsz + 8))) { free(code); free(buf); return 0; }
    memset(data, 0, memsz + 8);
    memcpy(data, p, datalen);
    p = p + datalen;

    // patches: turn file offsets into host addresses
    p = p + 8;   // 'P' marker
    i = 0;
    while (i < patchlen) {
        ptype = loader_word(p);
        paddr = loader_word(p + 8);
        pvalu = loader_word(p + 16);
        p = p + 24;
        if (ptype == -1)      code[paddr] = (int)(code + pvalu);
        else if (ptype == -2) code[paddr] = (int)(data + pvalu);
        else if (ptype == -3) *(int *)(data + paddr) = (int)(code + pvalu);
        else if (ptype == -4) *(int *)(data + paddr) = (int)(data + pvalu);
        else {
            // A positive type is an unresolved external symbol. That
            // is the linker's job, not ours: c4rlink should have
            // consumed it, so its survival means an incomplete image.
            printf("c4mp: %s has an unresolved symbol (patch type %d)\n", path, ptype);
            free(code); free(data); free(buf);
            return 0;
        }
        ++i;
    }

    // Constructor and destructor lists. On disk each entry is a single
    // word -- the priority field the format documents is not written
    // (load-c4r.c reads only the value, with the priority read commented
    // out). Both lists point into buf, so they must be copied out.
    p = p + 8;   // 'c' marker
    cons = (int *)p;
    p = p + conslen * 8;
    des = (int *)p + 1;   // past the 'd' marker
    p = p + 8;   // 'd' marker
    p = p + deslen * 8;
    // 'S' marker and the symbol section follow. c4mp has no use for
    // symbols yet; STRC will want them when it learns to print names.

    if (conslen) {
        if (!(img->cons = (int *)malloc(conslen * 8))) {
            free(code); free(data); free(buf);
            return 0;
        }
        for (i = 0; i < conslen; ++i) img->cons[i] = (int)(code + cons[i]);
    }
    if (deslen) {
        if (!(img->des = (int *)malloc(deslen * 8))) {
            if (img->cons) free(img->cons);
            free(code); free(data); free(buf);
            return 0;
        }
        for (i = 0; i < deslen; ++i) img->des[i] = (int)(code + des[i]);
    }

    img->code  = code;
    img->data  = data;
    img->entry = code + entry;
    img->ncons = conslen;
    img->ndes  = deslen;
    free(buf);
    return 1;
}

void c4r_free(struct c4r_image *img) {
    if (img->cons) { free(img->cons); img->cons = 0; }
    if (img->des)  { free(img->des);  img->des  = 0; }
    if (img->code) { free(img->code); img->code = 0; }
    if (img->data) { free(img->data); img->data = 0; }
}
