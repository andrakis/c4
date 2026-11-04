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
// none of them is the kernel's, and memory can move under them: a block
// a task asks for has no memory until it is touched, and swap (below)
// puts the pages nobody has used lately out to the host and brings them
// back wherever there is room. The kernel's map (mmu_kdir) maps every user
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
// The TLB's shootdown by IPI (mmu_shootdown): each CPU's loaded space,
// whether it is anywhere but user mode, and whether it has been asked to
// forget its translations
static int mmu_loaded[CPU_MAX];
int mmu_inkernel[CPU_MAX], mmu_shoot[CPU_MAX];
static int mmu_shootdowns, mmu_shootlater;
static int *mmu_fc;            // a page of memory's frame block (stage 9), by page number
static int *mmu_fva;           // ... and the user address using it, 0 for none (a frame block freed whole, mmu_reclaim)
static int mmu_hand;           // swap's clock: the window's page it looks at next (mmu_clock)
static int mmu_flock[2];       // the frame blocks and swap's slots (klock: SMP's light path frees too)
enum { SWAP_AHEAD = 16 };      // pages a fault brings back: its own, and those after it that went out
enum { PTE_ZERO = 0x800 };     // the kernel's map: a page out that was never in, its slot never written (mmu_lazy)
enum { MMU_MINROOM = 1048576 };   // memory on demand: the least room to page it in (mmu_lazy)
int mmu_pageroom();
int mmu_swapok;                // the host keeps pages
static int mmu_swapslots, mmu_sfree;   // its slots, and how many are free
static int mmu_swapouts, mmu_swapins, mmu_moves, mmu_firsts;
// load control (mmu_balance): the cycles spent bringing pages back since
// the window's start, that start in machine ms, and the tasks held
static int mmu_pgcyc, mmu_nheld;
static int mmu_nofnew;         // mmu_fnew found the heap had no block for frames
int mmu_lwin;
static int mmu_salloc();
static void mmu_fvaset(int pa, int va, int np);
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
// the aligned 15 or 16 inside are used, on a list threaded through
// their first word, where freed ones go too. Where the heap has no run
// of 16 pages -- its memory frames, in pieces between the kernel's
// blocks -- a block of two, the aligned page in it; and where there is
// not even that, a page coming back makes one by putting pages out
// (mmu_pte_out). A table is what a page coming back needs to be mapped
// in its task's space: 40 c4m's in 32 MB had pages come back unmapped,
// and died of it ("page fault ... not mapped").
//
// A block whose pages are all on the list again goes back to the heap.
// Kept, the blocks a crowd of programs' tables took -- from wherever the
// heap had a run, the middle of it when frames filled the rest -- cut its
// free memory into pieces once the programs had gone: after innerbench
// -Tln 40, twenty of them from 7 MB to 23 MB, and nothing more than 5 MB
// in one run. (The kernel's map's tables, never freed, have blocks of
// their own: mmu_kpage.)
enum { MMU_CHUNK = 16 };
static int mmu_freepage;
struct tchunk {
    int raw;                   // the heap's block
    int n;                     // its pages
    int nfree;                 // ... of them on the list
};
static int *mmu_tc;            // a page of memory's block of table pages (struct tchunk), by page number

static int mmu_ulock[2];       // the regions, the map and the page list (klock: SMP's light path)

// The pages of the heap block RAW, N pages long, to the list
static void mmu_pages_add(int raw, int n) {
    struct tchunk *c;
    int p;
    // (no record, no lookup: the block is never freed)
    c = mmu_tc ? (struct tchunk *)malloc(sizeof(struct tchunk)) : 0;
    if (c) { c->raw = raw; c->n = 0; c->nfree = 0; }
    klock_take(mmu_ulock);
    p = (raw + MMU_PAGE - 1) & ~(MMU_PAGE - 1);
    while (p + MMU_PAGE <= raw + n * MMU_PAGE) {
        *(int *)p = mmu_freepage;
        mmu_freepage = p;
        if (c) { mmu_tc[p >> 12] = (int)c; ++c->n; ++c->nfree; }
        p = p + MMU_PAGE;
    }
    klock_drop(mmu_ulock);
}

static int mmu_page() {
    struct tchunk *c;
    int p, raw;
    klock_take(mmu_ulock);
    while (!mmu_freepage) {
        klock_drop(mmu_ulock);
        if ((raw = (int)malloc(MMU_CHUNK * MMU_PAGE))) mmu_pages_add(raw, MMU_CHUNK);
        else if ((raw = (int)malloc(2 * MMU_PAGE))) mmu_pages_add(raw, 2);
        else return 0;
        klock_take(mmu_ulock);
    }
    p = mmu_freepage;
    mmu_freepage = *(int *)p;
    if (mmu_tc && (c = (struct tchunk *)mmu_tc[p >> 12])) --c->nfree;
    klock_drop(mmu_ulock);
    memset((char *)p, 0, MMU_PAGE);
    return p;
}

// A table's page P back to the list, under mmu_ulock; its block, every
// page of it there, back to the heap
static void mmu_page_free(int p) {
    struct tchunk *c;
    int *pp;
    *(int *)p = mmu_freepage;
    mmu_freepage = p;
    if (!mmu_tc || !(c = (struct tchunk *)mmu_tc[p >> 12]) || ++c->nfree < c->n) return;
    pp = &mmu_freepage;
    while (*pp) {
        if (mmu_tc[*pp >> 12] == (int)c) { mmu_tc[*pp >> 12] = 0; *pp = *(int *)*pp; }
        else pp = (int *)*pp;
    }
    free((char *)c->raw);
    free((char *)c);
}

// The kernel's map's tables (mmu_kdir), which are never freed, from blocks
// of their own: among the others, each kept its block for good, and the
// heap's memory in pieces round them -- after innerbench -Tln 40, twenty
// blocks of 64 KB from 4 MB to 26 MB, kept by a table of the kernel's
// each. At boot, a block of them for a 128th of memory (a table maps 4 MB
// of the window: in 32 MB, 64, all of it), so that one taken later, from
// the middle of a heap that frames have filled, is rare.
static int mmu_kfreepage;

static int mmu_kfill(int n) {
    int raw, p;
    if (!(raw = (int)malloc(n * MMU_PAGE))) {
        n = 2;
        if (!(raw = (int)malloc(n * MMU_PAGE))) return 0;
    }
    klock_take(mmu_ulock);
    p = (raw + MMU_PAGE - 1) & ~(MMU_PAGE - 1);
    while (p + MMU_PAGE <= raw + n * MMU_PAGE) {
        *(int *)p = mmu_kfreepage;
        mmu_kfreepage = p;
        p = p + MMU_PAGE;
    }
    klock_drop(mmu_ulock);
    return 1;
}

// A page for a table of the kernel's map, zeroed: 0 if there is none
static int mmu_kpage() {
    int p;
    klock_take(mmu_ulock);
    while (!mmu_kfreepage) {
        klock_drop(mmu_ulock);
        if (!mmu_kfill(MMU_CHUNK)) return 0;
        klock_take(mmu_ulock);
    }
    p = mmu_kfreepage;
    mmu_kfreepage = *(int *)p;
    klock_drop(mmu_ulock);
    memset((char *)p, 0, MMU_PAGE);
    return p;
}

// VA's entry in directory DIR, if it has a table for it: 0 if not. D and VA
// are plain variables, read more than once.
#define MMU_PTE0(d, va) (((d)[((va) >> 22) & 1023] & PTE_P) ? (int *)((d)[((va) >> 22) & 1023] & ~(MMU_PAGE - 1)) + (((va) >> 12) & 1023) : (int *)0)

// ... and the table itself, for a loop over its pages
#define MMU_TBL(d, va) (((d)[((va) >> 22) & 1023] & PTE_P) ? (int *)((d)[((va) >> 22) & 1023] & ~(MMU_PAGE - 1)) : (int *)0)

// VA's entry in directory DIR, its table made if MAKE; 0 if there is none
static int *mmu_pte(int *dir, int va, int make) {
    int *pt;
    int pde;
    pde = dir[(va >> 22) & 1023];
    if (!(pde & PTE_P)) {
        if (!make || !(pt = (int *)(dir == mmu_kdir ? mmu_kpage() : mmu_page()))) return 0;
        // Looked at again under the lock: a program's malloc takes no kernel
        // lock (sched_light), and two CPUs' blocks in one 4 MB of the kernel's
        // map each made it a table. The second put in over the first lost
        // what the first had written there: a block's first pages, "not
        // mapped" when they were touched.
        klock_take(mmu_ulock);
        pde = dir[(va >> 22) & 1023];
        if (pde & PTE_P) {
            if (dir == mmu_kdir) { *pt = mmu_kfreepage; mmu_kfreepage = (int)pt; }
            else mmu_page_free((int)pt);
            pt = (int *)(pde & ~(MMU_PAGE - 1));
        } else {
            ++mmu_ntables;
            dir[(va >> 22) & 1023] = (int)pt | PTE_P;
        }
        klock_drop(mmu_ulock);
    } else pt = (int *)(pde & ~(MMU_PAGE - 1));
    return pt + ((va >> 12) & 1023);
}

// ... and where the heap has not even two pages in a run, pages put out
// to make them (mmu_kalloc). Pages move then: only where nothing holds a
// frame across it: a page coming in, before it takes its frame
// (mmu_pagein), and a fault on a page that is in with no table (mmu_fault).
static int *mmu_pte_out(int *dir, int va) {
    int *e;
    int raw;
    if ((e = mmu_pte(dir, va, 1))) return e;
    if (!(raw = (int)mmu_kalloc(2 * MMU_PAGE))) return 0;
    mmu_pages_add(raw, 2);
    return mmu_pte(dir, va, 1);
}

// Map N bytes at user address VA, page by page, to the memory at PA,
// with FLAGS (0 unmaps). This CPU forgets what it had of them; another
// CPU that has the space loaded but is not running it forgets them when
// it next loads it (mmu_gen, mmu_switch); one running it is never asked
// to (docs/c4ix-mmu.md 10).
int mmu_mapgen;                 // bumped after anything is unmapped (uaccess_args)

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
    // after: whoever read it before looking at the tables finds it changed.
    // (Atomic: one lost, a CPU could keep a page another has just unmapped.)
    if (!flags) {
        if (smp_ncpu > 1) __c4_fadd((int)&mmu_mapgen, 1);
        else ++mmu_mapgen;
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
// N + 4095 bytes long -- or, a block a task asks for, none: its pages are
// frames from anywhere, each as it is first touched (mmu_lazy). Its
// bookkeeping is the kernel's, in an SL4B
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
    int swapped;               // its pages put out to the host's swap (their slots in the kernel's map)
    int lazy;                  // no memory of its own: each page a frame of its own from first use (mmu_lazy)
    int canout;                // a block of what its task asked for, mapped: its pages may go out (MMU_RCAN)
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
// ... kept as what it says changes; its task's half is TF_PAGEOK
#define MMU_RCAN(r) ((r)->canout = (r)->kind == UR_BLOCK && (r)->heap && !(r)->shared && !(r)->guard && !(r)->code && (r)->dir && (r)->owner)
#define mmu_canout(r) ((r) && (r)->canout && TF_PAGEOK((r)->owner))
static struct urgn *mmu_lastin;   // the block whose pages last came back (swap, mmu_bringin's log)
static struct urgn *mmu_runr;     // ... and the one whose pages are going out (mmu_out's log)
static int mmu_runn, mmu_runva, mmu_runshot, mmu_runlater, mmu_runon;
static int mmu_runid;             // ... its task, and the task's name, as they were when the run began
static char mmu_runname[32];
static int mmu_quiet;             // reclaim for mmu_kalloc, which the kernel's log itself calls: no log
static void mmu_runlog();
// Swap's lines, SWAP_LINES a second of the machine's clock and the rest
// counted: a build bigger than memory puts pages out at every fault, and
// a 64 KB log of nothing else has lost the line that said why a program
// stopped (umem_alloc's)
enum { SWAP_LINES = 4 };
static int mmu_lsec, mmu_lsaid, mmu_lhush;
static int mmu_say() {
    int s;
    s = __time() / 1000;
    if (s != mmu_lsec) {
        if (mmu_lhush) klog("swap: %d lines more of pages out and back in, not said\n", mmu_lhush);
        mmu_lsec = s;
        mmu_lsaid = 0;
        mmu_lhush = 0;
    }
    if (mmu_lsaid < SWAP_LINES) { ++mmu_lsaid; return 1; }
    ++mmu_lhush;
    return 0;
}

static struct sl4b_cache *mmu_rcache;
static int *mmu_pg;            // a page's region, by page number

static void mmu_setpg(struct urgn *r, int base, int n, struct urgn *v) {
    int k;
    k = 0;
    while (k < n) { mmu_pg[(base >> 12) + k] = (int)v; ++k; }
}

// R's pages of memory no longer its: only where they still say R. Its
// block may have gone back to the heap already -- every page moved or put
// out (mmu_move, mmu_pageout, mmu_contig) -- and be another region's now,
// whose pages these are (mmu_rgn, and so mmu_uva, must find that one)
static void mmu_unsetpg(struct urgn *r) {
    int k;
    k = 0;
    while (k < r->npages) {
        if (mmu_pg[(r->base >> 12) + k] == (int)r) mmu_pg[(r->base >> 12) + k] = 0;
        ++k;
    }
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
    int *e;
    if (!mmu_uoff || p < mmu_uoff) return p;
    if (mmu_kdir && (e = MMU_PTE0(mmu_kdir, p)) && (*e & PTE_P)) return (*e & ~(MMU_PAGE - 1)) | (p & (MMU_PAGE - 1));
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
static struct task *mmu_root_slow(struct task *t) {
    while (t->vfparent) t = t->vfparent;
    return t;
}
#define mmu_root(t) ((t) && (t)->vfparent ? mmu_root_slow(t) : (t))

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

// ... in the common case without a call: not a vfork child, its space made
#define MMU_DIR(t) (!(t)->vfparent && (t)->mmdir ? (int *)(t)->mmdir : mmu_dir_of(t))

// ... and where there is no page for it, pages put out to make one
// (mmu_kalloc), as for a table (mmu_pte_out): what a task becoming a user
// task does (mmu_adopt), before it has frames of its own that would move.
// Without it, a space that could not be made was the kernel's own, empty
// directory, and the task's regions were mapped into that.
static int *mmu_dir_out(struct task *t) {
    int *d;
    int raw;
    if ((d = mmu_dir_of(t)) != mmu_dir || !(t = mmu_root(t)) || t->privs != PRIV_USER) return d;
    if (!(raw = (int)mmu_kalloc(2 * MMU_PAGE))) return d;
    mmu_pages_add(raw, 2);
    return mmu_dir_of(t);
}

// T is done with its space (task_destroy, and execve's old one): its
// tables go back to the page list. What was mapped in it is freed by name.
static void mmu_dir_free(int *d) {
    int k;
    if (!d || d == mmu_dir) return;
    k = 0;
    while (k < 1024) {
        if (d[k] & PTE_P) {
            mmu_page_free(d[k] & ~(MMU_PAGE - 1));
            --mmu_ntables;
        }
        ++k;
    }
    mmu_page_free((int)d);
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
    cpu_self->okva = 0 - 1;
    if (!mmu_on || !t || t->privs != PRIV_USER) return;
    d = (int)MMU_DIR(t);
    mmu_loaded[cpu_self->id] = d;
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

static int mmu_raw(int np);
static int mmu_contig(struct urgn *r);

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
static int mmu_bringall(struct urgn *r);
static int mmu_bringin(struct urgn *r, int va);
static int mmu_isout(int va);
static void mmu_frames_release(struct urgn *r);
static void mmu_release(struct urgn *r);
static int mmu_cnew(int raw, int base, int np, int used);

int mmu_mapfrom(struct task *t, int va, int n) {
    struct urgn *r;
    int *d, *e;
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
        if (r->swapped && !mmu_bringall(r)) return -1;
        if (r->peer[0] != d && r->peer[1] != d) {
            k = r->peer[0] ? 1 : 0;
            if (r->peer[k]) return -1;           // lent to two already
            r->peer[k] = d;
            mmu_pin(r);
            // page by page, wherever each is: one that has been out came
            // back into any frame
            k = 0;
            while (k < r->npages) {
                if ((e = mmu_pte(mmu_kdir, r->va + k * MMU_PAGE, 0)) && (*e & PTE_P))
                    mmu_map(d, r->va + k * MMU_PAGE, *e & ~(MMU_PAGE - 1), MMU_PAGE, PTE_P | PTE_U);
                ++k;
            }
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
    if (!(raw = mmu_raw(np))) { sl4b_free(mmu_rcache, (char *)r); return 0; }
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
    // its frames, the block's record counting them (stage 9: swap moves
    // them a page at a time)
    if (mmu_uoff && !mmu_cnew(raw, r->base, np, 1)) {
        klock_take(mmu_ulock);
        mmu_setpg(r, r->base, np, 0);
        mmu_setvpg(r->va, np, 0);
        mmu_vfree(r->va, np);
        klock_drop(mmu_ulock);
        free((char *)raw);
        sl4b_free(mmu_rcache, (char *)r);
        return 0;
    }
    if (mmu_uoff) mmu_fvaset(r->base, r->va, np);
    mmu_kmap(r, 1);
    return r;
}

// A region of N bytes with no memory yet (umem_alloc's blocks): user
// addresses, and for each page a slot of swap's that the host has never
// written, which is a page of zeros (devices.js swapOp). So the first
// touch is a page fault like any other, and brings the page "back" into
// a frame of its own (mmu_fault, mmu_bringin), from wherever there is
// one; the kernel's reads of a page not touched yet are zeros, and its
// first write makes the host's copy. A task may ask for more than the
// machine has free, up to swap's size: what it uses is what it holds,
// and swap makes the room (mmu_clock). 0 with no window, or no swap.
static struct urgn *mmu_lazy(int n) {
    struct urgn *r;
    int *e;
    int np, k, s;
    np = (n + MMU_PAGE - 1) / MMU_PAGE;
    if (!mmu_uoff || !mmu_swapok || np < 1 || mmu_sfree < np) return 0;
    // not where there is too little to page it in: two programs would put
    // each other's pages out for ever, where a heap block would have
    // failed and said so (a desktop boot in 12 MB went 32 billion cycles
    // without coming up); then the block is a heap block, or nothing
    if (mmu_pageroom() < MMU_MINROOM) return 0;
    if (!(r = (struct urgn *)sl4b_alloc(mmu_rcache))) return 0;
    r->kind = UR_BLOCK;
    r->npages = np;
    r->lazy = 1;
    klock_take(mmu_ulock);
    if (!(r->va = mmu_valloc(np))) { klock_drop(mmu_ulock); sl4b_free(mmu_rcache, (char *)r); return 0; }
    mmu_setvpg(r->va, np, r);
    klock_drop(mmu_ulock);
    k = 0;
    while (k < np) {
        if (!(e = mmu_pte(mmu_kdir, r->va + k * MMU_PAGE, 1)) || (s = mmu_salloc()) < 0) break;
        *e = (s << 12) | PTE_SWAP | PTE_ZERO;
        ++r->swapped;
        ++k;
    }
    if (k < np) { mmu_release(r); return 0; }
    return r;
}

// Map R into its owner's space, or take it out of the space it is in. A
// lazy region's pages are mapped one at a time as they come in.
static void mmu_rmap(struct urgn *r, int flags) {
    if (flags) r->dir = mmu_dir_of(r->owner);
    if (r->dir && (!flags || !r->lazy)) mmu_map(r->dir, r->va, r->base, r->npages * MMU_PAGE, flags);
    if (!flags) r->dir = 0;
    MMU_RCAN(r);
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

// A block of frames ("frames", below)
struct fchunk {
    int raw, base, npages;
    int nused;                 // frames in use
    int onlist;                // on mmu_fcfree
    int *bits;                 // a bit a free frame
    struct fchunk *next;       // the chunks with a free frame
    struct fchunk *anext;      // every chunk (mmu_reclaim)
    int tried;                 // mmu_reclaim's round that found it could not be freed
    int pool;                  // taken for single frames (mmu_fnew), not one region's
};

static int mmu_poolbytes;      // the heap's bytes in blocks of single frames (mmu_fnew)

// Off its owner's list, out of its space, back to the heap
static void mmu_release(struct urgn *r) {
    struct urgn **pp;
    struct fchunk *own;
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
    // its own block of frames, if it still is (every page of it moved or
    // put out, the block went back to the heap: mmu_unsetpg)
    own = 0;
    if (r->kind != UR_SLAB && mmu_uoff && !r->lazy && mmu_pg[r->base >> 12] == (int)r)
        own = (struct fchunk *)mmu_fc[r->base >> 12];
    if (r->kind != UR_SLAB) {
        if (!r->lazy) mmu_unsetpg(r);
        if (mmu_uoff) mmu_setvpg(r->va, r->npages, 0);
    }
    klock_drop(mmu_ulock);
    // its frames back to their blocks (a block goes to the heap when none
    // of its frames is in use), or with no window, its block
    if (r->kind != UR_SLAB) {
        if (mmu_uoff) mmu_frames_release(r);
        else if (r->raw) free((char *)r->raw);
    }
    // Its addresses last, with nothing of it left in the kernel's map. A
    // program's free takes no kernel lock (sched_light): handed out before
    // that, another CPU's malloc had them, and the entries it had just made
    // for its own block were taken for this one's, emptied and their slots
    // freed -- a c4sc in a -j 3 build killed "not mapped" at its block's
    // first page, 512 pages with slots and the first with none.
    if (r->kind != UR_SLAB && mmu_uoff) {
        klock_take(mmu_ulock);
        mmu_vfree(r->va, r->npages);
        klock_drop(mmu_ulock);
    }
    // its block, still the heap's for frames lent to other pages, is a
    // block of single frames now: its free ones serve first, it counts as
    // room to page in, and mmu_freeblock may move what is left out of it
    if (own && (struct fchunk *)mmu_fc[r->base >> 12] == own && !own->pool) {
        own->pool = 1;
        mmu_poolbytes = mmu_poolbytes + own->npages * MMU_PAGE + MMU_PAGE - 1;
    }
    if (mmu_lastin == r) mmu_lastin = 0;
    if (mmu_runr == r) mmu_runlog();
    sl4b_free(mmu_rcache, (char *)r);
}

// N bytes the kernel fills for a task that has not got them yet: an
// image, a stack, argv. Page-aligned, and the kernel's until mmu_adopt.
int umalloc(int n) {
    struct urgn *r;
    if (!mmu_on) return (int)vfs_malloc(n);
    if (!(r = mmu_region(n, UR_BLOCK))) return 0;
    return r->base;
}

// N bytes of stack, with a guard page below them that is never mapped
int umalloc_stack(int n) {
    struct urgn *r;
    if (!mmu_on) return (int)vfs_malloc(n);
    if (!(r = mmu_region(n + MMU_PAGE, UR_BLOCK))) return 0;
    r->guard = 1;
    MMU_RCAN(r);
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
    MMU_RCAN(r);
    r->flags = PTE_P | PTE_U | PTE_X;
}

// P is an image's code (mmu_fault)
static void mmu_code(int p) {
    struct urgn *r;
    if (p && (r = mmu_rgn(p)) && !mmu_off(r, p)) { r->code = 1; MMU_RCAN(r); }
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
    mmu_dir_out(t);
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
    int k, i, w, b, c[10];
    t = mmu_root(t);
    if (n < 0) return 0;
    // a block of its own: with swap, no memory until it is used (mmu_lazy)
    if (n > UR_SMALL) {
        if (!(r = mmu_lazy(n)) && !(r = mmu_region(n, UR_BLOCK))) {
            // said (dmesg), with what there was: a program that stops out
            // of memory with megabytes free has its reason here
            mmu_counts(c);
            klog("mmu: task %d (%s): no memory for a block of %d KB: room to page in %d KB, %d KB of swap free, the heap's biggest block %d KB\n",
                t->id, t->name, n / 1024, mmu_pageroom() / 1024, mmu_sfree * 4, c[5] < 0 ? c[5] : c[5] / 1024);
            return 0;
        }
        r->heap = 1;
        mmu_give(r, t, PTE_P | PTE_U | PTE_W);
        return r->va;
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
    memset((char *)(s->va + i * s->size), 0, s->size);
    return s->va + i * s->size;
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
    int *d, *e;
    int va, end, need;
    if (!mmu_on || !(t = sched_current()) || t->privs != PRIV_USER) return 1;
    if (n <= 0) return 1;
    end = p + n;
    if (p < mmu_uoff + MMU_PAGE || end < p || end > mmu_uoff + (mmu_uoff ? MMU_WINPAGES * MMU_PAGE : mmu_memsize)) return 0;
    need = PTE_P | PTE_U | (w ? PTE_W : 0);
    va = p & ~(MMU_PAGE - 1);
    d = MMU_DIR(t);
    while (va < end) {
        if ((!(e = MMU_PTE0(d, va)) || (*e & need) != need) &&
            (!mmu_fetch(t, va) || !(e = MMU_PTE0(d, va)) || (*e & need) != need)) return 0;
        va = va + MMU_PAGE;
    }
    return 1;
}

// ... and a string: its length, or -1
// The arguments at P: mmu_mapgen read before the tables are, so that an unmap
// after this look changes what the next one compares (UARGS_OK)
int uaccess_args(int p) {
    int g;
    g = mmu_mapgen;
    if (!uaccess(p, 3 * sizeof(int), 0)) return 0;
    cpu_self->okva = p & ~(MMU_PAGE - 1);
    cpu_self->okgen = g;
    return 1;
}

int ustrlen(char *s) {
    struct task *t;
    int *d, *e;
    int n, p, va, lim;
    if (!mmu_on || !(t = sched_current()) || t->privs != PRIV_USER) {
        n = 0;
        while (s[n]) ++n;
        return n;
    }
    // a page checked once, then its bytes
    d = MMU_DIR(t);
    p = (int)s;
    while (1) {
        if (p < mmu_uoff + MMU_PAGE || p >= mmu_uoff + (mmu_uoff ? MMU_WINPAGES * MMU_PAGE : mmu_memsize)) return -1;
        va = p & ~(MMU_PAGE - 1);
        if ((!(e = MMU_PTE0(d, va)) || (*e & (PTE_P | PTE_U)) != (PTE_P | PTE_U)) &&
            (!mmu_fetch(t, va) || !(e = MMU_PTE0(d, va)) ||
             (*e & (PTE_P | PTE_U)) != (PTE_P | PTE_U))) return -1;
        lim = va + MMU_PAGE;
        while (p < lim && *(char *)p) ++p;
        if (p < lim) return p - (int)s;
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
    int n;
    if (!host_has(C4IX_I_MMU)) return;
    mmu_memsize = *(int *)MMU_MEMSIZE;
    mmu_npages = mmu_memsize / MMU_PAGE;
    if (!(mmu_rcache = sl4b_cache_create("urgn", sizeof(struct urgn), 32)) ||
        !(mmu_pg = (int *)malloc(mmu_npages * sizeof(int))) ||
        !(mmu_tc = (int *)malloc(mmu_npages * sizeof(int)))) {
        klog("mmu: no memory for its tables; left off\n");
        return;
    }
    memset((char *)mmu_tc, 0, mmu_npages * sizeof(int));
    if (!(mmu_dir = (int *)mmu_page())) {
        klog("mmu: no memory for its tables; left off\n");
        return;
    }
    memset((char *)mmu_pg, 0, mmu_npages * sizeof(int));
    // user addresses from 256 MB, where memory fits below them: the
    // window's books and the kernel's map of it, with tables for a 128th of
    // memory (and a page more: the block is not page-aligned)
    if (mmu_memsize <= MMU_UWIN_BASE) {
        n = mmu_npages / 128;
        if (n < MMU_CHUNK) n = MMU_CHUNK;
        if (n > MMU_WINPAGES / 1024) n = MMU_WINPAGES / 1024;
        n = n + 1;
        if (!(mmu_vpg = (int *)malloc(MMU_WINPAGES * sizeof(int))) ||
            !(mmu_vbits = (int *)malloc(MMU_WINPAGES / 8)) ||
            !(mmu_kdir = (int *)mmu_page()) || !mmu_kfill(n)) {
            klog("mmu: no memory for its tables; left off\n");
            return;
        }
        if (!(mmu_fc = (int *)malloc(mmu_npages * sizeof(int))) ||
            !(mmu_fva = (int *)malloc(mmu_npages * sizeof(int)))) {
            klog("mmu: no memory for its tables; left off\n");
            return;
        }
        memset((char *)mmu_fc, 0, mmu_npages * sizeof(int));
        memset((char *)mmu_fva, 0, mmu_npages * sizeof(int));
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
    struct task *root;
    int *e, *u, *d;
    int k;
    info = *(int *)MMU_FINFO;
    pc = *(int *)MMU_FPC;
    // A device's use of a block of its own that is not one run of memory
    // -- a block's pages are frames from anywhere once they have been out
    // or were never touched -- the display's ring, say: the block made one
    // run, and the access runs again
    // (a page that is out is brought in below, as for the task's own
    // access: only a refusal of pages that are in -- not one run -- is this)
    r = t ? mmu_rgn(va) : 0;
    root = mmu_root(t);
    if (r && (info & MMU_FK_DMA) && (info & MMU_FK_PRESENT) && r->owner == root && mmu_contig(r)) return;
    // Its own memory, on swap or not touched yet: in, and the instruction
    // runs again (faults are precise). With no room for it even after
    // putting out what else could go, the task cannot go on.
    if (r && r->owner == root && mmu_isout(va)) {
        k = mmu_bringin(r, va);
        if (k) return;
        kprintf("c4ix: task %d (%s): its memory at 0x%08x is on swap, and there is no room for it; killed\n",
            t->id, t->name, va);
        t->killsig = C4IX_SIGSEGV;
        TF_SET(t, TF_SIG);
        task_exit(-1);
        return;
    }
    // Its own page, in memory, that its space has not got: one that came in
    // when there was no memory for a table of its space (mmu_pagein), or
    // one mmu_contig took out and could not move -- or a page of its image,
    // its stack, a shared library's code, mapped when it was given them
    // where there was no memory for a table either (mmu_rmap, mmu_take:
    // innerbench -Tln 40 had one c4m killed so, "not mapped"). Never a
    // stack's guard page.
    d = 0;
    if (r && r->owner == root) d = r->dir;
    else if (r && r->shared) d = mmu_dir_of(t);
    if (d && d != mmu_dir && !(r->guard && mmu_off(r, va) < MMU_PAGE) && (e = MMU_PTE0(mmu_kdir, va)) && (*e & PTE_P) &&
        !((u = MMU_PTE0(d, va)) && (*u & PTE_P))) {
        if (!(u = mmu_pte_out(d, va))) {
            kprintf("c4ix: task %d (%s): its page at 0x%08x is in memory, and there is no memory for a table to map it in; killed\n",
                t->id, t->name, va);
            t->killsig = C4IX_SIGSEGV;
            TF_SET(t, TF_SIG);
            task_exit(-1);
            return;
        }
        // (pages put out to make the table may have been this one: then
        // the access faults again, and brings it in)
        if (*e & PTE_P) {
            *u = (*e & ~(MMU_PAGE - 1)) | r->flags;
            *(int *)MMU_INVLPG = va;
        }
        return;
    }
    // A C4KE program writing its own code: C4KE's ps patches itself. The
    // page it writes is made writable, that page alone, and the store
    // runs again (faults are precise: the trap returns to the
    // instruction); the rest of its text stays read-only. C4IX's own
    // programs never do this, and are killed for it below.
    if (r && t->ckabi && (info & MMU_FK_WRITE) && !(info & MMU_FK_DMA) && r->code &&
        r->owner == root && r->dir && (e = MMU_PTE0(r->dir, va)) &&
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
    if (t) { t->killsig = C4IX_SIGSEGV; TF_SET(t, TF_SIG); }
    task_exit(-1);
}

// ---- frames ----
//
// With user addresses of their own (mmu_uoff), user memory is frames: the
// pages of heap blocks, a region's own block (an image, a stack) until
// swap puts a page out, or blocks of single frames, taken from the heap's
// top, that any page touched or coming back gets one of. A block's record
// counts its frames in use wherever they are mapped, and a block with none
// in use goes back to the heap. So swap moves a page at a time: a page put
// out gives its frame to a page coming in, and a block whose pages have
// all gone gives the heap its memory. (Without a window of user addresses
// the old way stands: a region is its heap block, freed with it, and
// there is no swap.)
static struct fchunk *mmu_fcfree, *mmu_fcall;
static int mmu_round;          // mmu_reclaim's rounds
// Pages of a block taken for single frames: a big one where the heap has
// one run that long, else a small one. Any frame serves any page, so a
// big block's free frames are as good as a new block's, and the fewer
// blocks the user's frames are in, the fewer holes they cut between the
// kernel's long-lived blocks (its RAM files).
enum { FC_SMALL = 16, FC_BIG = 256 };
// the host's swap (below): its registers, and a page through them
enum { SWAP_ADDR = 0x11c, SWAP_CMD = 0x158, SWAP_OUT = 1, SWAP_IN = 2, SWAP_DROP = 3, SWAP_RESET = 4, SWAP_HIGH = 5, SWAP_LARGEST = 6 };
static int mmu_sop(int op, int slot, int pa);
static void mmu_sput(int s);

static int mmu_cnew(int raw, int base, int np, int used) {
    struct fchunk *c;
    int k, n;
    n = (np + 31) / 32;
    if (!(c = (struct fchunk *)malloc(sizeof(struct fchunk)))) return 0;
    if (!(c->bits = (int *)malloc(n * sizeof(int)))) { free((char *)c); return 0; }
    memset((char *)c->bits, used ? 0 : 255, n * sizeof(int));
    c->raw = raw;
    c->base = base;
    c->npages = np;
    c->nused = used ? np : 0;
    c->onlist = 0;
    c->next = 0;
    c->tried = 0;
    // (a record from the heap may be a freed one's: mmu_fput counts a pool
    // block's bytes off as it goes)
    c->pool = 0;
    k = 0;
    while (k < np) { mmu_fc[(base >> 12) + k] = (int)c; ++k; }
    klock_take(mmu_flock);
    c->anext = mmu_fcall;
    mmu_fcall = c;
    if (!used) { c->next = mmu_fcfree; mmu_fcfree = c; c->onlist = 1; }
    klock_drop(mmu_flock);
    return (int)c;
}

// Which user address each frame from PA on is (the next NP), 0: none
static void mmu_fvaset(int pa, int va, int np) {
    int k;
    k = 0;
    while (k < np) { mmu_fva[(pa >> 12) + k] = va ? va + k * MMU_PAGE : 0; ++k; }
}

// A frame back to its block; a block with none in use, back to the heap
static void mmu_fput(int pa) {
    struct fchunk *c, **pp;
    int i, k;
    if (!(c = (struct fchunk *)mmu_fc[pa >> 12])) return;
    mmu_fva[pa >> 12] = 0;
    klock_take(mmu_flock);
    i = (pa - c->base) >> 12;
    if (c->bits[i >> 5] & (1 << (i & 31))) { klock_drop(mmu_flock); return; }
    c->bits[i >> 5] = c->bits[i >> 5] | (1 << (i & 31));
    --c->nused;
    if (!c->onlist) { c->next = mmu_fcfree; mmu_fcfree = c; c->onlist = 1; }
    if (c->nused) { klock_drop(mmu_flock); return; }
    pp = &mmu_fcfree;
    while (*pp && *pp != c) pp = &(*pp)->next;
    if (*pp) *pp = c->next;
    pp = &mmu_fcall;
    while (*pp && *pp != c) pp = &(*pp)->anext;
    if (*pp) *pp = c->anext;
    k = 0;
    // the block's pages are the heap's again, nobody's region (a region
    // whose pages all moved or went out lives on elsewhere: mmu_unsetpg)
    while (k < c->npages) { mmu_fc[(c->base >> 12) + k] = 0; mmu_pg[(c->base >> 12) + k] = 0; ++k; }
    klock_drop(mmu_flock);
    if (c->pool) mmu_poolbytes = mmu_poolbytes - (c->npages * MMU_PAGE + MMU_PAGE - 1);
    mmu_nofnew = 0;
    free((char *)c->raw);
    free((char *)c->bits);
    free((char *)c);
}

static int mmu_clock(int keep, int want);

// What memory on demand has to page in: the heap, less its sixteenth for
// the kernel and less what the kernel and the regions of their own hold
// (in use, but for the blocks of single frames, and the frames of
// programs' own blocks that are free, their pages out: mmu_ftake lends
// them when there is nothing else)
int mmu_pageroom() {
    struct fchunk *c;
    int used, size, lend;
    used = *(int *)C4IX_HEAP_USED;
    size = *(int *)C4IX_HEAP_END - *(int *)C4IX_HEAP_BASE;
    lend = 0;
    klock_take(mmu_flock);
    c = mmu_fcfree;
    while (c) { if (!c->pool) lend = lend + (c->npages - c->nused) * MMU_PAGE; c = c->next; }
    klock_drop(mmu_flock);
    return size - size / 16 - (used - mmu_poolbytes - lend);
}

// Is there room in the heap for another block of NP frames, leaving the
// kernel its sixteenth? Past that, frames come from pages put out.

static int mmu_roomy(int np) {
    int used, size;
    used = *(int *)C4IX_HEAP_USED;
    size = *(int *)C4IX_HEAP_END - *(int *)C4IX_HEAP_BASE;
    return used + (np + 1) * MMU_PAGE < size - size / 16;
}

// A new block of frames, a big one if it fits: 0 if none does. From the
// heap's top (SWAP_HIGH), where the kernel's own blocks, which come from
// its bottom, are not: user memory and the kernel's grow toward each
// other, and a block of frames freed leaves a hole next to others, not
// between two RAM files.
static int mmu_fnew(int room) {
    struct fchunk *c;
    int raw, np;
    np = FC_BIG;
    while (1) {
        raw = 0;
        if (room || mmu_roomy(np)) {
            if (mmu_swapok) mmu_sop(SWAP_HIGH, 1, 0);
            raw = (int)malloc(np * MMU_PAGE + MMU_PAGE - 1);
            if (mmu_swapok) mmu_sop(SWAP_HIGH, 0, 0);
        }
        if (raw) {
            if ((c = (struct fchunk *)mmu_cnew(raw, (raw + MMU_PAGE - 1) & ~(MMU_PAGE - 1), np, 0))) {
                c->pool = 1;
                mmu_poolbytes = mmu_poolbytes + np * MMU_PAGE + MMU_PAGE - 1;
                return 1;
            }
            free((char *)raw);
            return 0;
        }
        if (np == FC_SMALL) return 0;
        np = FC_SMALL;
    }
    return 0;
}

// A free frame of a block there is already, not SKIP, for the page at VA:
// of a block taken for single frames (POOL), or of any; of those, the one
// with the most frames in use, so that the emptiest give theirs up and go
// back to the heap whole. 0 if there is none.
//
// A program's own block (mmu_region: its image, its stack, a block the
// heap had in one run) has free frames where its pages went out, and they
// serve another's page only when no block of single frames has one and
// the heap has no room for another: one that did would keep the block --
// all of it, the heap's in one run -- after its program had gone, for as
// long as the page it was lent to lived (a shell's, the desktop's).
static int mmu_ftake(int va, struct fchunk *skip, int pool) {
    struct fchunk *c, *best, **pp;
    int i;
    klock_take(mmu_flock);
    while (1) {
        best = 0;
        pp = &mmu_fcfree;
        while ((c = *pp)) {
            if (c->nused >= c->npages) { *pp = c->next; c->onlist = 0; continue; }
            if (c != skip && (!pool || c->pool) && (!best || c->nused > best->nused)) best = c;
            pp = &c->next;
        }
        if (!(c = best)) { klock_drop(mmu_flock); return 0; }
        i = 0;
        while (i < c->npages && !c->bits[i >> 5]) i = i + 32;
        while (i < c->npages && !(c->bits[i >> 5] & (1 << (i & 31)))) ++i;
        if (i < c->npages) break;
        // (counted as having one free, but none is: not on the list)
        c->nused = c->npages;
    }
    c->bits[i >> 5] = c->bits[i >> 5] & ~(1 << (i & 31));
    ++c->nused;
    if (c->nused == c->npages) {
        pp = &mmu_fcfree;
        while (*pp && *pp != c) pp = &(*pp)->next;
        if (*pp) *pp = c->next;
        c->onlist = 0;
    }
    klock_drop(mmu_flock);
    mmu_fva[(c->base >> 12) + i] = va;
    return c->base + i * MMU_PAGE;
}

// ... and, but for a page brought in only because it follows the one a
// fault wanted (EVICT 0), from pages put out
static int mmu_fget(int va, int keep, int evict) {
    int f, room;
    room = 0;
    while (1) {
        if ((f = mmu_ftake(va, 0, 1))) return f;
        // (the heap had no block: not asked again for every frame, which is
        // 2 refused mallocs and 6 of its registers, until a block has gone
        // back or load control's next look: mmu_balance)
        if (room || !mmu_nofnew) {
            if (mmu_fnew(room)) continue;
            if (!room) mmu_nofnew = 1;
        }
        if ((f = mmu_ftake(va, 0, 0))) return f;
        if (evict && vfs_reclaim(FC_BIG * MMU_PAGE)) continue;
        if (!evict) return 0;
        if (mmu_clock(keep, SWAP_AHEAD)) continue;
        // nothing could go: the kernel's sixteenth, as a last resort
        if (room) return 0;
        room = 1;
    }
    return 0;
}

// R's frames back, and its slots: each page as the kernel's map has it
static void mmu_frames_release(struct urgn *r) {
    int *e;
    int k;
    k = 0;
    while (k < r->npages) {
        if ((e = mmu_pte(mmu_kdir, r->va + k * MMU_PAGE, 0))) {
            if (*e & PTE_P) mmu_fput(*e & ~(MMU_PAGE - 1));
            else if (*e & PTE_SWAP) { mmu_sop(SWAP_DROP, *e >> 12 & 0xfffff, 0); mmu_sput(*e >> 12 & 0xfffff); }
            *e = 0;
        }
        ++k;
    }
}

// ---- swap ----
//
// When the heap has no block for a region, or there is no frame for a page
// coming back, pages of idle tasks are put out to the host's swap, a page
// to a slot, and their frames are free; a task that touches one again
// takes a page fault, and it comes back into any free frame (mmu_fault),
// at the same user address, with the pages after it that went out too.
// The host keeps the pages (libjs, devices.js SWAP_*: memory beyond the
// arena, forgotten with the machine) behind registers of their own, so
// nothing a task does with the drives comes between swap's stores, and
// nothing is left behind on any drive.
//
// A page put out stays in the kernel's map, not present, with its slot in
// the entry (PTE_SWAP): the kernel, which might follow a pointer into it
// that it did not check first, reads and writes the host's copy there
// (libjs's user window does), and never lands on whatever has the page's
// old frame now.
//
// Which go: pages of what tasks asked for (umem_alloc), not their images,
// stacks or arguments -- the asking task's too, since a page the kernel
// reaches through the window may move between its loads and stores
// (libjs looks it up each time, and finds the host's copy of one that is
// out), and a syscall that waits is run again when its task wakes, and
// checks its pointers again (uaccess brings back what they name). Not a
// page a device has used (PTE_PIN, which the host sets in the kernel's
// map): the display's framebuffer and rings, a drive's buffer. Which of
// those: a clock's (mmu_clock), the pages nobody has used since its hand
// last passed going first (PTE_A).
static int *mmu_sbits;         // slots in use, a bit each
static int mmu_snext;

// One page through the host's swap: OP on SLOT, at PA in memory
static int mmu_sop(int op, int slot, int pa) {
    int r;
    klock_take(mmu_flock);
    *(int *)SWAP_ADDR = pa;
    *(int *)SWAP_CMD = op | (slot << 4);
    r = *(int *)SWAP_CMD;
    klock_drop(mmu_flock);
    return r;
}

static int mmu_salloc() {
    int i, n;
    klock_take(mmu_flock);
    i = mmu_snext; n = 0;
    while (n < mmu_swapslots) {
        if (i >= mmu_swapslots) i = 0;
        if (!(mmu_sbits[i >> 5] & (1 << (i & 31)))) {
            mmu_sbits[i >> 5] = mmu_sbits[i >> 5] | (1 << (i & 31));
            mmu_snext = i + 1;
            --mmu_sfree;
            klock_drop(mmu_flock);
            return i;
        }
        // a word of slots all in use at once
        if (mmu_sbits[i >> 5] == -1) { n = n + 32 - (i & 31); i = (i | 31) + 1; continue; }
        ++i; ++n;
    }
    klock_drop(mmu_flock);
    return -1;
}

static void mmu_sput(int s) {
    klock_take(mmu_flock);
    mmu_sbits[s >> 5] = mmu_sbits[s >> 5] & ~(1 << (s & 31));
    ++mmu_sfree;
    klock_drop(mmu_flock);
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
    klog("swap: the host keeps up to %d MB of pages put out, a page at a time\n", mmu_swapslots / 256);
}

// May R's pages go? What a task asked for, mapped in its space, of a task
// that is going on: the asking one, an idle one, or one another CPU is
// running, which is told to forget them first (mmu_pageout's shootdown)
// The page at VA of R out to the host, its frame free: 0 if it cannot go
static int mmu_shootdown(int d);

static int mmu_pageout(struct urgn *r, int va) {
    int *e, *u;
    int f, s, old;
    if (!(e = MMU_PTE0(mmu_kdir, va)) || !(*e & PTE_P) || (*e & PTE_PIN) || mmu_sfree < 1) return 0;
    f = *e & ~(MMU_PAGE - 1);
    s = mmu_salloc();
    // out of its space -- a CPU running it forgets it now (the IPI below),
    // and each other that has it loaded before it runs it again (mmu_gen)
    // -- and into the kernel's map as its slot
    u = r->dir ? MMU_PTE0(r->dir, va) : 0;
    old = u ? *u : 0;
    if (u) *u = 0;
    *(int *)MMU_INVLPG = va;
    // another CPU running the space may have the page in its TLB: it
    // forgets it, and says so, before the copy (so no store of its lands
    // after) and before the frame goes to anyone else
    if (r->dir && !mmu_shootdown((int)r->dir)) {
        if (u) *u = old;
        mmu_sop(SWAP_DROP, s, 0);
        mmu_sput(s);
        return 0;
    }
    if (mmu_sop(SWAP_OUT, s, f) < 0) {
        if (u) *u = old;
        mmu_sput(s);
        klog("swap: the host would not keep a page; swap off\n");
        mmu_swapok = 0;
        return 0;
    }
    *e = (s << 12) | PTE_SWAP;
    ++r->swapped;
    ++mmu_gen;
    ++mmu_swapouts;
    mmu_fput(f);
    return 1;
}

// The page at VA of R back from the host, into any free frame (a page
// never touched comes back as zeros): 0 if there is none. Of the pages
// from KEEP, which are coming in with it, none goes to make room; and
// none at all but for the page a fault wants (EVICT). It comes in as used
// (PTE_A): the instruction that wanted it has not run yet, and the clock
// is to give it its turn, not take it back first -- the instruction may
// want another page too, which putting this one out to bring in would go
// round for ever.
static int mmu_pagein(struct urgn *r, int va, int keep, int evict) {
    int *e, *u;
    int f, s;
    if (!(e = MMU_PTE0(mmu_kdir, va)) || !(*e & PTE_SWAP)) return 1;
    // the table it is mapped with first, where making one may put pages
    // out -- which a frame taken for it would not live through (none of
    // them this one, which is out already); without one it comes in
    // unmapped, and its fault maps it (mmu_fault)
    u = 0;
    if (r->dir && evict) u = mmu_pte_out(r->dir, va);
    else if (r->dir) u = mmu_pte(r->dir, va, 1);
    if (!(f = mmu_fget(va, keep, evict))) return 0;
    s = *e >> 12 & 0xfffff;
    mmu_sop(SWAP_IN, s, f);
    mmu_sput(s);
    *e = f | PTE_P | PTE_U | PTE_W;
    if (u) *u = f | r->flags | PTE_A;
    *(int *)MMU_INVLPG = va;
    --r->swapped;
    ++mmu_swapins;
    return 1;
}

// Is the page at VA out?
static int mmu_isout(int va) {
    int *e;
    return mmu_kdir && (e = MMU_PTE0(mmu_kdir, va)) && !(*e & PTE_P) && (*e & PTE_SWAP);
}

// The page at VA of R back, and up to SWAP_AHEAD - 1 after it that are out
// too, while there are frames free for them (a program goes through its
// memory in order more often than not, but when memory is short a page it
// may never want is not worth one it does): 0 if the page itself could
// not come back
static int mmu_bringin(struct urgn *r, int va) {
    int *e;
    int k, n, end, fresh, c0;
    va = va & ~(MMU_PAGE - 1);
    fresh = (e = MMU_PTE0(mmu_kdir, va)) && (*e & PTE_ZERO);
    c0 = __c4_cycles();
    if (!mmu_pagein(r, va, va, 1)) { mmu_pgcyc = mmu_pgcyc + (__c4_cycles() - c0); return 0; }
    n = 1;
    end = r->va + r->npages * MMU_PAGE;
    k = va + MMU_PAGE;
    while (n < SWAP_AHEAD && k < end && mmu_isout(k) && mmu_pagein(r, k, va, 0)) { ++n; k = k + MMU_PAGE; }
    // (load control's measure, mmu_balance)
    mmu_pgcyc = mmu_pgcyc + (__c4_cycles() - c0);
    // said once for a block while its pages come back, not for each
    // sixteen: the log is 64 KB. A page's first touch is not news.
    if (fresh) ++mmu_firsts;
    else if (r != mmu_lastin) {
        if (mmu_say())
            klog("swap: %d KB of task %d (%s) at 0x%08x back in, the rest of its block as it is touched\n",
                n * 4, r->owner->id, r->owner->name, va);
        mmu_lastin = r;
    }
    return 1;
}

// Every page of R back
static int mmu_bringall(struct urgn *r) {
    int k;
    k = 0;
    while (r->swapped && k < r->npages) {
        if (mmu_isout(r->va + k * MMU_PAGE) && !mmu_bringin(r, r->va + k * MMU_PAGE)) return 0;
        ++k;
    }
    return 1;
}

// A page fault or a syscall's pointer at VA, T's own memory out: back
static int mmu_fetch(struct task *t, int va) {
    struct urgn *r;
    if (!(r = mmu_rgn(va)) || r->owner != mmu_root(t) || !mmu_isout(va)) return 0;
    return mmu_bringin(r, va);
}

// What went out of one block, said once for up to a MB of it, and when
// pages of another block go, or the block is freed: the log is 64 KB, and
// a program bigger than memory puts pages out at every fault
// (the task and its name kept from the run's start: the run is said
// later, when the region or its task may be gone; nothing said while
// mmu_kalloc reclaims, which klog reaches as its file grows -- a klog
// inside a klog)
static void mmu_runlog() {
    if (mmu_runr && mmu_runn && !mmu_quiet && mmu_say())
        klog("swap: %d KB of task %d (%s) at 0x%08x out%s\n", mmu_runn * 4, mmu_runid, mmu_runname, mmu_runva,
            mmu_shootdowns != mmu_runshot ? ", the CPU running it told to forget them first (an IPI)" :
            mmu_shootlater == mmu_runlater ? "" : mmu_runon ? ", the CPU running it, in the kernel, told to forget them on its way back" :
            ", the CPU that ran it last told to forget them before it runs it again");
    mmu_runr = 0;
    mmu_runn = 0;
}

// The page at VA of R out, as part of a run for the log: 0 if it could not go
static int mmu_out(struct urgn *r, int va) {
    int k;
    if (r != mmu_runr || mmu_runn >= 256) {
        mmu_runlog();
        mmu_runr = r; mmu_runva = va; mmu_runshot = mmu_shootdowns; mmu_runlater = mmu_shootlater;
        mmu_runon = r->owner->oncpu;
        mmu_runid = r->owner->id;
        k = 0;
        while (k < 31 && r->owner->name[k]) { mmu_runname[k] = r->owner->name[k]; ++k; }
        mmu_runname[k] = 0;
    }
    if (!mmu_pageout(r, va)) return 0;
    ++mmu_runn;
    // allocation takes the kernel lock from now, where swap runs (sched_light)
    mmu_short = 1;
    return 1;
}

// Swap's clock: up to WANT pages put out, how many went. The hand goes
// round the window's pages; a page used since it last passed (PTE_A, set
// as the MMU fills a translation) is passed over, its bit cleared and the
// translation forgotten so that a use sets it again, and the first that
// has not been used goes -- any task's, the asking one's too (mmu_canout).
// Never one of the SWAP_AHEAD pages from KEEP, which are coming in, nor
// one a device has used (PTE_PIN). Two turns round the window find one
// if any can go: the first clears what the second finds.
static int mmu_clock(int keep, int want) {
    struct urgn *r;
    int *e, *u, *kt, *ut;
    int i, k, n, seen, va, end;
    if (!mmu_swapok || !sched_has_bkl()) return 0;
    n = 0; seen = 0;
    while (n < want && seen < 2 * MMU_WINPAGES) {
        if (mmu_hand < 1 || mmu_hand >= MMU_WINPAGES) mmu_hand = 1;
        i = mmu_hand;
        r = (struct urgn *)mmu_vpg[i];
        va = mmu_uoff + i * MMU_PAGE;
        if (!mmu_canout(r)) {
            // the rest of the region, or of the window's 32 pages not handed out
            if (r) k = (r->va + r->npages * MMU_PAGE - va) / MMU_PAGE;
            else k = mmu_vbits[i >> 5] ? 1 : 32 - (i & 31);
            if (k < 1) k = 1;
            mmu_hand = i + k;
            seen = seen + k;
            continue;
        }
        // the rest of this region, asked of it once, and its tables looked
        // up once per 1,024 pages: when memory is short this is most of
        // what the machine does
        end = i + (r->va + r->npages * MMU_PAGE - va) / MMU_PAGE;
        if (end > MMU_WINPAGES) end = MMU_WINPAGES;
        kt = 0;
        while (i < end && n < want) {
            k = (va >> 12) & 1023;
            if (!kt || !k) { kt = MMU_TBL(mmu_kdir, va); ut = MMU_TBL(r->dir, va); }
            mmu_hand = ++i;
            ++seen;
            if (kt && !(va >= keep && va < keep + SWAP_AHEAD * MMU_PAGE)) {
                e = kt + k;
                if ((*e & PTE_P) && !(*e & PTE_PIN)) {
                    u = ut ? ut + k : 0;
                    if (u && (*u & PTE_A)) {
                        *u = *u & ~PTE_A;
                        *(int *)MMU_INVLPG = va;
                    } else if (mmu_out(r, va)) ++n;
                }
            }
            va = va + MMU_PAGE;
        }
    }
    return n;
}

// The page at VA of R into a free frame of another block than FROM, at the
// same user address -- a copy, no swap -- or, with none free, out (OUT; or
// without it, only into a block of single frames, and never out): 0 if it
// could do neither. As a page put out leaves its space, so this one does
// while it is copied: a CPU running it forgets it first, and each other
// that has the space loaded before it next runs it (mmu_gen).
static int mmu_move(struct urgn *r, int va, struct fchunk *from, int out) {
    int *e, *u;
    int f, g, old;
    if (!(e = MMU_PTE0(mmu_kdir, va)) || !(*e & PTE_P) || (*e & PTE_PIN)) return 0;
    if (!(g = mmu_ftake(va, from, 1)) && (!out || !(g = mmu_ftake(va, from, 0)))) return out ? mmu_out(r, va) : 0;
    f = *e & ~(MMU_PAGE - 1);
    u = r->dir ? MMU_PTE0(r->dir, va) : 0;
    old = u ? *u : 0;
    if (u) *u = 0;
    *(int *)MMU_INVLPG = va;
    if (r->dir && !mmu_shootdown((int)r->dir)) { if (u) *u = old; mmu_fput(g); return 0; }
    memcpy((char *)g, (char *)f, MMU_PAGE);
    *e = g | (*e & (MMU_PAGE - 1));
    if (u && old) *u = g | (old & (MMU_PAGE - 1));
    *(int *)MMU_INVLPG = va;
    ++mmu_gen;
    ++mmu_moves;
    mmu_fput(f);
    return 1;
}

// Can every page in block C move? Never the pages from KEEP.
static int mmu_movable(struct fchunk *c, int keep) {
    struct urgn *r;
    int *e;
    int k, va, ok;
    ok = 1;
    k = 0;
    while (ok && k < c->npages) {
        if (!(c->bits[k >> 5] & (1 << (k & 31)))) {
            va = mmu_fva[(c->base >> 12) + k];
            r = va ? (struct urgn *)mmu_vpg[(va - mmu_uoff) >> 12] : 0;
            ok = mmu_canout(r) && !(va >= keep && va < keep + SWAP_AHEAD * MMU_PAGE) &&
                (e = MMU_PTE0(mmu_kdir, va)) && (*e & PTE_P) && !(*e & PTE_PIN);
        }
        ++k;
    }
    return ok;
}

// Every page of block C moved (mmu_move, OUT as it has it), or as many
// as would go; the block is freed with its last frame
static void mmu_empty(struct fchunk *c, int out) {
    struct urgn *r;
    int k, va, n;
    k = 0;
    while (k < c->npages) {
        // nothing of the block is read after its last frame
        va = (c->bits[k >> 5] & (1 << (k & 31))) ? 0 : mmu_fva[(c->base >> 12) + k];
        n = c->nused;
        if (va && (r = mmu_rgn(va)) && !mmu_move(r, va, c, out)) return;
        if (va && n == 1) return;
        ++k;
    }
}

// A frame block freed whole (the clock's choice is no use when the heap
// needs one run of memory), its pages moved to free frames of other
// blocks, or put out where there are none: of the blocks whose pages can
// all go, the one with the fewest in use; 0 if there is none. Never the
// pages from KEEP.
static int mmu_freeblock(int keep) {
    struct fchunk *c, *best;
    best = 0;
    c = mmu_fcall;
    while (c) {
        if (c->nused && c->tried != mmu_round && (!best || c->nused < best->nused)) {
            if (mmu_movable(c, keep)) best = c;
            else c->tried = mmu_round;
        }
        c = c->anext;
    }
    if (!best) return 0;
    best->tried = mmu_round;
    mmu_empty(best, 1);
    return 1;
}

// ---- compaction ----
//
// A block of single frames goes back to the heap only with its last frame,
// and a page coming back from swap takes whichever frame is free: once a
// crowd of programs has gone, what is left -- the shells', the desktop's --
// is a few pages to a block, across dozens of blocks, with the heap's free
// memory in pieces between them (the desktop after innerbench -Tln 40 in
// 32 MB: 60 blocks, 7 MB, 4 MB of it in use, and nothing free more than
// 1.1 MB in one run). So when a task is done (task_destroy), the emptiest
// block of single frames whose pages the others have free frames for has
// them copied there (mmu_move, no swap), and goes back to the heap; then
// the next, while one fits. Each block so freed is one fewer, and its
// memory is the heap's top again, where blocks of frames come from.
enum { COMPACT_MAX = 64 };     // blocks freed at a time, at most
static void mmu_compact() {
    struct fchunk *c, *best;
    int spare, n, was;
    if (!mmu_uoff || !mmu_fcall || !sched_has_bkl()) return;
    ++mmu_round;
    n = 0;
    while (n < COMPACT_MAX) {
        spare = 0;
        c = mmu_fcall;
        while (c) { if (c->pool) spare = spare + c->npages - c->nused; c = c->anext; }
        best = 0;
        c = mmu_fcall;
        while (c) {
            if (c->pool && c->nused && c->tried != mmu_round && c->nused <= spare - (c->npages - c->nused) &&
                (!best || c->nused < best->nused)) {
                if (mmu_movable(c, 0)) best = c;
                else c->tried = mmu_round;
            }
            c = c->anext;
        }
        if (!best) break;
        best->tried = mmu_round;
        was = mmu_moves;
        mmu_empty(best, 0);
        if (mmu_moves == was) break;
        ++n;
    }
    if (n) klog("mmu: %d blocks of frames given back to the heap, their pages moved to others\n", n);
}

// ---- load control ----
//
// Swap lets a task have more memory than there is, and every task that:
// forty c4m's in 32 MB, a few MB each, ran, their pages going out and
// coming back as they took turns. Fifty did not: each one's pages were out
// before its turn came round again, every turn went on bringing them back,
// and nothing finished -- 446,000 pages in and out in six minutes of the
// machine's time, the CPUs taking turns at the kernel lock to do it. So
// when bringing pages back is most of what the machine does, a task is
// held: it is not run (sched_mine), its pages go out as the clock finds
// them unused, and the others have the memory. Measured over each LOAD_MS
// of the machine's clock, the cycles spent bringing pages back
// (mmu_bringin) against a CPU's: more than half, and one more is held;
// less than an eighth, and one is let go. Held for HOLD_MS, a task is let
// go anyway, and if paging is still too much, another is held in its place
// -- the one that has run longest since it was last held: so the held
// take turns, and none waits for good. A task done lets one go.
// Which is held: a user task ready to run, while one other at least is;
// never one with a window on the display (a GUI ring: the desktop, a
// game), a favoured one (nice), or one with a signal to take.
enum { HOLD_MS = 30000 };

void mmu_letgo(struct task *t) {
    if (!t->held) return;
    t->held = 0;
    TF_CLR(t, TF_HELD);
    sched_wake = 1;
    t->letgo = __time();
    --mmu_nheld;
    klog("mmu: task %d (%s) let go, %d held\n", t->id, t->name, mmu_nheld);
}

// The task held longest, 0 if none is
static struct task *mmu_oldest() {
    struct task *t, *o;
    o = 0;
    t = task_head;
    while (t && mmu_nheld) {
        if (t->held && (!o || t->held - o->held < 0)) o = t;
        t = t->next;
    }
    return o;
}

void mmu_balance() {
    struct task *t, *v;
    int now, dt, hi, lo, n;
    now = __time();
    dt = now - mmu_lwin;
    mmu_lwin = now;
    mmu_nofnew = 0;
    if (klog_cpms <= 0) return;
    // (a window of the machine idle, or the first: calm)
    hi = dt <= 4 * MMU_LOAD_MS && mmu_pgcyc / klog_cpms > dt / 2;
    lo = dt > 4 * MMU_LOAD_MS || mmu_pgcyc / klog_cpms < dt / 8;
    mmu_pgcyc = 0;
    // the one held longest, if paging is light or it has been held long enough
    if ((v = mmu_oldest()) && (lo || now - v->held >= HOLD_MS)) mmu_letgo(v);
    if (!hi) return;
    // one more held: of those that may be, the one let go longest ago
    n = 0;
    v = 0;
    t = task_head;
    while (t) {
        if (TF_HOLDOK(t)) {
            ++n;
            if (!v || t->letgo - v->letgo < 0) v = t;
        }
        t = t->next;
    }
    if (!v || n < 2) return;
    v->held = now ? now : 1;
    TF_SET(v, TF_HELD);
    ++mmu_nheld;
    klog("mmu: paging is most of what the machine does: task %d (%s) held, %d held\n", v->id, v->name, mmu_nheld);
}

// A task done (task_destroy): its memory is free -- a block of frames
// whose pages fit in the others' is emptied into them (mmu_compact) -- and
// one held is let go. (Under the kernel lock, preemption masked: a kernel
// task reaps too -- init -- and a page must not be used while it moves.)
void mmu_gone() {
    struct task *o;
    if (!mmu_on) return;
    sched_lock();
    mmu_compact();
    if ((o = mmu_oldest())) mmu_letgo(o);
    sched_unlock();
}

// A heap block of N bytes, putting out frame blocks whole to make room
// while there are any that can go (a block's memory is the heap's again
// once all its frames are free): 0 if even then there is none. Never the
// pages from KEEP.
static int mmu_reclaim(int n, int keep) {
    int raw;
    if (!mmu_swapok || !sched_has_bkl()) return 0;
    ++mmu_round;
    while (!(raw = (int)malloc(n))) {
        if (!mmu_freeblock(keep)) return 0;
    }
    return raw;
}

// A heap block for NP pages, dropping files' cached bytes (vfs.c) and then
// putting pages out to make room; and whether memory is short now
// (mmu_short: allocation takes the kernel lock, where swap may run). A
// cached file is the cheaper to give up: its bytes are on the host
// already, where a page put out is written.
static int mmu_raw(int np) {
    int raw, used, size;
    while (!(raw = (int)malloc(np * MMU_PAGE + MMU_PAGE - 1)) && vfs_reclaim(np * MMU_PAGE + MMU_PAGE - 1)) ;
    if (!raw && !(raw = mmu_reclaim(np * MMU_PAGE + MMU_PAGE - 1, 0))) return 0;
    if (mmu_swapok) {
        used = *(int *)C4IX_HEAP_USED;
        size = *(int *)C4IX_HEAP_END - *(int *)C4IX_HEAP_BASE;
        mmu_short = used > size / 8 * 7;
    }
    return raw;
}

// The kernel's own memory, N bytes in one run, when there is no such run
// left: pages put out to make it (mmu_reclaim). What a big buffer needs
// -- a program's image as the loader reads it, a file's bytes -- in a
// heap that user memory has cut up; files' cached bytes go first (vfs.c).
// Without swap, or on SMP's light path (no kernel lock), malloc as it is.
char *mmu_kalloc(int n) {
    char *p;
    int q;
    if ((p = (char *)malloc(n)) || !mmu_on || n <= 0) return p;
    while (!(p = (char *)malloc(n)) && vfs_reclaim(n)) ;
    if (p) return p;
    q = mmu_quiet;
    mmu_quiet = 1;
    p = (char *)mmu_reclaim(n, 0);
    mmu_runlog();
    mmu_quiet = q;
    return p;
}

// R's pages made one run of memory, for a device that wants one (its
// access faults otherwise: arena.dma, docs/c4ix-mmu.md 11): a heap block of
// R's size (frame blocks freed to make it if need be), each page copied or
// brought into its place, at the same user addresses. 0 if R is one run
// already, every page in -- the device's fault is another -- or there is no
// such block.
static int mmu_contig(struct urgn *r) {
    int *e, *u;
    int k, va, raw, base, f, s, run;
    // nor one lent to another space, or any of whose pages a device holds
    // (PTE_PIN): their maps and addresses would be left on the old frames
    if (!mmu_uoff || r->kind != UR_BLOCK || !r->heap || r->shared || !r->dir || r->peer[0] || r->peer[1]) return 0;
    run = 1;
    f = 0;
    k = 0;
    while (k < r->npages) {
        e = mmu_pte(mmu_kdir, r->va + k * MMU_PAGE, 0);
        if (e && (*e & PTE_PIN)) return 0;
        run = run && e && (*e & PTE_P) && (!k || (*e & ~(MMU_PAGE - 1)) == f + MMU_PAGE);
        if (e && (*e & PTE_P)) f = *e & ~(MMU_PAGE - 1);
        ++k;
    }
    if (run || !(raw = mmu_raw(r->npages))) return 0;
    base = (raw + MMU_PAGE - 1) & ~(MMU_PAGE - 1);
    if (!mmu_cnew(raw, base, r->npages, 1)) { free((char *)raw); return 0; }
    // out of its space while it moves: no CPU stores to the old frames after
    mmu_map(r->dir, r->va, 0, r->npages * MMU_PAGE, 0);
    // (a CPU that would not answer: the new block back, and the pages are
    // mapped again as they are touched, mmu_fault)
    if (!mmu_shootdown((int)r->dir)) {
        k = 0;
        while (k < r->npages) { mmu_fput(base + k * MMU_PAGE); ++k; }
        return 0;
    }
    k = 0;
    while (k < r->npages) {
        va = r->va + k * MMU_PAGE;
        e = mmu_pte(mmu_kdir, va, 1);
        if (*e & PTE_P) {
            f = *e & ~(MMU_PAGE - 1);
            memcpy((char *)(base + k * MMU_PAGE), (char *)f, MMU_PAGE);
            mmu_fput(f);
        } else if (*e & PTE_SWAP) {
            s = *e >> 12 & 0xfffff;
            mmu_sop(SWAP_IN, s, base + k * MMU_PAGE);
            mmu_sput(s);
            --r->swapped;
        } else memset((char *)(base + k * MMU_PAGE), 0, MMU_PAGE);
        *e = (base + k * MMU_PAGE) | PTE_P | PTE_U | PTE_W;
        mmu_fva[(base >> 12) + k] = va;
        if ((u = mmu_pte(r->dir, va, 1))) *u = (base + k * MMU_PAGE) | r->flags;
        *(int *)MMU_INVLPG = va;
        ++k;
    }
    klock_take(mmu_ulock);
    r->lazy = 0;
    r->raw = raw;
    r->base = base;
    mmu_setpg(r, base, r->npages, r);
    klock_drop(mmu_ulock);
    ++mmu_gen;
    klog("mmu: task %d (%s): its %d KB at 0x%08x made one run of memory, for a device\n",
        r->owner->id, r->owner->name, r->npages * 4, r->va);
    return 1;
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

// ---- the TLB's shootdown ----
//
// A CPU running a space keeps translations of it in its TLB, which a page
// leaving the space would leave behind: before the page's frame is anyone
// else's, every other CPU running the space forgets them. It is asked
// (mmu_shoot) and sent an IPI, which arrives as a trap whatever its mask,
// and the trap's first thing, before it waits for the kernel lock this
// CPU holds, is the flush and the answer (mmu_trap_in). A CPU anywhere but
// user mode is not waited for: it flushes on its way back (mmu_trap_out).
// The flags change by compare-and-swap, a fence, so that each CPU sees the
// other's word in the order it was written.
enum { SHOOT_SPINS = 4000000 };

static void mmu_set(int *p, int v) {
    int o, r;
    o = *p;
    while ((r = __c4_cas((int)p, o, v)) != o) o = r;
}

// What mmu_trap_in, mmu_trap_out and mmu_trap_nested do when this CPU is
// asked to forget its translations (macros, c4ix.h). An IPI inside a
// handler, before it says so or after (sched_trap), does only this, which
// says nothing of where this CPU is.
int mmu_trap_flush() {
    *(int *)MMU_FLUSH = 1;
    mmu_set(&mmu_shoot[cpu_self->id], 0);
    return 0;
}

// Every other CPU with space D loaded has forgotten its translations of
// it, or will before it next runs user mode: 0 if one did not answer
static int mmu_shootdown(int d) {
    int k, me, spins, ok;
    if (smp_ncpu < 2) return 1;
    me = cpu_self->id;
    ok = 1;
    k = 0;
    while (k < smp_ncpu) {
        if (k != me && mmu_loaded[k] == d) {
            mmu_set(&mmu_shoot[k], 1);
            if (mmu_inkernel[k]) ++mmu_shootlater;
            else {
                __c4_ipi(k);
                ++mmu_shootdowns;
                spins = 0;
                while (mmu_shoot[k] && !mmu_inkernel[k] && spins < SHOOT_SPINS) ++spins;
                if (mmu_shoot[k] && !mmu_inkernel[k]) ok = 0;
            }
        }
        ++k;
    }
    return ok;
}

// /proc/meminfo's numbers (vfs.c): the heap's bytes in blocks of single
// frames; swap's slots, those free, and pages put out and brought back
// since boot; the biggest block the heap could hand out now, where the
// host says (libjs: SWAP_CMD 6), else -1; and every block of frames,
// single frames' and regions' own: their bytes, how many, the bytes of
// frames in use in them, and the bytes of blocks less than an eighth in
// use -- a block goes back to the heap only with its last frame, so a
// few pages can keep megabytes
void mmu_counts(int *c) {
    struct fchunk *k;
    c[0] = mmu_poolbytes;
    c[1] = mmu_swapslots;
    c[2] = mmu_sfree;
    c[3] = mmu_swapouts;
    c[4] = mmu_swapins;
    c[5] = host_has(C4IX_I_C4MJS) ? mmu_sop(SWAP_LARGEST, 0, 0) : 0 - 1;
    c[6] = 0; c[7] = 0; c[8] = 0; c[9] = 0;
    klock_take(mmu_flock);
    k = mmu_fcall;
    while (k) {
        c[6] = c[6] + k->npages * MMU_PAGE;
        c[7] = c[7] + 1;
        c[8] = c[8] + k->nused * MMU_PAGE;
        if (k->nused * 8 < k->npages) c[9] = c[9] + k->npages * MMU_PAGE;
        k = k->anext;
    }
    klock_drop(mmu_flock);
}
