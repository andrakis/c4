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
    if ((p = (char *)malloc(bytes))) memset(p, 0, bytes);
    return p;
}

// free(), for what may have come from kmem_alloc: the arena's own carvings
// stay where they are (a host's free would be handed the middle of a block)
void kmem_free(char *p) {
    if ((int)p >= kmem_base && (int)p < kmem_end) return;
    free(p);
}

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
    sl4b_namecpy(c->name, name);
    c->next = caches;
    caches = c;
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
