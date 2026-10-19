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
// Shared libraries. An image linked with `c4rlink -l LIB` still holds
// symbol patches -- references to what LIB defines -- and a static
// global named __c4_needed for each library, whose value is the data
// offset of its run-time path ("/lib/libc4ix.c4s"). The loader reads
// the image, then every library it needs (and every library THOSE
// need, each once), and resolves each module's symbol patches against
// what the libraries export: their non-static functions and globals.
// Each task gets its own copy of every library's DATA, freed with the
// task -- a library's globals, errno and stdio buffers among them,
// belong to the task that uses them. Its CODE is shared when the
// library was compiled -fpic: c4lc then reaches every datum through
// __c4_dbase, a word the loader gives that library and the scheduler
// sets to the running task's copy of its data at every switch
// (loader_switch). The first task to load such a library leaves its
// relocated code in a cache, with the file as the template every later
// task's data copy is made from, so the second task loads a library
// without reading or relocating its code. A library built without
// -fpic is loaded whole, code and all, for every task. Library
// constructors run before the program's, deepest library first.
//

#include "c4ix.h"

enum { C4R_BUF_MAX = 4194304,     // 4MB image limit
       LIB_MAX = 8,               // libraries one program may load, in all
       SCLASS_FUN = 129, SCLASS_GLO = 131, SATTR_STATIC = 8, SATTR_EXTERN = 16 };

// Set while a caller is TRYING candidate paths and a miss is expected
// -- a bare C4KE program name may match a real file that is not an
// image at all. Complaints about those are noise, not diagnosis.
int loader_quiet;

// One image, read and relocated, with its file still at hand: the
// symbol patches and the symbol table are read out of it in place.
struct c4r_mod {
    char *buf;
    int *code;
    char *data;
    int entry;
    char *patches; int npatch;    // the patch section, in buf
    char *syms; int nsym;         // the symbol section, in buf
    int *cons; int ncons;         // constructor offsets, in buf
    int *des; int ndes;           // destructor offsets, in buf
    char *path;                   // a library's run-time path, for messages
    char *dtmpl;                  // the data as the file has it, in buf
    int datalen, memsz;
    int pic;                      // -fpic: refers to __c4_dbase
    int shared;                   // its code is the cache's: neither relink nor free it
    int *slot;                    // pic: the word that holds the task's data base
};

// -fpic libraries, loaded once: the first task's mod, kept, by path
struct shlib {
    char path[96];
    struct c4r_mod *mod;
    struct shlib *next;
};
static struct shlib *shlibs;

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

// A symbol record (load-c4r.c): id, type, class and attrs as words,
// then a 1-byte name length, the name, then the value word.
static int sym_class(char *p) { return loader_word(p + 2 * sizeof(int)); }
static int sym_attrs(char *p) { return loader_word(p + 3 * sizeof(int)); }
static int sym_namelen(char *p) { return p[4 * sizeof(int)] & 255; }
static char *sym_name(char *p) { return p + 4 * sizeof(int) + 1; }
static int sym_value(char *p) { return loader_word(p + 4 * sizeof(int) + 1 + sym_namelen(p)); }
static char *sym_next(char *p) { return p + 5 * sizeof(int) + 1 + sym_namelen(p); }

// Walk the symbol section looking for __c4ix_systable, a global the
// image declares so the kernel can hand it the syscall entry point.
// That is how userland reaches the kernel on plain c4, which has no
// trap machinery: libc4ix calls through the slot instead of raising
// an interrupt. On c4m the slot is still filled but libc4ix prefers
// the real trap gateway, so protected mode does its job.
static void loader_systable(char *p, int nsyms, int database) {
    int i;
    i = 0;
    while (i < nsyms) {
        if (loader_nameis(sym_name(p), sym_namelen(p), "__c4ix_systable")) {
            if (sym_class(p) == SCLASS_GLO)      // Glo: value is a data offset
                *(int *)(database + sym_value(p)) = (int)&sys_dispatch;
            return;
        }
        p = sym_next(p);
        ++i;
    }
}

static void mod_data_relocate(struct c4r_mod *m, char *d);

// does M's symbol table name NAME (defined or not)?
static int mod_has(struct c4r_mod *m, char *name) {
    char *p;
    int i;
    p = m->syms;
    i = 0;
    while (i < m->nsym) {
        if (loader_nameis(sym_name(p), sym_namelen(p), name)) return 1;
        p = sym_next(p);
        ++i;
    }
    return 0;
}

static void mod_free(struct c4r_mod *m) {
    if (m->shared) { if (m->data) free(m->data); m->data = 0; m->code = 0; m->buf = 0; return; }
    if (m->code) free(m->code);
    if (m->data) free(m->data);
    if (m->buf) free(m->buf);
    m->code = 0; m->data = 0; m->buf = 0;
}

// Read and relocate one image. Returns 1 and fills M; its buf stays
// allocated until the caller has linked it.
static int mod_read(char *path, struct c4r_mod *m) {
    char *buf, *p, *data;
    int *code;
    int fd, n, total;
    int entry, codelen, datalen, patchlen, symlen, conslen, deslen, memsz;
    int i, ptype, paddr, pvalu;
    struct vnode *vn;

    memset((char *)m, 0, sizeof(struct c4r_mod));
    if (!(buf = (char *)malloc(C4R_BUF_MAX))) return 0;
    // A RAM file by this path wins over the host. The comment below once
    // said the two could not name the same file, but a host is free to
    // answer a path it does not have -- c4bb's disk tries the bare file
    // name when the full path misses -- so "/ram/hello.c4r", compiled a
    // moment ago, ran the disk's own hello.c4r instead.
    vn = vfs_lookup(path);
    if (vn && vn->type != VN_RAMFILE) vn = 0;
    if (!vn && (fd = open(path, 0)) >= 0) {
        total = 0;
        // never past the buffer: a host file of any size can be named
        while (total < C4R_BUF_MAX &&
               (n = read(fd, buf + total, C4R_BUF_MAX - total < 65536 ? C4R_BUF_MAX - total : 65536)) > 0)
            total = total + n;
        close(fd);
    } else {
        if (!vn) {
            // Silent: callers try several candidate names (the shell
            // resolves "wc" against three), so a miss is routine and
            // the caller is the one that knows when to complain.
            free(buf);
            return 0;
        }
        // The RAM filesystem -- and this is not a convenience. A program the machine produced
        // ITSELF cannot be on the host: the C4 VM has no write syscall,
        // so nothing running under it can put a file there. What a
        // compiler running as a task CAN do is write to standard output
        // and let the shell redirect that into a RAM file, so
        // `c4th.c4r ... > /ram/prog.c4r` and then `/ram/prog.c4r` is
        // the whole compile-and-run loop, and this is what joins the
        // two halves. Without it the kernel can only ever run programs
        // that were built somewhere else.
        if (vn->lazy) vfs_fill(vn);
        total = vn->size;
        if (total > C4R_BUF_MAX) {
            kprintf("c4ix: loader: %s is %d bytes, over the %d limit\n",
                    path, total, C4R_BUF_MAX);
            free(buf);
            return 0;
        }
        memcpy(buf, vn->data, total);
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
    if (!(code = (int *)malloc(codelen * sizeof(int) + sizeof(int)))) { free(buf); return 0; }
    memcpy(code, p, codelen * sizeof(int));
    p = p + codelen * sizeof(int);

    // data: allocate the full in-memory size (memsz), zero-padded by
    // malloc+memset so the BSS tail [datalen, memsz) is ready; only
    // datalen bytes are copied from the image.
    p = p + sizeof(int);   // 'D' marker
    if (!(data = (char *)malloc(memsz + 8))) { free(code); free(buf); return 0; }
    memset(data, 0, memsz + 8);
    memcpy(data, p, datalen);
    m->dtmpl = p;
    p = p + datalen;

    // patches: byte-offset address rewrites into the fresh segments;
    // the symbol references among them wait for link_modules
    p = p + sizeof(int);   // 'P' marker
    m->patches = p;
    m->npatch = patchlen;
    p = p + patchlen * 3 * sizeof(int);

    // Constructor and destructor lists ('c' and 'd' markers), then
    // the symbol section ('S'). They point INTO buf.
    p = p + sizeof(int);   // 'c' marker
    m->cons = (int *)p;
    m->ncons = conslen;
    p = p + conslen * sizeof(int);
    p = p + sizeof(int);   // 'd' marker
    m->des = (int *)p;
    m->ndes = deslen;
    p = p + deslen * sizeof(int);
    p = p + sizeof(int);   // 'S' marker
    m->syms = p;
    m->nsym = symlen;

    m->buf = buf;
    m->code = code;
    m->data = data;
    m->entry = entry;
    m->datalen = datalen;
    m->memsz = memsz;
    m->pic = mod_has(m, "__c4_dbase");

    // relocate the code; under -fpic a code word that points into the
    // data holds the OFFSET, since __c4_dbase supplies the base
    p = m->patches;
    i = 0;
    while (i < patchlen) {
        ptype = loader_word(p); paddr = loader_word(p + sizeof(int)); pvalu = loader_word(p + 2 * sizeof(int));
        p = p + 3 * sizeof(int);
        if (ptype == -1) code[paddr] = (int)(code + pvalu);
        else if (ptype == -2) code[paddr] = m->pic ? pvalu : (int)(data + pvalu);
        ++i;
    }
    mod_data_relocate(m, data);
    return 1;
}

// The data-resident patches, applied to a copy D of M's data.
static void mod_data_relocate(struct c4r_mod *m, char *d) {
    char *p;
    int i, ptype, paddr, pvalu;
    p = m->patches;
    i = 0;
    while (i < m->npatch) {
        ptype = loader_word(p); paddr = loader_word(p + sizeof(int)); pvalu = loader_word(p + 2 * sizeof(int));
        p = p + 3 * sizeof(int);
        if (ptype == -3) *(int *)(d + paddr) = (int)(m->code + pvalu);
        else if (ptype == -4) *(int *)(d + paddr) = (int)(d + pvalu);
        ++i;
    }
}

// A fresh copy of a cached library's data, for one more task.
static char *mod_data_copy(struct c4r_mod *m) {
    char *d;
    if (!(d = (char *)malloc(m->memsz + 8))) return 0;
    memset(d, 0, m->memsz + 8);
    memcpy(d, m->dtmpl, m->datalen);
    mod_data_relocate(m, d);
    return d;
}

// The symbol with id ID in M (the linker writes ids in order, so it is
// usually the ID'th), or 0.
static char *mod_symbol(struct c4r_mod *m, int id) {
    char *p;
    int i;
    p = m->syms;
    i = 0;
    while (i < m->nsym) {
        if (loader_word(p) == id) return p;
        p = sym_next(p);
        ++i;
    }
    return 0;
}

// What M exports by NAME: the address of a non-static function or
// global it defines, or 0.
static int mod_export(struct c4r_mod *m, char *name, int len) {
    char *p;
    int i, c;
    p = m->syms;
    i = 0;
    while (i < m->nsym) {
        c = sym_class(p);
        if (sym_namelen(p) == len && !(sym_attrs(p) & (SATTR_STATIC | SATTR_EXTERN)) &&
            (c == SCLASS_FUN || c == SCLASS_GLO) && !memcmp(sym_name(p), name, len)) {
            if (c == SCLASS_FUN) return (int)(m->code + sym_value(p));
            return (int)(m->data + sym_value(p));
        }
        p = sym_next(p);
        ++i;
    }
    return 0;
}

static int str_eq(char *a, char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

// Load every library the modules need, breadth first, each once.
// Returns the module count, or 0 having said why.
// the cached -fpic library with this run-time path, or 0
static struct shlib *shlib_find(char *path) {
    struct shlib *s;
    s = shlibs;
    while (s) { if (str_eq(s->path, path)) return s; s = s->next; }
    return 0;
}

static int load_libraries(char *path, struct c4r_mod *mods) {
    int n, k, i, j, have;
    char *p, *lib;
    struct shlib *sh;
    n = 1;
    k = 0;
    while (k < n) {
        p = (mods + k)->syms;
        i = 0;
        while (i < (mods + k)->nsym) {
            if (loader_nameis(sym_name(p), sym_namelen(p), "__c4_needed") && sym_class(p) == SCLASS_GLO) {
                lib = (mods + k)->data + sym_value(p);
                have = 0;
                j = 1;
                while (j < n) { if (str_eq((mods + j)->path, lib)) have = 1; ++j; }
                if (!have) {
                    if (n > LIB_MAX) {
                        kprintf("c4ix: loader: %s needs more than %d libraries\n", path, LIB_MAX);
                        return 0;
                    }
                    // /lib/NAME in the VFS, or else NAME on the host: a
                    // native host's own /lib is not C4IX's, and the
                    // build leaves its libraries beside its programs
                    j = 0;
                    while (lib[j]) ++j;
                    while (j > 0 && lib[j - 1] != '/') --j;
                    if ((sh = shlib_find(lib))) {
                        // shared code already loaded: only the data is new
                        memcpy((char *)(mods + n), (char *)sh->mod, sizeof(struct c4r_mod));
                        (mods + n)->shared = 1;
                        have = ((mods + n)->data = mod_data_copy(sh->mod)) != 0;
                    } else {
                        loader_quiet = loader_quiet + 1;
                        have = mod_read(lib, mods + n) || mod_read(lib + j, mods + n);
                        loader_quiet = loader_quiet - 1;
                    }
                    if (!have) {
                        kprintf("c4ix: loader: %s needs %s, which is not there\n", path, lib);
                        return 0;
                    }
                    (mods + n)->path = lib;
                    ++n;
                }
            }
            p = sym_next(p);
            ++i;
        }
        ++k;
    }
    return n;
}

static int mod_is_data(struct c4r_mod *m, int addr);

// the id of the symbol named NAME in M, or -1
static int mod_symid(struct c4r_mod *m, char *name) {
    char *p;
    int i;
    p = m->syms;
    i = 0;
    while (i < m->nsym) {
        if (loader_nameis(sym_name(p), sym_namelen(p), name)) return loader_word(p);
        p = sym_next(p);
        ++i;
    }
    return -1;
}

// Resolve every module's symbol patches against what the libraries
// export. Returns 1, or 0 having named what was missing.
static int link_modules(char *path, struct c4r_mod *mods, int n) {
    struct c4r_mod *m;
    char *p, *s;
    char name[128];
    int k, i, j, ptype, paddr, addr, bad, len, dbid;
    bad = 0;
    k = 0;
    while (k < n) {
        m = mods + k;
        ++k;
        if (m->shared) continue;            // linked once, when it was first loaded
        if (m->pic && !m->slot) {
            if (k == 1) { kprintf("c4ix: loader: %s: a program cannot be -fpic\n", path); return 0; }
            m->slot = (int *)malloc(sizeof(int));
            *m->slot = (int)m->data;
        }
        // __c4_dbase is most of a -fpic library's symbol references (one
        // per function that touches data): find its id once, not per patch
        dbid = m->pic ? mod_symid(m, "__c4_dbase") : -1;
        p = m->patches;
        i = 0;
        while (i < m->npatch) {
            ptype = loader_word(p); paddr = loader_word(p + sizeof(int));
            p = p + 3 * sizeof(int);
            ++i;
            if (ptype < 0) continue;
            if (ptype == dbid) { m->code[paddr] = (int)m->slot; continue; }
            if (!(s = mod_symbol(m, ptype))) {
                kprintf("c4ix: loader: %s: a patch names symbol %d, which is not in its table\n", k > 1 ? m->path : path, ptype);
                bad = 1;
                continue;
            }
            if (m->pic && loader_nameis(sym_name(s), sym_namelen(s), "__c4_dbase")) {
                m->code[paddr] = (int)m->slot;
                continue;
            }
            addr = 0;
            j = 1;
            while (j < n && !addr) {
                if (j != k - 1) {
                    addr = mod_export(mods + j, sym_name(s), sym_namelen(s));
                    // shared code may only point at what every task shares
                    if (addr && m->pic && (!(mods + j)->pic || mod_is_data(mods + j, addr))) {
                        kprintf("c4ix: loader: %s is -fpic, but uses '%s' from %s, which is not shared code\n",
                                m->path, "a global", (mods + j)->path);
                        bad = 1;
                    }
                }
                ++j;
            }
            if (!addr) {
                // the name is not NUL-terminated in the file
                len = sym_namelen(s) < 127 ? sym_namelen(s) : 127;
                memcpy(name, sym_name(s), len);
                name[len] = 0;
                kprintf("c4ix: loader: %s needs '%s', which none of its libraries has\n",
                        k > 1 ? m->path : path, name);
                bad = 1;
                continue;
            }
            m->code[paddr] = addr;
        }
    }
    return !bad;
}

// is ADDR inside M's data?
static int mod_is_data(struct c4r_mod *m, int addr) {
    return addr >= (int)m->data && addr < (int)m->data + m->memsz + 8;
}

// Load and relocate, with any libraries. Returns 1 and fills img on success.
int c4r_load(char *path, struct c4r_image *img) {
    struct c4r_mod *mods;
    struct shlib *sh;
    int n, k, i, ncons, ndes, total;
    int *cons, *des, *libs;

    memset((char *)img, 0, sizeof(struct c4r_image));
    if (!(mods = (struct c4r_mod *)malloc((LIB_MAX + 1) * sizeof(struct c4r_mod)))) return 0;
    memset((char *)mods, 0, (LIB_MAX + 1) * sizeof(struct c4r_mod));
    if (!mod_read(path, mods)) { free(mods); return 0; }
    (mods + 0)->path = path;
    n = load_libraries(path, mods);
    if (n == 0 || !link_modules(path, mods, n)) {
        if (n == 0) n = LIB_MAX + 1;
        k = 0;
        while (k < n) { if ((mods + k)->buf) mod_free(mods + k); ++k; }
        free(mods);
        return 0;
    }

    // Only where traps do not exist: on c4m userland must go through
    // the real gateway so protected mode means something.
    if (host_type() == HOST_C4) {
        k = 0;
        while (k < n) { loader_systable((mods + k)->syms, (mods + k)->nsym, (int)(mods + k)->data); ++k; }
    }

    // Constructors and destructors, as absolute addresses for the task
    // to run itself: the deepest library first, the program last. The
    // destructors are run in reverse, so the program's go first.
    ncons = 0; ndes = 0;
    k = 0;
    while (k < n) { ncons = ncons + (mods + k)->ncons; ndes = ndes + (mods + k)->ndes; ++k; }
    cons = 0; des = 0;
    if (ncons) cons = (int *)malloc(ncons * sizeof(int));
    if (ndes) des = (int *)malloc(ndes * sizeof(int));
    total = 0;
    k = n - 1;
    while (k >= 0) {
        i = 0;
        while (i < (mods + k)->ncons) { cons[total++] = (int)((mods + k)->code + (mods + k)->cons[i]); ++i; }
        --k;
    }
    total = 0;
    k = n - 1;
    while (k >= 0) {
        i = 0;
        while (i < (mods + k)->ndes) { des[total++] = (int)((mods + k)->code + (mods + k)->des[i]); ++i; }
        --k;
    }

    // a -fpic library loaded for the first time stays, code and file,
    // for the next task that needs it
    k = 1;
    while (k < n) {
        if ((mods + k)->pic && !(mods + k)->shared) {
            sh = (struct shlib *)malloc(sizeof(struct shlib));
            sh->mod = (struct c4r_mod *)malloc(sizeof(struct c4r_mod));
            memcpy((char *)sh->mod, (char *)(mods + k), sizeof(struct c4r_mod));
            sh->mod->data = 0;
            i = 0;
            while ((mods + k)->path[i] && i < 95) { sh->path[i] = (mods + k)->path[i]; ++i; }
            sh->path[i] = 0;
            sh->next = shlibs;
            shlibs = sh;
            (mods + k)->shared = 1;
        }
        ++k;
    }

    // the libraries, for the task: code to free (0 when shared), its own
    // data, and the slot the scheduler points at that data (0 if none)
    libs = 0;
    if (n > 1) {
        libs = (int *)malloc(3 * (n - 1) * sizeof(int));
        k = 1;
        while (k < n) {
            libs[3 * (k - 1)] = (mods + k)->shared ? 0 : (int)(mods + k)->code;
            libs[3 * (k - 1) + 1] = (int)(mods + k)->data;
            libs[3 * (k - 1) + 2] = (int)(mods + k)->slot;
            ++k;
        }
    }

    img->code = (int)(mods + 0)->code;
    img->data = (int)(mods + 0)->data;
    img->entry = (int)((mods + 0)->code + (mods + 0)->entry);
    img->cons = (int)cons;
    img->ncons = ncons;
    img->des = (int)des;
    img->ndes = ndes;
    img->libs = (int)libs;
    img->nlibs = n - 1;
    k = 0;
    while (k < n) { if (!(mods + k)->shared) free((mods + k)->buf); ++k; }
    free(mods);
    return 1;
}

// The running task changes: point each shared library's __c4_dbase at
// the incoming task's copy of its data. The scheduler calls this at
// every switch; a task that uses no -fpic library costs one test.
void loader_switch(struct task *t) {
    int *l;
    int k;
    if (!t || !t->img_libs) return;
    l = (int *)t->img_libs;
    k = 0;
    while (k < t->img_nlibs) {
        if (l[3 * k + 2]) *(int *)l[3 * k + 2] = l[3 * k + 1];
        ++k;
    }
}

// Load an image and run it as a task. argv is passed through to the
// image's main untouched. Spawned programs are userland: on c4m they
// run behind the protected-mode boundary, on plain c4 (no such
// hardware) the privilege level is recorded but unenforced.
struct task *task_spawn_priv(char *path, int argc, int argv, int privs) {
    struct c4r_image img;
    struct task *t;
    char *name;
    int i;

    if (!c4r_load(path, &img)) return 0;
    // The task is named for the program, not the path it was found by:
    // ps says "spin" whether the shell ran /bin/spin or a host image.
    name = path;
    i = 0;
    while (path[i]) { if (path[i] == '/') name = path + i + 1; ++i; }
    if (!(t = task_create(name, img.entry, argc, argv))) {
        c4r_unload(&img);
        return 0;
    }
    t->img_code = img.code;
    t->img_data = img.data;
    t->img_cons = img.cons;
    t->img_ncons = img.ncons;
    t->img_des = img.des;
    t->img_ndes = img.ndes;
    t->img_libs = img.libs;
    t->img_nlibs = img.nlibs;
    t->privs = (host_type() == HOST_C4M) ? privs : PRIV_KERNEL;
    return t;
}

// Free what c4r_load allocated: the image, its libraries, the lists.
void c4r_unload(struct c4r_image *img) {
    int k;
    if (img->code) free((int *)img->code);
    if (img->data) free((char *)img->data);
    if (img->cons) free((int *)img->cons);
    if (img->des) free((int *)img->des);
    if (img->libs) {
        k = 0;
        while (k < img->nlibs) {
            if (((int *)img->libs)[3 * k]) free((int *)((int *)img->libs)[3 * k]);
            free((char *)((int *)img->libs)[3 * k + 1]);
            ++k;
        }
        free((int *)img->libs);
    }
    memset((char *)img, 0, sizeof(struct c4r_image));
}

struct task *task_spawn(char *path, int argc, int argv) {
    return task_spawn_priv(path, argc, argv, PRIV_USER);
}
