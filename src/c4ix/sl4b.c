//
// SL4B: the C4IX slab allocator.
//
// Fixed-size kernel objects (task structs first; fds and vnodes will
// join in X3) come from per-type caches instead of malloc-per-object.
// A cache grows by malloc'ing a slab of `perslab` objects and
// threading them onto a freelist; alloc pops, free pushes. Slabs are
// never returned to the host -- freed objects stay cached for reuse,
// which is the point. So slabs come from the kernel arena (below).
//

#include "c4ix.h"

static struct sl4b_cache *caches;   // all caches, for sl4b_stats

// ---- the kernel arena ----
//
// One block, taken at boot before any program runs, from which the
// kernel carves what it keeps until shutdown (c4ix.h). On libjs the heap
// hands out its lowest addresses first (heap.js's bump pointer), so the
// block sits directly after the kernel image and the va area, below every
// block a program will ever be given. Carving is a bump pointer: nothing
// here is freed, so nothing needs more.
int kmem_base, kmem_end, kmem_brk;
int kmem_spills;             // carvings that did not fit, taken from the heap

// The arena for NCPU processors: the kernel task's stack, an idle task's
// for each CPU, and room for the slabs.
void kmem_init(int ncpu) {
    int bytes;
    bytes = KMEM_STACK_BYTES * (1 + ncpu) + KMEM_SLAB_BYTES;
    if (!(kmem_base = (int)malloc(bytes))) {
        kputs("c4ix: panic: no memory for the kernel arena\n");
        exit(-1);
    }
    memset((char *)kmem_base, 0, bytes);
    kmem_brk = kmem_base;
    kmem_end = kmem_base + bytes;
}

// BYTES of the arena, zeroed, or of the heap once the arena is full
char *kmem_alloc(int bytes) {
    char *p;
    bytes = (bytes + 7) / 8 * 8;
    sched_lock();
    if (kmem_brk && kmem_brk + bytes <= kmem_end) {
        p = (char *)kmem_brk;
        kmem_brk = kmem_brk + bytes;
        sched_unlock();
        return p;
    }
    ++kmem_spills;
    sched_unlock();
    if ((p = mmu_klow(bytes))) memset(p, 0, bytes);
    return p;
}

// free(), for what may have come from kmem_alloc: the arena's own carvings
// stay where they are (a host's free would be handed the middle of a block)
void kmem_free(char *p) {
    if ((int)p >= kmem_base && (int)p < kmem_end) return;
    free(p);
}

// Out of the arena: the heap's, and can go back to it
#define sl4b_outside(p) ((int)(p) < kmem_base || (int)(p) >= kmem_end)

static void sl4b_namecpy(char *dst, char *src) {
    int i;
    i = 0;
    while (src[i] && i < SL4B_NAME_MAX - 1) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

// Add one slab's worth of objects to the freelist.
static int sl4b_grow(struct sl4b_cache *c) {
    struct sl4b_slab *s;
    char *obj;
    int i;

    if (!(s = (struct sl4b_slab *)kmem_alloc(sizeof(struct sl4b_slab) + c->objsize * c->perslab)))
        return 0;
    s->next = c->slabs;
    c->slabs = s;
    ++c->nslabs;
    if (sl4b_outside(s)) ++c->nspilled;

    obj = (char *)s + sizeof(struct sl4b_slab);
    i = 0;
    while (i < c->perslab) {
        *(int *)obj = (int)c->freelist;
        c->freelist = (int *)obj;
        obj = obj + c->objsize;
        ++i;
    }
    return 1;
}

struct sl4b_cache *sl4b_cache_create(char *name, int objsize, int perslab) {
    struct sl4b_cache *c;

    if (!(c = (struct sl4b_cache *)kmem_alloc(sizeof(struct sl4b_cache)))) return 0;
    // round the object size up to whole words: the freelist threads a
    // pointer through the first word of every free object
    c->objsize = (objsize + 7) / 8 * 8;
    if (c->objsize < 8) c->objsize = 8;
    c->perslab = perslab;
    c->slabs = 0;
    c->freelist = 0;
    c->nallocs = 0;
    c->nfrees = 0;
    c->nslabs = 0;
    c->nspilled = 0;
    c->moves = 0;
    sl4b_namecpy(c->name, name);
    c->next = caches;
    caches = c;
    klog("sl4b: cache '%s', %d-byte objects, %d a slab\n", c->name, c->objsize, c->perslab);
    return c;
}

char *sl4b_alloc(struct sl4b_cache *c) {
    int *obj;

    sched_lock();
    if (!c->freelist) {
        if (!sl4b_grow(c)) { sched_unlock(); return 0; }
    }
    obj = c->freelist;
    c->freelist = (int *)*obj;
    ++c->nallocs;
    sched_unlock();
    memset(obj, 0, c->objsize);
    return (char *)obj;
}

void sl4b_free(struct sl4b_cache *c, char *obj) {
    sched_lock();
    *(int *)obj = (int)c->freelist;
    c->freelist = (int *)obj;
    ++c->nfrees;
    sched_unlock();
}

// ---- slabs given back ----
//
// When the arena is full a slab comes from the heap, wherever the heap has room,
// and at the top of a crowd of programs that is the middle of what will be free
// memory once they have gone. Nothing asks for a slab back when its objects are
// freed: alloc and free are a pop and a push, and stay that. So now and then
// (mmu_tidy, at a trap's start, when a task has gone) each cache out of the arena
// has its free objects counted against its slabs, and from the highest slab down:
//
//   - with every object free, it goes back to the heap;
//   - in a cache whose objects can move (c->moves), what is alive in it is copied to
//     free objects lower down, the arena's first, and everything that names them told
//     (mmu_rmove); then it goes back too. As long as enough is left free for the next
//     few allocations not to take a slab straight back.
//
// The free list is left lowest first, so what is allocated next is low too.
// This works on raw memory, with or without the MMU. Task structs, vnodes and files
// do not move: raw pointers to them are everywhere, on kernel tasks' stacks too. Their
// slabs go back when they are empty. Nothing here costs alloc or free anything.
enum { SL4B_TIDY_MOVES = 256 };   // objects moved at a time, at most: a record's owner's list is walked for each

// The slab out of the arena that holds object OBJ
static struct sl4b_slab *sl4b_slabof(struct sl4b_cache *c, int obj) {
    struct sl4b_slab *s;
    int a, n;
    n = c->objsize * c->perslab;
    s = c->slabs;
    while (s) {
        a = (int)s + sizeof(struct sl4b_slab);
        if (obj >= a && obj < a + n) return s;
        s = s->next;
    }
    return 0;
}

// Its slabs, the highest first
static void sl4b_sort(struct sl4b_cache *c) {
    struct sl4b_slab *s, *n, *to;
    struct sl4b_slab **pp;
    to = 0;
    s = c->slabs;
    while (s) {
        n = s->next;
        pp = &to;
        while (*pp && (int)*pp > (int)s) pp = &(*pp)->next;
        s->next = *pp;
        *pp = s;
        s = n;
    }
    c->slabs = to;
}

static int sl4b_tidy1(struct sl4b_cache *c) {
    struct sl4b_slab *s, *d, *e;
    struct sl4b_slab **pp;
    int *arena, *tail, *o, *n, *rest;
    int i, k, navail, moved, given, more, p, q, a;
    if (!c->nspilled) return 0;
    sl4b_sort(c);
    s = c->slabs;
    while (s) { s->nfree = 0; s->free[0] = 0; s->free[1] = 0; s = s->next; }
    // the free objects: the arena's stay a list, the others a bit in their slab
    arena = 0; tail = 0; navail = 0;
    o = c->freelist;
    while (o) {
        n = (int *)*o;
        if (sl4b_outside(o) && (s = sl4b_slabof(c, (int)o))) {
            i = ((int)o - ((int)s + sizeof(struct sl4b_slab))) / c->objsize;
            s->free[i >> 5] = s->free[i >> 5] | (1 << (i & 31));
            ++s->nfree;
        } else {
            *o = (int)arena;
            arena = o;
            if (!tail) tail = o;
        }
        ++navail;
        o = n;
    }
    moved = 0; given = 0; more = 0;
    pp = &c->slabs;
    while ((s = *pp)) {
        if (!sl4b_outside(s)) { pp = &s->next; continue; }
        // (nothing is moved into this slab or above it)
        navail = navail - s->nfree;
        k = c->perslab - s->nfree;
        if (k && c->moves && navail - k >= c->perslab / 2) {
            if (moved + k > SL4B_TIDY_MOVES) more = 1;
            else {
                a = (int)s + sizeof(struct sl4b_slab);
                i = 0;
                while (i < c->perslab) {
                    if (!(s->free[i >> 5] & (1 << (i & 31)))) {
                        // the arena's first, then the lowest slab with room
                        if (arena) {
                            q = (int)arena;
                            arena = (int *)*arena;
                            if (!arena) tail = 0;
                        } else {
                            d = 0;
                            e = s->next;
                            while (e) { if (sl4b_outside(e) && e->nfree) d = e; e = e->next; }
                            k = 0;
                            while (!(d->free[k >> 5] & (1 << (k & 31)))) ++k;
                            d->free[k >> 5] = d->free[k >> 5] & ~(1 << (k & 31));
                            --d->nfree;
                            q = (int)d + sizeof(struct sl4b_slab) + k * c->objsize;
                        }
                        --navail;
                        p = a + i * c->objsize;
                        memcpy((char *)q, (char *)p, c->objsize);
                        mmu_rmove((char *)p, (char *)q);
                        s->free[i >> 5] = s->free[i >> 5] | (1 << (i & 31));
                        ++s->nfree;
                        ++moved;
                    }
                    ++i;
                }
            }
        }
        if (s->nfree == c->perslab) {
            *pp = s->next;
            free((char *)s);
            --c->nslabs;
            --c->nspilled;
            ++given;
            continue;
        }
        pp = &s->next;
    }
    // the free list again: the arena's, then the slabs that are left, lowest first
    rest = 0;
    s = c->slabs;
    while (s) {
        if (sl4b_outside(s)) {
            i = c->perslab;
            while (i) {
                --i;
                if (s->free[i >> 5] & (1 << (i & 31))) {
                    o = (int *)((int)s + sizeof(struct sl4b_slab) + i * c->objsize);
                    *o = (int)rest;
                    rest = o;
                }
            }
        }
        s = s->next;
    }
    if (tail) { *tail = (int)rest; c->freelist = arena; }
    else c->freelist = rest;
    if (moved || given) klog("sl4b: cache '%s': %d objects moved, %d slabs back to the heap\n", c->name, moved, given);
    return more;
}

// Every cache's. The caller has the kernel lock, the light path off it (mmu_still),
// and no object of a moving cache in hand.
int sl4b_tidy() {
    struct sl4b_cache *c;
    int more;
    more = 0;
    c = caches;
    while (c) {
        if (sl4b_tidy1(c)) more = 1;
        c = c->next;
    }
    return more;
}

int sl4b_spilled() {
    struct sl4b_cache *c;
    int n;
    n = 0;
    c = caches;
    while (c) { n = n + c->nspilled; c = c->next; }
    return n;
}

void sl4b_stats() {
    struct sl4b_cache *c;
    c = caches;
    while (c) {
        kprintf("sl4b: cache '%s' objsize %d perslab %d slabs %d allocs %d frees %d live %d\n",
            c->name, c->objsize, c->perslab, c->nslabs,
            c->nallocs, c->nfrees, c->nallocs - c->nfrees);
        c = c->next;
    }
    if (kmem_spills)
        kprintf("sl4b: the kernel arena (%d KB) filled: %d carvings came from the heap\n",
            (kmem_end - kmem_base) / 1024, kmem_spills);
}
