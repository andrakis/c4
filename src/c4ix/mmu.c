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
// maps its pages and the shared libraries' code, and nothing else. User
// addresses are a window of 256 MB from mmu_uoff, handed out by an
// allocator of their own (mmu_valloc), so they are not physical ones,
// none of them is the kernel's, and memory can move under them: swap
// (below) puts a task's idle blocks out to the host and brings them back
// wherever there is room. The kernel's map (mmu_kdir) maps every user
// address there is to its memory; kernel mode, untranslated, reaches a
// user address through it (libjs's user window, c4m.js kpa) as readily
// as its own, so a pointer a task passes in a syscall is one the kernel
// can follow once it has checked it (uaccess) against the task's own
// tables. What the kernel hands user mode is the user address of its
// memory (mmu_uva). Memory larger than the window's base keeps the
// identity map (mmu_uoff 0), and no swap.
//
// Without the MMU none of this runs: umalloc and ufree are malloc and
// free, and every task's memory is the heap's, as it always was.
//

#include "c4ix.h"

int mmu_on;                    // user mode is translated
int mmu_uoff;                  // where user addresses start
int mmu_short;                 // memory is short: allocations take the kernel lock (sched_light)
static int *mmu_kdir;          // the kernel's map of every user address (UDIR)
static int *mmu_vpg;           // a user page's region, by its page in the window
static int *mmu_vbits;         // the window's pages handed out, a bit each
static int mmu_vnext;          // where the next search for free ones starts
static int mmu_gen;            // bumped when a page leaves a space no CPU is running
static int mmu_seen[CPU_MAX];  // ... as each CPU last loaded its space
static int mmu_selfok;         // swap: the asking task's own blocks may go too (a page fault's, not a syscall's)
enum { MMU_WINPAGES = 65536 }; // the window: 256 MB
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

// Map N bytes at user address VA, page by page, to the memory at PA,
// with FLAGS (0 unmaps). This CPU forgets what it had of them; another
// CPU that has the space loaded but is not running it forgets them when
// it next loads it (mmu_gen, mmu_switch); one running it is never asked
// to (docs/c4ix-mmu.md 10).
int mmu_map(int *dir, int va, int pa, int n, int flags) {
    int *e;
    int k, np;
    np = (n + (va & (MMU_PAGE - 1)) + MMU_PAGE - 1) / MMU_PAGE;
    va = va & ~(MMU_PAGE - 1);
    pa = pa & ~(MMU_PAGE - 1);
    k = 0;
    while (k < np) {
        if (!(e = mmu_pte(dir, va, flags != 0))) { if (flags) return 0; }
        else {
            *e = flags ? (pa | flags) : 0;
            *(int *)MMU_INVLPG = va;
        }
        va = va + MMU_PAGE;
        pa = pa + MMU_PAGE;
        ++k;
    }
    return 1;
}

// ---- user addresses ----
//
// The window's pages, a bit each; its first page is never handed out, so
// that no user address is the window's base
static int mmu_valloc(int np) {
    int i, run, start, n;
    i = mmu_vnext; run = 0; start = i; n = 0;
    while (n < MMU_WINPAGES + np) {
        if (i >= MMU_WINPAGES) { i = 0; run = 0; start = 0; }
        if (mmu_vbits[i >> 5] & (1 << (i & 31))) { run = 0; start = i + 1; }
        else if (++run == np) {
            i = start;
            while (i < start + np) { mmu_vbits[i >> 5] = mmu_vbits[i >> 5] | (1 << (i & 31)); ++i; }
            mmu_vnext = start + np;
            return mmu_uoff + start * MMU_PAGE;
        }
        ++i;
        ++n;
    }
    return 0;
}

static void mmu_vfree(int va, int np) {
    int i;
    i = (va - mmu_uoff) >> 12;
    while (np > 0) { mmu_vbits[i >> 5] = mmu_vbits[i >> 5] & ~(1 << (i & 31)); ++i; --np; }
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
    int guard;                 // its first page is a stack's guard: never mapped
    int code;                  // an image's code: read-only, but a C4KE program's (mmu_fault)
    int va;                    // its user address (base's, with no window)
    int swapped;               // put out to the host's swap (its pages' slots in the kernel's map): base and raw are 0
    int *peer[2];              // spaces it is lent to, read-only (SYS_MAPFROM)
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

static void mmu_setvpg(int va, int n, struct urgn *v) {
    int k;
    k = 0;
    while (k < n) { mmu_vpg[((va - mmu_uoff) >> 12) + k] = (int)v; ++k; }
}

// The region at P (in memory, or at its user address), or 0
static struct urgn *mmu_rgn(int p) {
    if (mmu_uoff && p >= mmu_uoff) {
        if (p - mmu_uoff >= MMU_WINPAGES * MMU_PAGE) return 0;
        return (struct urgn *)mmu_vpg[(p - mmu_uoff) >> 12];
    }
    if (p < MMU_PAGE || p >= mmu_memsize) return 0;
    return (struct urgn *)mmu_pg[p >> 12];
}

// How far into R the address P is, memory's or user mode's
static int mmu_off(struct urgn *r, int p) {
    return mmu_uoff && p >= mmu_uoff ? p - r->va : p - r->base;
}

// A region's memory as user mode sees it, and a user address as memory has it
int mmu_uva(int p) {
    struct urgn *r;
    if (!mmu_uoff || !p || p >= mmu_uoff || !(r = mmu_rgn(p))) return p;
    return r->va + (p - r->base);
}

int mmu_phys(int p) {
    struct urgn *r;
    if (!mmu_uoff || p < mmu_uoff) return p;
    if ((r = mmu_rgn(p)) && !r->swapped) return r->base + (p - r->va);
    return p - mmu_uoff;
}

// R in the kernel's map, or out of it
static void mmu_kmap(struct urgn *r, int on) {
    if (mmu_uoff) mmu_map(mmu_kdir, r->va, r->base, r->npages * MMU_PAGE, on ? PTE_P | PTE_U | PTE_W : 0);
}

// What a region is called by its users: its base, past its guard page
static int mmu_start(struct urgn *r) {
    return r->base + (r->guard ? MMU_PAGE : 0);
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
static int mmu_slotva[8], mmu_slotpa[8];

static int *mmu_dir_of(struct task *t) {
    int *d;
    int k;
    t = mmu_root(t);
    if (!t || t->privs != PRIV_USER) return mmu_dir;
    if (t->mmdir) return (int *)t->mmdir;
    if (!(d = (int *)mmu_page())) return mmu_dir;
    k = 0;
    while (k < mmu_nslotpages) { mmu_map(d, mmu_slotva[k], mmu_slotpa[k], MMU_PAGE, PTE_P | PTE_U); ++k; }
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
    // A page went to swap from a space no CPU was running, perhaps this
    // one, which this CPU may still hold translations of: the TLB's
    // shootdown, at the one moment it matters, before the space runs
    else if (mmu_gen && mmu_seen[cpu_self->id] != mmu_gen) *(int *)MMU_FLUSH = 1;
    if (mmu_gen) mmu_seen[cpu_self->id] = mmu_gen;
    d = MMU_IOPM_USER | (mmu_root(t)->iopm & IOPM_DEV);
    if (*(int *)MMU_IOPM != d) *(int *)MMU_IOPM = d;
}

// SYS_IOPERM: c4bb's device registers for T (bits, IOPM_DEV), as Linux's
// ioperm gives a process its ports. persist asks, to write drive 1.
int mmu_ioperm(struct task *t, int bits) {
    t = mmu_root(t);
    if (!t || t->privs != PRIV_USER) return -1;
    t->iopm = bits & IOPM_DEV;
    if (mmu_on) mmu_switch(t);
    return 0;
}

static int mmu_raw(int np, struct urgn *keep);

// SYS_MAPFROM: N bytes at VA, the memory of a task T started (or one its
// tasks did), readable in T's space too, at the same address -- user
// addresses are the machine's, so it is free there. What a compositor
// needs: the desktop draws a window's framebuffer, the program's own
// memory, by handing the display that address (GUI_BLIT8), which the
// display checks against the desktop's space. The pages are pinned, so
// they stay where they are, and leave T's space when their owner frees
// them (mmu_release). Without the MMU there is nothing to do.
static int mmu_ancestor(struct task *a, struct task *t) {
    int n;
    n = 0;
    while (t && t != a && n < 64) { t = t->parent ? task_get(t->parent) : 0; ++n; }
    return t == a;
}

static void mmu_pin(struct urgn *r);

// The chunk slab S was cut from
static struct urgn *mmu_chunkof(struct urgn *s) {
    struct urgn *c;
    c = s->owner ? (struct urgn *)s->owner->urgns : 0;
    while (c && !(c->kind == UR_CHUNK && s->va >= c->va && s->va < c->va + c->npages * MMU_PAGE)) c = c->next;
    return c;
}
static int mmu_swapin(struct urgn *r);

int mmu_mapfrom(struct task *t, int va, int n) {
    struct urgn *r;
    int *d;
    int end, k;
    if (!mmu_on) return 0;
    t = mmu_root(t);
    if (!t || t->privs != PRIV_USER || n <= 0 || !mmu_uoff) return -1;
    d = mmu_dir_of(t);
    end = va + n;
    va = va & ~(MMU_PAGE - 1);
    while (va < end) {
        // a small block's page is its chunk's, which is what is lent
        if ((r = mmu_rgn(va)) && r->kind == UR_SLAB) r = mmu_chunkof(r);
        if (!r || !r->owner || r->owner == t || !mmu_ancestor(t, r->owner) || !r->dir) return -1;
        if (r->swapped && !mmu_swapin(r)) return -1;
        if (r->peer[0] != d && r->peer[1] != d) {
            k = r->peer[0] ? 1 : 0;
            if (r->peer[k]) return -1;           // lent to two already
            r->peer[k] = d;
            mmu_pin(r);
            mmu_map(d, r->va, r->base, r->npages * MMU_PAGE, PTE_P | PTE_U);
        }
        va = r->va + r->npages * MMU_PAGE;
    }
    return 0;
}

// A region of N bytes, unowned and unmapped: the kernel's until adopted.
// Its user address is its own, in the kernel's map from the start.
static struct urgn *mmu_region(int n, int kind) {
    struct urgn *r;
    int raw, np;
    np = (n + MMU_PAGE - 1) / MMU_PAGE;
    if (np < 1) np = 1;
    if (!(r = (struct urgn *)sl4b_alloc(mmu_rcache))) return 0;
    if (!(raw = mmu_raw(np, 0))) { sl4b_free(mmu_rcache, (char *)r); return 0; }
    r->kind = kind;
    r->raw = raw;
    r->base = (raw + MMU_PAGE - 1) & ~(MMU_PAGE - 1);
    r->npages = np;
    r->va = r->base;
    klock_take(mmu_ulock);
    if (mmu_uoff && !(r->va = mmu_valloc(np))) {
        klock_drop(mmu_ulock);
        free((char *)raw);
        sl4b_free(mmu_rcache, (char *)r);
        return 0;
    }
    mmu_setpg(r, r->base, np, r);
    if (mmu_uoff) mmu_setvpg(r->va, np, r);
    klock_drop(mmu_ulock);
    mmu_kmap(r, 1);
    return r;
}

// Map R into its owner's space, or take it out of the space it is in
static void mmu_rmap(struct urgn *r, int flags) {
    if (flags) r->dir = mmu_dir_of(r->owner);
    if (r->dir) mmu_map(r->dir, r->va, r->base, r->npages * MMU_PAGE, flags);
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
    // a stack's guard page: below it, not mapped, so running off the end
    // is a fault where it happens, not a write over whatever is below
    if (r->guard) mmu_map(r->dir, r->va, r->base, MMU_PAGE, 0);
}

static void mmu_swapdrop(struct urgn *r);

// Off its owner's list, out of its space, back to the heap
static void mmu_release(struct urgn *r) {
    struct urgn **pp;
    int *ring;
    int k;
    // out of the spaces it was lent to, which forget it before they next run
    k = 0;
    while (k < 2) {
        if (r->peer[k]) { mmu_map(r->peer[k], r->va, r->base, r->npages * MMU_PAGE, 0); r->peer[k] = 0; ++mmu_gen; }
        ++k;
    }
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
        if (*ring && mmu_rgn(*ring) == r) *ring = 0;
    }
    klock_take(mmu_ulock);
    if (r->kind != UR_SLAB) {
        if (!r->swapped) mmu_setpg(r, r->base, r->npages, 0);
        if (mmu_uoff) { mmu_setvpg(r->va, r->npages, 0); mmu_vfree(r->va, r->npages); }
    }
    klock_drop(mmu_ulock);
    if (r->kind != UR_SLAB && !r->swapped) mmu_kmap(r, 0);
    if (r->swapped) mmu_swapdrop(r);
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

// N bytes of stack, with a guard page below them that is never mapped
int umalloc_stack(int n) {
    struct urgn *r;
    if (!mmu_on) return (int)malloc(n);
    if (!(r = mmu_region(n + MMU_PAGE, UR_BLOCK))) return 0;
    r->guard = 1;
    return r->base + MMU_PAGE;
}

// A umalloc'd region given back, or a kernel block
void ufree(int p) {
    struct urgn *r;
    if (!p) return;
    if (!mmu_on) { kmem_free((char *)p); return; }
    if ((r = mmu_rgn(p)) && r->kind == UR_BLOCK && mmu_off(r, p) == (r->guard ? MMU_PAGE : 0)) {
        if (!r->shared) mmu_release(r);
        return;
    }
    // not the MMU's: the heap's, or the kernel arena's (an idle task's stack)
    kmem_free((char *)p);
}

// A cached library's code: in every space that loads the library
// (mmu_adopt maps it), and never freed
void umem_share(int p) {
    struct urgn *r;
    if (!mmu_on || !(r = mmu_rgn(p))) return;
    r->shared = 1;
    r->flags = PTE_P | PTE_U | PTE_X;
}

// P is an image's code (mmu_fault)
static void mmu_code(int p) {
    struct urgn *r;
    if (p && (r = mmu_rgn(p)) && !mmu_off(r, p)) r->code = 1;
}

// Give P, if it is an unowned region, to T, mapped with FLAGS; a shared
// one is mapped into T's space as it is everyone's
static void mmu_take(struct task *t, int p, int flags) {
    struct urgn *r;
    if (!p || !(r = mmu_rgn(p)) || r->kind != UR_BLOCK || mmu_off(r, p) != (r->guard ? MMU_PAGE : 0)) return;
    if (r->shared) { mmu_map(mmu_dir_of(t), r->va, r->base, r->npages * MMU_PAGE, r->flags); return; }
    if (r->owner) return;
    mmu_give(r, t, flags);
}

// What the kernel made for T while it was being made, now T's: its stack,
// its image and its libraries' data, its arguments and environment.
// Called when T becomes a user task (task_spawn_priv), when a vfork child
// is made, and after execve.
void mmu_adopt(struct task *t) {
    int *l, *x;
    int k, code;
    if (!mmu_on || !t || t->privs != PRIV_USER) return;
    // A frame forged before it was a user task (sched_forge) has memory's
    // addresses in it -- its stack, its argv, its entry -- which user mode
    // is about to use: its own addresses for them
    if (t->sv == SV_FRAME && t->sv_sp && t->sv_sp < mmu_uoff) {
        x = (int *)t->sv_sp;
        x[3] = mmu_uva(x[3]);
        x[5] = mmu_uva(x[5]);
        t->sv_sp = mmu_uva(t->sv_sp);
    }
    // code may be run and read, not written; data may not be run
    code = PTE_P | PTE_U | PTE_X;
    mmu_code(t->img_code);
    if (t->img_libs) {
        l = (int *)t->img_libs;
        k = 0;
        while (k < t->img_nlibs) { mmu_code(l[3 * k]); ++k; }
    }
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
    s->va = c->va + (s->base - c->base);
    s->npages = 1;
    s->owner = t;
    s->size = 16 << k;
    s->nfree = MMU_PAGE / s->size;
    --c->left;
    klock_take(mmu_ulock);
    mmu_pg[s->base >> 12] = (int)s;
    if (mmu_uoff) mmu_vpg[(s->va - mmu_uoff) >> 12] = (int)s;
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
        return mmu_uva(r->base);
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
    return mmu_uva(s->base + i * s->size);
}

// P from T's blocks back; anything that is not one of them is left alone
// (a task cannot free what is not its own)
void umem_free(struct task *t, int p) {
    struct urgn *r;
    int i;
    t = mmu_root(t);
    if (!p || !(r = mmu_rgn(p)) || r->owner != t) return;
    if (r->kind == UR_SLAB) {
        i = mmu_off(r, p) / r->size;
        if (i * r->size != mmu_off(r, p)) return;
        klock_take(mmu_ulock);
        if (r->used[i >> 5] & (1 << (i & 31))) {
            r->used[i >> 5] = r->used[i >> 5] & ~(1 << (i & 31));
            ++r->nfree;
        }
        klock_drop(mmu_ulock);
        return;
    }
    if (r->kind == UR_BLOCK && !mmu_off(r, p)) mmu_release(r);
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
    *w = OPC_JMP; w[1] = mmu_uva((int)w); w = w + 2;
    r->heap = 1;
    mmu_give(r, t, PTE_P | PTE_U | PTE_X);
    t->sv_pc = mmu_uva(r->base);
    return 1;
}

// ---- the shared libraries' data slots ----

// A new page of slots, into every space there is
static void mmu_slotall(int va, int pa) {
    struct task *t;
    t = task_first();
    while (t) {
        if (t->mmdir) mmu_map((int *)t->mmdir, va, pa, MMU_PAGE, PTE_P | PTE_U);
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
        mmu_slotva[mmu_nslotpages] = r->va;
        mmu_slotpa[mmu_nslotpages] = r->base;
        ++mmu_nslotpages;
        mmu_slotall(r->va, r->base);
    }
    ++mmu_nslots;
    return mmu_slotpage + (mmu_nslots - 1) * sizeof(int);
}

static int mmu_fetch(struct task *t, int va);

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
    if (p < mmu_uoff + MMU_PAGE || end < p || end > mmu_uoff + (mmu_uoff ? MMU_WINPAGES * MMU_PAGE : mmu_memsize)) return 0;
    need = PTE_P | PTE_U | (w ? PTE_W : 0);
    va = p & ~(MMU_PAGE - 1);
    while (va < end) {
        if ((!(e = mmu_pte(mmu_dir_of(t), va, 0)) || (*e & need) != need) &&
            (!mmu_fetch(t, va) || !(e = mmu_pte(mmu_dir_of(t), va, 0)) || (*e & need) != need)) return 0;
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
            if ((int)s + n < mmu_uoff + MMU_PAGE ||
                (int)s + n >= mmu_uoff + (mmu_uoff ? MMU_WINPAGES * MMU_PAGE : mmu_memsize)) return -1;
            if ((!(e = mmu_pte(mmu_dir_of(t), va, 0)) || (*e & (PTE_P | PTE_U)) != (PTE_P | PTE_U)) &&
                (!mmu_fetch(t, va) || !(e = mmu_pte(mmu_dir_of(t), va, 0)) ||
                 (*e & (PTE_P | PTE_U)) != (PTE_P | PTE_U))) return -1;
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
    // Page 0: the display, the sound card, and the CPU-local window to
    // read (the libraries' data slots) but not to write: it is the
    // kernel's struct cpu. Not the ROM, so that a null pointer's read is a
    // fault (the TLEV word there still runs). c4bb's devices only for a
    // task that asks (SYS_IOPERM). docs/c4ix-mmu.md 7.
    *(int *)MMU_IOPM = MMU_IOPM_USER;
    *(int *)MMU_UWIN = mmu_uoff;
    *(int *)MMU_UDIR = (int)mmu_kdir;
    *(int *)MMU_CTL = 1;
    klog("mmu: CPU %d translating user mode, its traps on a %d KB stack at 0x%08x\n",
        k, MMU_KSTACK / 1024, s);
}

static void mmu_swapinit();

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
    // user addresses from 256 MB, where memory fits below them: the
    // window's books and the kernel's map of it
    if (mmu_memsize <= MMU_UWIN_BASE) {
        if (!(mmu_vpg = (int *)malloc(MMU_WINPAGES * sizeof(int))) ||
            !(mmu_vbits = (int *)malloc(MMU_WINPAGES / 8)) ||
            !(mmu_kdir = (int *)mmu_page())) {
            klog("mmu: no memory for its tables; left off\n");
            return;
        }
        memset((char *)mmu_vpg, 0, MMU_WINPAGES * sizeof(int));
        memset((char *)mmu_vbits, 0, MMU_WINPAGES / 8);
        mmu_vbits[0] = 1;
        mmu_vnext = 1;
        mmu_uoff = MMU_UWIN_BASE;
    }
    mmu_on = 1;
    klog("mmu: 4 KB pages, %d of them; each user task has a space of its own, its own pages and the shared code\n",
        mmu_npages);
    if (mmu_uoff) klog("mmu: user addresses are 256 MB from 0x%08x, none of them the kernel's or physical\n", mmu_uoff);
    else klog("mmu: memory reaches 0x%08x: user addresses are physical ones\n", MMU_UWIN_BASE);
    mmu_cpu(0);
    mmu_swapinit();
}

// ---- faults ----

// Whose memory VA is, for the fault's message
static void mmu_whose(int va, char *buf) {
    struct urgn *r;
    char *s;
    int i;
    r = mmu_rgn(va);
    if (va < MMU_PAGE) s = "page 0";
    else if (va >= mmu_memsize && (!mmu_uoff || va < mmu_uoff || va >= mmu_uoff + MMU_WINPAGES * MMU_PAGE)) s = "beyond memory";
    else if (mmu_uoff && va >= mmu_uoff && !r) s = "nobody's";
    else if (r && r->shared) s = "shared library code";
    else if (r && r->owner && r->guard && mmu_off(r, va) < MMU_PAGE) {
        s = "the stack guard of task ";
        i = 0;
        while (*s) { buf[i] = *s; ++s; ++i; }
        s = r->owner->name;
        while (*s && i < 44) { buf[i] = *s; ++s; ++i; }
        buf[i] = 0;
        return;
    }
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
    else if (mmu_phys(va) < *(int *)C4IX_HEAP_BASE) s = "the kernel's image";
    else s = "the kernel's heap";
    i = 0;
    while (*s) { buf[i] = *s; ++s; ++i; }
    buf[i] = 0;
}

// A user task took a page fault (TRAP_SEGV): its memory back from swap,
// or say what it was, and kill it
void mmu_fault(struct task *t, int va) {
    int info, pc;
    char *what;
    char whose[64];
    struct urgn *r;
    int *e;
    int k;
    info = *(int *)MMU_FINFO;
    pc = *(int *)MMU_FPC;
    // Its own memory, on swap: back in, and the instruction runs again
    // (faults are precise). With no room for it even after putting out
    // others, the task cannot go on.
    if (t && (r = mmu_rgn(va)) && r->swapped && r->owner == mmu_root(t)) {
        mmu_selfok = 1;
        k = mmu_swapin(r);
        mmu_selfok = 0;
        if (k) return;
        kprintf("c4ix: task %d (%s): its memory at 0x%08x is on swap, and there is no room for it; killed\n",
            t->id, t->name, va);
        t->killsig = C4IX_SIGSEGV;
        task_exit(-1);
        return;
    }
    // A C4KE program writing its own code: C4KE's ps patches itself. The
    // page it writes is made writable, that page alone, and the store
    // runs again (faults are precise: the trap returns to the
    // instruction); the rest of its text stays read-only. C4IX's own
    // programs never do this, and are killed for it below.
    if (t && t->ckabi && (info & MMU_FK_WRITE) && !(info & MMU_FK_DMA) && (r = mmu_rgn(va)) && r->code &&
        r->owner == mmu_root(t) && r->dir && (e = mmu_pte(r->dir, va, 0)) &&
        (*e & (PTE_P | PTE_U)) == (PTE_P | PTE_U) && !(*e & PTE_W)) {
        *e = *e | PTE_W;
        *(int *)MMU_INVLPG = va;
        klog("mmu: task %d (%s), a C4KE program, writes its own code: the page at 0x%08x made writable\n",
            t->id, t->name, va & ~(MMU_PAGE - 1));
        return;
    }
    if (info & MMU_FK_DMA) what = (info & MMU_FK_WRITE) ? "a device's write to" : "a device's read of";
    else what = (info & MMU_FK_WRITE) ? "write to" : (info & MMU_FK_FETCH) ? "jump to" : "read of";
    mmu_whose(va, whose);
    kprintf("c4ix: task %d (%s): page fault: %s 0x%08x (%s, %s) at pc 0x%08x; killed\n",
        t ? t->id : -1, t ? t->name : "?", what, va, whose,
        (info & MMU_FK_PRESENT) ? "not allowed" : "not mapped", pc);
    if (t) t->killsig = C4IX_SIGSEGV;
    task_exit(-1);
}

// ---- swap ----
//
// When the heap has no block for a region, the blocks of idle tasks are
// put out to the host's swap, a page to a slot, and their memory goes back
// to the heap; a task that touches one again takes a page fault, and it
// comes back wherever there is room (mmu_fault), at the same user address.
// The host keeps the pages (libjs, devices.js SWAP_*: memory beyond the
// arena, forgotten with the machine) behind registers of their own, so
// nothing a task does with the drives comes between swap's stores, and
// nothing is left behind on any drive.
//
// A page put out stays in the kernel's map, not present, with its slot in
// the entry (PTE_SWAP): the kernel, which might follow a pointer into it
// that it did not check first, reads and writes the host's copy there
// (libjs's user window does), and never lands on whatever has the page's
// old memory now.
//
// Which go: a block a task asked for (umem_alloc), not its image, stack or
// arguments, of a task no CPU is running and that is not the one asking --
// every task not running is at the edge of user mode, since a syscall that
// waits is run again when its task wakes, and checks its pointers again
// (uaccess brings back what they name). Not a block the kernel or a device
// has used (PTE_PIN, which the host sets in the kernel's map): the
// display's framebuffer and rings, what the kernel copied in and out.
// Blocks whose pages nobody has used since the last look go first (PTE_A).
enum { SWAP_ADDR = 0x11c, SWAP_CMD = 0x158, SWAP_OUT = 1, SWAP_IN = 2, SWAP_DROP = 3, SWAP_RESET = 4 };
static int mmu_swapok;         // the host keeps pages
static int mmu_swapslots, mmu_sfree;   // its slots, and how many are free
static int *mmu_sbits;         // slots in use, a bit each
static int mmu_snext;
static int mmu_swapouts, mmu_swapins;

// One page through the host's swap: OP on SLOT, at PA in memory
static int mmu_sop(int op, int slot, int pa) {
    *(int *)SWAP_ADDR = pa;
    *(int *)SWAP_CMD = op | (slot << 4);
    return *(int *)SWAP_CMD;
}

static int mmu_salloc() {
    int i, n;
    i = mmu_snext; n = 0;
    while (n < mmu_swapslots) {
        if (i >= mmu_swapslots) i = 0;
        if (!(mmu_sbits[i >> 5] & (1 << (i & 31)))) {
            mmu_sbits[i >> 5] = mmu_sbits[i >> 5] | (1 << (i & 31));
            mmu_snext = i + 1;
            --mmu_sfree;
            return i;
        }
        ++i; ++n;
    }
    return -1;
}

static void mmu_sput(int s) {
    mmu_sbits[s >> 5] = mmu_sbits[s >> 5] & ~(1 << (s & 31));
    ++mmu_sfree;
}

static void mmu_swapinit() {
    if (!mmu_uoff || !host_has(C4IX_I_C4MJS)) {
        klog("swap: none (%s)\n", mmu_uoff ? "the host keeps no pages" : "user addresses are physical");
        return;
    }
    // what the host keeps, emptied: a new boot's swap is empty
    *(int *)SWAP_CMD = SWAP_RESET;
    if ((mmu_swapslots = *(int *)SWAP_CMD) <= 0 || !(mmu_sbits = (int *)malloc((mmu_swapslots + 31) / 32 * sizeof(int)))) {
        klog("swap: none (the host keeps no pages)\n");
        return;
    }
    memset((char *)mmu_sbits, 0, (mmu_swapslots + 31) / 32 * sizeof(int));
    mmu_sfree = mmu_swapslots;
    mmu_swapok = 1;
    klog("swap: the host keeps up to %d MB of pages put out\n", mmu_swapslots / 256);
}

// R's pages: each one's slot, from the kernel's map, given back
static void mmu_swapdrop(struct urgn *r) {
    int *e;
    int k;
    k = 0;
    while (k < r->npages) {
        if ((e = mmu_pte(mmu_kdir, r->va + k * MMU_PAGE, 0)) && (*e & PTE_SWAP)) {
            mmu_sop(SWAP_DROP, *e >> 12 & 0xfffff, 0);
            mmu_sput(*e >> 12 & 0xfffff);
            *e = 0;
        }
        ++k;
    }
}

// Has the host pinned any page of R (the kernel or a device used it)?
// Has user mode used any since the last look (and forget that it did)?
static int mmu_pinned(struct urgn *r) {
    int *e;
    int k;
    k = 0;
    while (k < r->npages) {
        if ((e = mmu_pte(mmu_kdir, r->va + k * MMU_PAGE, 0)) && (*e & PTE_PIN)) return 1;
        ++k;
    }
    return 0;
}

static int mmu_used(struct urgn *r) {
    int *e;
    int k, used;
    used = 0;
    k = 0;
    while (k < r->npages && r->dir) {
        if ((e = mmu_pte(r->dir, r->va + k * MMU_PAGE, 0)) && (*e & PTE_A)) { *e = *e & ~PTE_A; used = 1; }
        ++k;
    }
    return used;
}

// R's pages out to the host, and its memory back to the heap
static int mmu_swapout(struct urgn *r) {
    int *e;
    int len, k, s;
    len = r->npages * MMU_PAGE;
    if (mmu_sfree < r->npages) return 0;
    // out of its space -- no CPU is running it, and each that has it
    // loaded forgets it before it runs it again (mmu_gen)
    if (r->dir) mmu_map(r->dir, r->va, r->base, len, 0);
    ++mmu_gen;
    // each page to a slot, the slot in the kernel's map
    k = 0;
    while (k < r->npages) {
        s = mmu_salloc();
        if (mmu_sop(SWAP_OUT, s, r->base + k * MMU_PAGE) < 0) {
            klog("swap: the host would not keep a page; swap off\n");
            mmu_swapok = 0;
        }
        e = mmu_pte(mmu_kdir, r->va + k * MMU_PAGE, 1);
        *e = (s << 12) | PTE_SWAP;
        ++k;
    }
    klock_take(mmu_ulock);
    mmu_setpg(r, r->base, r->npages, 0);
    klock_drop(mmu_ulock);
    free((char *)r->raw);
    r->raw = 0;
    r->base = 0;
    r->swapped = 1;
    ++mmu_swapouts;
    klog("swap: %d KB of task %d (%s) at 0x%08x out\n", len / 1024, r->owner->id, r->owner->name, r->va);
    return 1;
}

// R back from the host, wherever there is room, at its user address
static int mmu_swapin(struct urgn *r) {
    int *e;
    int raw, base, len, k, s;
    len = r->npages * MMU_PAGE;
    if (!(raw = mmu_raw(r->npages, r))) return 0;
    base = (raw + MMU_PAGE - 1) & ~(MMU_PAGE - 1);
    k = 0;
    while (k < r->npages) {
        e = mmu_pte(mmu_kdir, r->va + k * MMU_PAGE, 0);
        s = *e >> 12 & 0xfffff;
        mmu_sop(SWAP_IN, s, base + k * MMU_PAGE);
        mmu_sput(s);
        ++k;
    }
    r->raw = raw;
    r->base = base;
    r->swapped = 0;
    klock_take(mmu_ulock);
    mmu_setpg(r, base, r->npages, r);
    klock_drop(mmu_ulock);
    mmu_kmap(r, 1);
    if (r->dir) mmu_map(r->dir, r->va, base, len, r->flags);
    ++mmu_swapins;
    klog("swap: %d KB of task %d (%s) at 0x%08x back in\n", len / 1024, r->owner->id, r->owner->name, r->va);
    return 1;
}

// A page fault or a syscall's pointer at VA, T's own memory on swap: back
static int mmu_fetch(struct task *t, int va) {
    struct urgn *r;
    if (!(r = mmu_rgn(va)) || !r->swapped || r->owner != mmu_root(t)) return 0;
    return mmu_swapin(r);
}

// The largest block that may go, of those nobody has used since the last
// look (PASS 0), or of any (PASS 1); 0 if none. Never KEEP. The asking
// task's own only for a page fault (mmu_selfok): one that brings back one
// of its blocks may put out another, which it can fault back in turn,
// where a syscall's pointers must all stay in.
static struct urgn *mmu_victim(int pass, struct urgn *keep) {
    struct task *t, *me;
    struct urgn *r, *best;
    me = mmu_root(sched_current());
    best = 0;
    t = task_first();
    while (t) {
        if (t->privs == PRIV_USER && !t->vfparent && !t->cpuidle && t->state != TS_ZOMBIE && t->state != TS_VFORK &&
            (t == me ? mmu_selfok : !t->oncpu)) {
            r = (struct urgn *)t->urgns;
            while (r) {
                if (r->kind == UR_BLOCK && r->heap && !r->shared && !r->guard && !r->swapped && !r->code &&
                    r != keep && r->dir && r->npages <= mmu_sfree && (!best || r->npages > best->npages) &&
                    !mmu_pinned(r) && (pass || !mmu_used(r))) best = r;
                r = r->next;
            }
        }
        t = t->next;
    }
    return best;
}

// Room for a block of NP pages, by putting blocks out: whether any went
static int mmu_reclaim(int np, struct urgn *keep) {
    struct urgn *r;
    if (!mmu_swapok || !sched_has_bkl()) return 0;
    if (!(r = mmu_victim(0, keep)) && !(r = mmu_victim(1, keep))) return 0;
    return mmu_swapout(r);
}

// A heap block for NP pages, putting blocks out to make room while there
// are any; and whether memory is short now (mmu_short: allocation takes
// the kernel lock, where swap may run)
static int mmu_raw(int np, struct urgn *keep) {
    int raw, used, size;
    while (!(raw = (int)malloc(np * MMU_PAGE + MMU_PAGE - 1))) {
        if (!mmu_reclaim(np, keep)) return 0;
    }
    if (mmu_swapok) {
        used = *(int *)C4IX_HEAP_USED;
        size = *(int *)C4IX_HEAP_END - *(int *)C4IX_HEAP_BASE;
        mmu_short = used > size / 8 * 7;
    }
    return raw;
}

// R's pages pinned in the kernel's map: swap leaves them where they are
static void mmu_pin(struct urgn *r) {
    int *e;
    int k;
    if (!mmu_kdir) return;
    k = 0;
    while (k < r->npages) {
        if ((e = mmu_pte(mmu_kdir, r->va + k * MMU_PAGE, 0)) && (*e & PTE_P)) *e = *e | PTE_PIN;
        ++k;
    }
}
