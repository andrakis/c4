//
// SL4B: the C4IX slab allocator.
//
// Fixed-size kernel objects (task structs first; fds and vnodes will
// join in X3) come from per-type caches instead of malloc-per-object.
// A cache grows by malloc'ing a slab of `perslab` objects and
// threading them onto a freelist; alloc pops, free pushes. Slabs are
// never returned to the host -- freed objects stay cached for reuse,
// which is the point.
//

#include "c4ix.h"

static struct sl4b_cache *caches;   // all caches, for sl4b_stats

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

    if (!(s = (struct sl4b_slab *)malloc(sizeof(struct sl4b_slab) + c->objsize * c->perslab)))
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

    if (!(c = (struct sl4b_cache *)malloc(sizeof(struct sl4b_cache)))) return 0;
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
}
