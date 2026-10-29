//
// C4IX: the MMU (docs/c4ix-mmu.md).
//
// libjs fits an MMU when its host asks for one (cli.js --mmu, the page's
// ?mmu=1), and announces it as C4I_MMU. It translates user mode only:
// the kernel runs untranslated, on physical addresses, as it always has.
// This module owns the page tables, two levels of 1,024 words in the
// i386's shape, built from pages of the heap; turns the MMU on, on
// every CPU, with a stack of its own for the traps that come from user
// mode; hands user tasks their memory in whole pages, mapped for them
// and for nobody else; checks the pointers they pass the kernel; and
// says what a page fault was.
//
// Each user task has an address space of its own: a page directory that
// maps its pages and the shared libraries' code, and nothing else.
// Memory is mapped at its own address (an identity map), so a pointer a
// task passes in a syscall is one the kernel can use once it has checked
// it (uaccess) against the task's own tables.
//
// Without the MMU none of this runs: umalloc and ufree are malloc and
// free, and every task's memory is the heap's, as it always was.
//

#include "c4ix.h"

int mmu_on;                    // user mode is translated
static int *mmu_dir;           // the directory the kernel's own tasks run in: nothing mapped
static int mmu_memsize;        // the arena, bytes
static int mmu_npages;
static int mmu_ntables;        // page tables made
int mmu_kstack[CPU_MAX];       // each CPU's trap stack (its base)

// ---- pages for the tables ----
//
// A table is one page, page-aligned. The heap's blocks are 8-byte
// aligned, so pages come from blocks of 16 pages at a time, of which
// the aligned 15 or 16 inside are used; freed ones go on a list threaded
// through their first word.
enum { MMU_CHUNK = 16 };
static int mmu_freepage;
static int mmu_chunkp, mmu_chunkn;

static int mmu_ulock[2];       // the regions, the map and the page list (klock: SMP's light path)

static int mmu_page() {
    int p, raw;
    klock_take(mmu_ulock);
    if (mmu_freepage) {
        p = mmu_freepage;
        mmu_freepage = *(int *)p;
    } else {
        if (!mmu_chunkn) {
            if (!(raw = (int)malloc(MMU_CHUNK * MMU_PAGE))) { klock_drop(mmu_ulock); return 0; }
            mmu_chunkp = (raw + MMU_PAGE - 1) & ~(MMU_PAGE - 1);
            mmu_chunkn = (raw + MMU_CHUNK * MMU_PAGE - mmu_chunkp) / MMU_PAGE;
        }
        p = mmu_chunkp;
        mmu_chunkp = mmu_chunkp + MMU_PAGE;
        --mmu_chunkn;
    }
    klock_drop(mmu_ulock);
    memset((char *)p, 0, MMU_PAGE);
    return p;
}

// VA's entry in directory DIR, its table made if MAKE; 0 if there is none
static int *mmu_pte(int *dir, int va, int make) {
    int *pt;
    int pde;
    pde = dir[(va >> 22) & 1023];
    if (!(pde & PTE_P)) {
        if (!make || !(pt = (int *)mmu_page())) return 0;
        ++mmu_ntables;
        dir[(va >> 22) & 1023] = (int)pt | PTE_P;
    } else pt = (int *)(pde & ~(MMU_PAGE - 1));
    return pt + ((va >> 12) & 1023);
}

// Map N bytes at VA, page by page, to the same addresses, with FLAGS
// (0 unmaps). This CPU forgets what it had of them; another CPU running
// the same space is not told (docs/c4ix-mmu.md 10).
int mmu_map(int *dir, int va, int n, int flags) {
    int *e;
    int end;
    end = va + n;
    va = va & ~(MMU_PAGE - 1);
    while (va < end) {
        if (!(e = mmu_pte(dir, va, flags != 0))) { if (flags) return 0; }
        else {
            *e = flags ? (va | flags) : 0;
            *(int *)MMU_INVLPG = va;
        }
        va = va + MMU_PAGE;
    }
    return 1;
}

// ---- user memory ----
//
// A region is whole pages, page-aligned, cut from one heap block (raw)
// N + 4095 bytes long. Its bookkeeping is the kernel's, in an SL4B
// object, never in the pages themselves: a task can write those, and a
// header there would let it point the kernel's frees anywhere. Every
// page knows its region (mmu_pg, the kernel's map of all memory by page),
// which is how a free finds it and how a fault says whose an address is.
//
// UR_BLOCK   one allocation: an image's code or data, a stack, argv, or
//            a block a task asked for
// UR_CHUNK   pages a task's small blocks are cut from, a page at a time
// UR_SLAB    one page of a chunk, cut into blocks of one size
//
// A region is unowned (owner 0) from umalloc until mmu_adopt gives it to
// the task it was made for; only then is it mapped.
enum { UR_BLOCK = 1, UR_CHUNK = 2, UR_SLAB = 3 };
// Small blocks: 16 to 2048 bytes in eight sizes, from chunks of 16 pages
enum { UR_SMALL = 2048, UR_CLASSES = 8, UR_CHUNK_PAGES = 16 };

struct urgn {
    int kind;                  // UR_*
    int base;                  // the first page
    int npages;
    int raw;                   // the heap block to free; 0 for a slab
    int flags;                 // PTE_* it is mapped with
    struct task *owner;        // 0 until adopted
    int shared;                // a cached library's code: mapped, never freed
    int heap;                  // what the task asked for (or its entry page): freed with its heap
    int *dir;                  // the space it is mapped in: its owner's, or a vfork parent's
    struct urgn *next;         // the owner's regions
    // UR_SLAB
    int size;                  // its blocks' size
    int nfree;
    int used[8];               // a bit a block, 256 at most
    struct urgn *snext;        // the owner's next slab of this size
    // UR_CHUNK: pages not yet made slabs
    int left;
};

static struct sl4b_cache *mmu_rcache;
static int *mmu_pg;            // a page's region, by page number

static void mmu_setpg(struct urgn *r, int base, int n, struct urgn *v) {
    int k;
    k = 0;
    while (k < n) { mmu_pg[(base >> 12) + k] = (int)v; ++k; }
}

// The region at P, or 0
static struct urgn *mmu_rgn(int p) {
    if (p < MMU_PAGE || p >= mmu_memsize) return 0;
    return (struct urgn *)mmu_pg[p >> 12];
}

// Whose memory a task's region is: a vfork child allocates for its parent
static struct task *mmu_root(struct task *t) {
    while (t && t->vfparent) t = t->vfparent;
    return t;
}

// T's address space: a vfork child's is its parent's, whose memory it
// is using; a kernel task has none. Made on first use, with the shared
// libraries' slot pages in it.
static int mmu_nslotpages;
static int mmu_slotpages[8];

static int *mmu_dir_of(struct task *t) {
    int *d;
    int k;
    t = mmu_root(t);
    if (!t || t->privs != PRIV_USER) return mmu_dir;
    if (t->mmdir) return (int *)t->mmdir;
    if (!(d = (int *)mmu_page())) return mmu_dir;
    k = 0;
    while (k < mmu_nslotpages) { mmu_map(d, mmu_slotpages[k], MMU_PAGE, PTE_P | PTE_U); ++k; }
    t->mmdir = (int)d;
    return d;
}

// T is done with its space (task_destroy, and execve's old one): its
// tables go back to the page list. What was mapped in it is freed by name.
static void mmu_dir_free(int *d) {
    int k;
    if (!d || d == mmu_dir) return;
    k = 0;
    while (k < 1024) {
        if (d[k] & PTE_P) {
            *(int *)(d[k] & ~(MMU_PAGE - 1)) = mmu_freepage;
            mmu_freepage = d[k] & ~(MMU_PAGE - 1);
            --mmu_ntables;
        }
        ++k;
    }
    *d = mmu_freepage;
    mmu_freepage = (int)d;
}

void mmu_task_done(struct task *t) {
    if (!mmu_on || !t->mmdir) return;
    klock_take(mmu_ulock);
    mmu_dir_free((int *)t->mmdir);
    klock_drop(mmu_ulock);
    t->mmdir = 0;
}

// The switch (loader_switch, at every one): the incoming task's space on
// this CPU. A kernel task leaves the last one in: it runs untranslated,
// and the user task it is likely to go back to finds its own still there.
void mmu_switch(struct task *t) {
    int d;
    if (!mmu_on || !t || t->privs != PRIV_USER) return;
    d = (int)mmu_dir_of(t);
    if (*(int *)MMU_PTBR != d) *(int *)MMU_PTBR = d;
}

// A region of N bytes, unowned and unmapped: the kernel's until adopted
static struct urgn *mmu_region(int n, int kind) {
    struct urgn *r;
    int raw, np;
    np = (n + MMU_PAGE - 1) / MMU_PAGE;
    if (np < 1) np = 1;
    if (!(r = (struct urgn *)sl4b_alloc(mmu_rcache))) return 0;
    if (!(raw = (int)malloc(np * MMU_PAGE + MMU_PAGE - 1))) { sl4b_free(mmu_rcache, (char *)r); return 0; }
    r->kind = kind;
    r->raw = raw;
    r->base = (raw + MMU_PAGE - 1) & ~(MMU_PAGE - 1);
    r->npages = np;
    klock_take(mmu_ulock);
    mmu_setpg(r, r->base, np, r);
    klock_drop(mmu_ulock);
    return r;
}

// Map R into its owner's space, or take it out of the space it is in
static void mmu_rmap(struct urgn *r, int flags) {
    if (flags) r->dir = mmu_dir_of(r->owner);
    if (r->dir) mmu_map(r->dir, r->base, r->npages * MMU_PAGE, flags);
    if (!flags) r->dir = 0;
}

static void mmu_give(struct urgn *r, struct task *t, int flags) {
    r->owner = t;
    r->flags = flags;
    klock_take(mmu_ulock);
    r->next = (struct urgn *)t->urgns;
    t->urgns = (int)r;
    klock_drop(mmu_ulock);
    mmu_rmap(r, flags);
}

// Off its owner's list, out of its space, back to the heap
static void mmu_release(struct urgn *r) {
    struct urgn **pp;
    int *ring;
    if (r->owner) {
        klock_take(mmu_ulock);
        pp = (struct urgn **)&r->owner->urgns;
        while (*pp && *pp != r) pp = &(*pp)->next;
        if (*pp) *pp = r->next;
        klock_drop(mmu_ulock);
        if (r->kind != UR_SLAB) mmu_rmap(r, 0);
    }
    // a display ring the task never detached: the host would go on
    // writing events into memory that is someone else's now
    if (host_has(C4IX_I_GUI)) {
        ring = (int *)C4IX_GUI_RING;
        if (*ring >= r->base && *ring < r->base + r->npages * MMU_PAGE) *ring = 0;
    }
    klock_take(mmu_ulock);
    if (r->kind != UR_SLAB) mmu_setpg(r, r->base, r->npages, 0);
    klock_drop(mmu_ulock);
    if (r->raw) free((char *)r->raw);
    sl4b_free(mmu_rcache, (char *)r);
}

// N bytes the kernel fills for a task that has not got them yet: an
// image, a stack, argv. Page-aligned, and the kernel's until mmu_adopt.
int umalloc(int n) {
    struct urgn *r;
    if (!mmu_on) return (int)malloc(n);
    if (!(r = mmu_region(n, UR_BLOCK))) return 0;
    return r->base;
}

// A umalloc'd region given back, or a kernel block
void ufree(int p) {
    struct urgn *r;
    if (!p) return;
    if (!mmu_on) { free((char *)p); return; }
    if ((r = mmu_rgn(p)) && r->kind == UR_BLOCK && r->base == p) {
        if (!r->shared) mmu_release(r);
        return;
    }
    free((char *)p);
}

// A cached library's code: in every space that loads the library
// (mmu_adopt maps it), and never freed
void umem_share(int p) {
    struct urgn *r;
    if (!mmu_on || !(r = mmu_rgn(p))) return;
    r->shared = 1;
    r->flags = PTE_P | PTE_U | PTE_X | PTE_W;
}

// Give P, if it is an unowned region, to T, mapped with FLAGS; a shared
// one is mapped into T's space as it is everyone's
static void mmu_take(struct task *t, int p, int flags) {
    struct urgn *r;
    if (!p || !(r = mmu_rgn(p)) || r->kind != UR_BLOCK || r->base != p) return;
    if (r->shared) { mmu_map(mmu_dir_of(t), r->base, r->npages * MMU_PAGE, r->flags); return; }
    if (r->owner) return;
    mmu_give(r, t, flags);
}

// What the kernel made for T while it was being made, now T's: its stack,
// its image and its libraries' data, its arguments and environment.
// Called when T becomes a user task (task_spawn_priv), when a vfork child
// is made, and after execve.
void mmu_adopt(struct task *t) {
    int *l;
    int k, code;
    if (!mmu_on || !t || t->privs != PRIV_USER) return;
    code = PTE_P | PTE_U | PTE_X | PTE_W;
    mmu_take(t, t->stack, PTE_P | PTE_U | PTE_W);
    mmu_take(t, t->argv_vec, PTE_P | PTE_U | PTE_W);
    mmu_take(t, t->argv_data, PTE_P | PTE_U | PTE_W);
    mmu_take(t, t->env_vec, PTE_P | PTE_U | PTE_W);
    mmu_take(t, t->env_data, PTE_P | PTE_U | PTE_W);
    mmu_take(t, t->img_code, code);
    mmu_take(t, t->img_data, PTE_P | PTE_U | PTE_W);
    if (t->img_libs) {
        l = (int *)t->img_libs;
        k = 0;
        while (k < t->img_nlibs) {
            mmu_take(t, l[3 * k], code);
            mmu_take(t, l[3 * k + 1], PTE_P | PTE_U | PTE_W);
            ++k;
        }
    }
}

// A block of N bytes for task T to use (SYS_SBRK, and malloc behind
// protected mode): up to UR_SMALL bytes from a slab of its own, more as
// a region of its own
static int mmu_cls(int n) {
    int c, k;
    c = 16; k = 0;
    while (c < n) { c = c + c; ++k; }
    return k;
}

static struct urgn *mmu_newslab(struct task *t, int k) {
    struct urgn *c, *s;
    c = (struct urgn *)t->uchunk;
    if (!c || !c->left) {
        if (!(c = mmu_region(UR_CHUNK_PAGES * MMU_PAGE, UR_CHUNK))) return 0;
        c->left = c->npages;
        c->heap = 1;
        mmu_give(c, t, PTE_P | PTE_U | PTE_W);
        t->uchunk = (int)c;
    }
    if (!(s = (struct urgn *)sl4b_alloc(mmu_rcache))) return 0;
    s->kind = UR_SLAB;
    s->heap = 1;
    s->base = c->base + (c->npages - c->left) * MMU_PAGE;
    s->npages = 1;
    s->owner = t;
    s->size = 16 << k;
    s->nfree = MMU_PAGE / s->size;
    --c->left;
    klock_take(mmu_ulock);
    mmu_pg[s->base >> 12] = (int)s;
    s->next = (struct urgn *)t->urgns;
    t->urgns = (int)s;
    s->snext = (struct urgn *)t->uslab[k];
    t->uslab[k] = (int)s;
    klock_drop(mmu_ulock);
    return s;
}

int umem_alloc(struct task *t, int n) {
    struct urgn *r, *s;
    int k, i, w, b;
    t = mmu_root(t);
    if (n < 0) return 0;
    if (n > UR_SMALL) {
        if (!(r = mmu_region(n, UR_BLOCK))) return 0;
        r->heap = 1;
        mmu_give(r, t, PTE_P | PTE_U | PTE_W);
        return r->base;
    }
    k = mmu_cls(n);
    klock_take(mmu_ulock);
    s = (struct urgn *)t->uslab[k];
    while (s && !s->nfree) s = s->snext;
    klock_drop(mmu_ulock);
    if (!s && !(s = mmu_newslab(t, k))) return 0;
    klock_take(mmu_ulock);
    i = 0;
    while (i < MMU_PAGE / s->size) {
        w = s->used[i >> 5];
        b = 1 << (i & 31);
        if (w == -1) { i = (i | 31) + 1; continue; }
        if (!(w & b)) break;
        ++i;
    }
    s->used[i >> 5] = s->used[i >> 5] | (1 << (i & 31));
    --s->nfree;
    klock_drop(mmu_ulock);
    // memory a task gets is zeroed: the last owner's bytes are not its
    memset((char *)(s->base + i * s->size), 0, s->size);
    return s->base + i * s->size;
}

// P from T's blocks back; anything that is not one of them is left alone
// (a task cannot free what is not its own)
void umem_free(struct task *t, int p) {
    struct urgn *r;
    int i;
    t = mmu_root(t);
    if (!p || !(r = mmu_rgn(p)) || r->owner != t) return;
    if (r->kind == UR_SLAB) {
        i = (p - r->base) / r->size;
        if (r->base + i * r->size != p) return;
        klock_take(mmu_ulock);
        if (r->used[i >> 5] & (1 << (i & 31))) {
            r->used[i >> 5] = r->used[i >> 5] & ~(1 << (i & 31));
            ++r->nfree;
        }
        klock_drop(mmu_ulock);
        return;
    }
    if (r->kind == UR_BLOCK && r->base == p) mmu_release(r);
}

// T's heap, at its end or its execve (task_drop_image): its blocks, the
// chunks and slabs its small ones came from, and its entry page. Its
// image, stack and arguments are freed by name. ALL: everything it still
// owns, whatever it is (task_destroy, last).
void umem_free_all(struct task *t, int all) {
    struct urgn *r, **pp;
    if (!mmu_on) return;
    pp = (struct urgn **)&t->urgns;
    while ((r = *pp)) {
        if (!all && !r->heap) { pp = &r->next; continue; }
        if (r->kind == UR_SLAB) {
            klock_take(mmu_ulock);
            *pp = r->next;
            klock_drop(mmu_ulock);
            sl4b_free(mmu_rcache, (char *)r);
        } else mmu_release(r);
    }
    t->uchunk = 0;
    memset((char *)t->uslab, 0, UR_CLASSES * sizeof(int));
}

// What T holds, in bytes (ps's memory column)
int umem_bytes(struct task *t) {
    struct urgn *r;
    int b;
    b = 0;
    klock_take(mmu_ulock);
    r = (struct urgn *)t->urgns;
    while (r) { if (r->kind != UR_SLAB) b = b + r->npages * MMU_PAGE; r = r->next; }
    klock_drop(mmu_ulock);
    return b;
}

// ---- a user task's first instructions ----
//
// A task used to start in task_shim, kernel code that read its
// constructors from its struct. User mode can do neither now, so the
// same sequence goes in a page of its own, as boot.js writes the
// machine's first one. The forged frame (sched_forge) has argv, argc and
// the entry above a return address, so this is a function of three:
//
//      ENT 0
//      IMM 0; PSH; JSR ctor; ADJ 1         each constructor, in order
//      LEA 3; LI; PSH; LEA 2; LI; PSH      argc, argv
//      JSRS 4; ADJ 2                       the entry, through its slot
//      PSH                                 its result, under the rest
//      JSR dtor                            each destructor, in reverse
//      IMM SYS_EXIT; PSH; OPCD             exit(result)
//      JMP .                               (exit does not come back)
enum {
    OPC_LEA = 0, OPC_IMM = 1, OPC_JMP = 2, OPC_JSR = 3, OPC_ENT = 6, OPC_ADJ = 7,
    OPC_LI = 9, OPC_PSH = 13, OPC_OPCD = 48, OPC_JSRS = 62
};

int mmu_user_entry(struct task *t) {
    struct urgn *r;
    int *w, *c;
    int i, n;
    if (!mmu_on || t->privs != PRIV_USER || t->sv != SV_FRAME) return 1;
    n = 24 + 7 * t->img_ncons + 2 * t->img_ndes;
    if (!(r = mmu_region(n * sizeof(int), UR_BLOCK))) return 0;
    w = (int *)r->base;
    *w++ = OPC_ENT; *w++ = 0;
    c = (int *)t->img_cons;
    i = 0;
    while (i < t->img_ncons) {
        *w++ = OPC_IMM; *w++ = 0; *w++ = OPC_PSH;
        *w++ = OPC_JSR; *w++ = c[i]; *w++ = OPC_ADJ; *w++ = 1;
        ++i;
    }
    *w++ = OPC_LEA; *w++ = 3; *w++ = OPC_LI; *w++ = OPC_PSH;
    *w++ = OPC_LEA; *w++ = 2; *w++ = OPC_LI; *w++ = OPC_PSH;
    *w++ = OPC_JSRS; *w++ = 4; *w++ = OPC_ADJ; *w++ = 2;
    *w++ = OPC_PSH;
    c = (int *)t->img_des;
    i = t->img_ndes;
    while (i) { --i; *w++ = OPC_JSR; *w++ = c[i]; }
    *w++ = OPC_IMM; *w++ = SYS_EXIT; *w++ = OPC_PSH; *w++ = OPC_OPCD;
    *w = OPC_JMP; w[1] = (int)w; w = w + 2;
    r->heap = 1;
    mmu_give(r, t, PTE_P | PTE_U | PTE_X | PTE_W);
    t->sv_pc = r->base;
    return 1;
}

// ---- the shared libraries' data slots ----

// A new page of slots, into every space there is
static void mmu_slotall(int page) {
    struct task *t;
    t = task_first();
    while (t) {
        if (t->mmdir) mmu_map((int *)t->mmdir, page, MMU_PAGE, PTE_P | PTE_U);
        t = t->next;
    }
}
//
// A -fpic library reads the address of the running task's copy of its
// data from a word the scheduler sets (loader_switch). User mode reads
// it, and must not write it: the slots are a page of their own, mapped
// readable into every space. (Under SMP they are the CPU-local window's.)
static int mmu_slotpage, mmu_nslots;

int mmu_slot() {
    struct urgn *r;
    if (!mmu_slotpage || mmu_nslots == MMU_PAGE / sizeof(int)) {
        if (mmu_nslotpages == 8 || !(r = mmu_region(MMU_PAGE, UR_BLOCK))) return 0;
        r->shared = 1;
        r->flags = PTE_P | PTE_U;
        mmu_slotpage = r->base;
        mmu_nslots = 0;
        mmu_slotpages[mmu_nslotpages++] = r->base;
        mmu_slotall(r->base);
    }
    ++mmu_nslots;
    return mmu_slotpage + (mmu_nslots - 1) * sizeof(int);
}

// ---- checking what a task hands the kernel ----
//
// Could T use the N bytes at P (for writing, if W)? The task's own page
// tables say: a syscall must not be a way round them.
int uaccess(int p, int n, int w) {
    struct task *t;
    int *e;
    int va, end, need;
    if (!mmu_on || !(t = sched_current()) || t->privs != PRIV_USER) return 1;
    if (n <= 0) return 1;
    end = p + n;
    if (p < MMU_PAGE || end < p || end > mmu_memsize) return 0;
    need = PTE_P | PTE_U | (w ? PTE_W : 0);
    va = p & ~(MMU_PAGE - 1);
    while (va < end) {
        if (!(e = mmu_pte(mmu_dir_of(t), va, 0)) || (*e & need) != need) return 0;
        va = va + MMU_PAGE;
    }
    return 1;
}

// ... and a string: its length, or -1
int ustrlen(char *s) {
    struct task *t;
    int *e;
    int n, va;
    if (!mmu_on || !(t = sched_current()) || t->privs != PRIV_USER) {
        n = 0;
        while (s[n]) ++n;
        return n;
    }
    n = 0;
    va = -1;
    while (1) {
        if (((int)s + n & ~(MMU_PAGE - 1)) != va) {
            va = (int)s + n & ~(MMU_PAGE - 1);
            if ((int)s + n < MMU_PAGE || (int)s + n >= mmu_memsize) return -1;
            if (!(e = mmu_pte(mmu_dir_of(t), va, 0)) || (*e & (PTE_P | PTE_U)) != (PTE_P | PTE_U)) return -1;
        }
        if (!s[n]) return n;
        ++n;
    }
    return -1;
}

// ... a vector of N strings (N < 0: up to a null pointer), and each
int uvecok(char **v, int n) {
    int k;
    if (!mmu_on || !v) return 1;
    k = 0;
    while (n < 0 || k < n) {
        if (!uaccess((int)(v + k), sizeof(int), 0)) return 0;
        if (!v[k]) return n < 0 || k >= n;
        if (ustrlen(v[k]) < 0) return 0;
        ++k;
    }
    return 1;
}

// N bytes of the kernel's at P, copied into the running task's own
// memory for it to read: what the C4KE layer hands a program that its
// kernel would have handed it a pointer into itself for. Freed with the
// task, or by its free().
char *ucopy(char *p, int n) {
    char *d;
    if (!(d = (char *)umem_alloc(sched_current(), n))) return 0;
    memcpy(d, p, n);
    return d;
}

// ---- turning it on ----

// This CPU: a stack for traps from user mode, the directory, and on.
// CPU 0 from mmu_init; the others as they start (sched.c smp_ap).
void mmu_cpu(int k) {
    int s;
    if (!mmu_on) return;
    if (!(s = (int)malloc(MMU_KSTACK))) {
        kputs("c4ix: panic: no memory for a trap stack\n");
        exit(-1);
    }
    mmu_kstack[k] = s;
    *(int *)MMU_KSP = s + MMU_KSTACK;
    *(int *)MMU_PTBR = (int)mmu_dir;   // until the first user task
    // Page 0: the ROM, c4bb's devices, the display, the sound card, and
    // the CPU-local window to read (the libraries' data slots) but not
    // to write: it is the kernel's struct cpu (docs/c4ix-mmu.md 7)
    *(int *)MMU_IOPM = IOPM_ROM | IOPM_DEV | IOPM_GUI | IOPM_SND | IOPM_LOCAL;
    *(int *)MMU_CTL = 1;
    klog("mmu: CPU %d translating user mode, its traps on a %d KB stack at 0x%08x\n",
        k, MMU_KSTACK / 1024, s);
}

// At boot, before anything runs in user mode
void mmu_init() {
    if (!host_has(C4IX_I_MMU)) return;
    mmu_memsize = *(int *)MMU_MEMSIZE;
    mmu_npages = mmu_memsize / MMU_PAGE;
    if (!(mmu_rcache = sl4b_cache_create("urgn", sizeof(struct urgn), 32)) ||
        !(mmu_pg = (int *)malloc(mmu_npages * sizeof(int))) ||
        !(mmu_dir = (int *)mmu_page())) {
        klog("mmu: no memory for its tables; left off\n");
        return;
    }
    memset((char *)mmu_pg, 0, mmu_npages * sizeof(int));
    mmu_on = 1;
    klog("mmu: 4 KB pages, %d of them; each user task has a space of its own, its own pages and the shared code\n",
        mmu_npages);
    mmu_cpu(0);
}

// ---- faults ----

// Whose memory VA is, for the fault's message
static void mmu_whose(int va, char *buf) {
    struct urgn *r;
    char *s;
    int i;
    r = mmu_rgn(va);
    if (va < MMU_PAGE) s = "page 0";
    else if (va >= mmu_memsize) s = "beyond memory";
    else if (r && r->shared) s = "shared library code";
    else if (r && r->owner) {
        s = "task ";
        i = 0;
        while (*s) { buf[i] = *s; ++s; ++i; }
        s = r->owner->name;
        while (*s && i < 40) { buf[i] = *s; ++s; ++i; }
        buf[i] = 0;
        return;
    }
    else if (r) s = "the kernel's, for a task being made";
    else if (va < *(int *)C4IX_HEAP_BASE) s = "the kernel's image";
    else s = "the kernel's heap";
    i = 0;
    while (*s) { buf[i] = *s; ++s; ++i; }
    buf[i] = 0;
}

// A user task took a page fault (TRAP_SEGV): say what it was, and kill it
void mmu_fault(struct task *t, int va) {
    int info, pc;
    char *what;
    char whose[48];
    info = *(int *)MMU_FINFO;
    pc = *(int *)MMU_FPC;
    what = (info & MMU_FK_WRITE) ? "write to" : (info & MMU_FK_FETCH) ? "jump to" : "read of";
    mmu_whose(va, whose);
    kprintf("c4ix: task %d (%s): page fault: %s 0x%08x (%s, %s) at pc 0x%08x; killed\n",
        t ? t->id : -1, t ? t->name : "?", what, va, whose,
        (info & MMU_FK_PRESENT) ? "not allowed" : "not mapped", pc);
    if (t) t->killsig = C4IX_SIGSEGV;
    task_exit(-1);
}
