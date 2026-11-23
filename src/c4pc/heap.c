/* heap.c -- the host side of MALC / FREE / RALC: libjs/heap.js in C,
 * block for block, so that a guest gets the same addresses on both.
 *
 * The arena holds only the guest's bytes; the books are here: a table of
 * live blocks (address -> requested size, span, generation and when it
 * was tagged), a stack of free blocks for each small size class, and the
 * free ranges for large blocks, sorted by address and merged. See
 * heap.js for why each rule is what it is.
 */
#include "c4pc.h"
#include "clib.h"

#define SMALL_MAX 4096
#define NCLASS 9                         /* 16 .. 4096 */

typedef struct { u32 addr; i32 size; u32 span; i32 gen; u32 seq; u8 st; } blk;   /* st: 0 empty, 1 live, 2 tomb */
typedef struct { u32 addr, len; } range;
typedef struct { u32 *v; u32 n, cap; } stack;

struct heap {
  u32 base, end, brk, top;
  int high;
  blk *t; u32 tcap, tn, tused;           /* live blocks */
  stack small[NCLASS];
  int class_order[NCLASS], nclass_seen;  /* the order classes were first used: heap.js's Map */
  range *large; u32 nlarge, large_cap;
  i32 used, peak, count, gen, released;
  u32 seq;
  int locked; volatile int lk;           /* every CPU's at once (par.c) */
};

static int class_index(u32 n, u32 *span) {
  u32 c = 16; int i = 0;
  while (c < n) { c <<= 1; i++; }
  *span = c;
  return i;
}
static u32 bhash(u32 x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }

heap *heap_new(void) {
  heap *h = xmalloc(sizeof *h);
  h->tcap = 1024;
  h->t = xmalloc(h->tcap * sizeof *h->t);
  return h;
}

void heap_reset(heap *h, u32 base, u32 end) {
  h->base = (base + 7) & ~7u;
  h->end = end & ~7u;
  h->brk = h->base;
  h->top = h->end;
  h->high = 0;
  memset(h->t, 0, h->tcap * sizeof *h->t);
  h->tn = h->tused = 0;
  for (int i = 0; i < NCLASS; i++) h->small[i].n = 0;
  h->nclass_seen = 0;
  h->nlarge = 0;
  h->used = h->peak = h->count = h->gen = h->released = 0;
  h->seq = 0;
}

/* ---- the table of live blocks ---- */
static blk *tfind(heap *h, u32 addr) {
  u32 j = bhash(addr) & (h->tcap - 1);
  while (h->t[j].st) {
    if (h->t[j].st == 1 && h->t[j].addr == addr) return &h->t[j];
    j = (j + 1) & (h->tcap - 1);
  }
  return 0;
}
static void tgrow(heap *h) {
  blk *o = h->t; u32 oc = h->tcap;
  h->tcap = h->tn * 4 > oc ? oc * 2 : oc;
  h->t = xmalloc(h->tcap * sizeof *h->t);
  h->tn = h->tused = 0;
  for (u32 i = 0; i < oc; i++) if (o[i].st == 1) {
    u32 j = bhash(o[i].addr) & (h->tcap - 1);
    while (h->t[j].st) j = (j + 1) & (h->tcap - 1);
    h->t[j] = o[i]; h->tn++; h->tused++;
  }
  xfree(o);
}
static blk *tadd(heap *h, u32 addr) {
  if ((h->tused + 1) * 2 > h->tcap) tgrow(h);
  u32 j = bhash(addr) & (h->tcap - 1);
  while (h->t[j].st == 1) j = (j + 1) & (h->tcap - 1);
  if (!h->t[j].st) h->tused++;
  h->tn++;
  blk *b = &h->t[j];
  memset(b, 0, sizeof *b);
  b->st = 1; b->addr = addr;
  return b;
}
static void tdel(heap *h, blk *b) { b->st = 2; h->tn--; }

/* ---- free stacks and ranges ---- */
static void spush(heap *h, int ci, u32 a) {
  stack *s = &h->small[ci];
  int seen = 0;
  for (int i = 0; i < h->nclass_seen; i++) if (h->class_order[i] == ci) seen = 1;
  if (!seen) h->class_order[h->nclass_seen++] = ci;
  if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 64; s->v = xrealloc(s->v, s->cap * 4); }
  s->v[s->n++] = a;
}
static void linsert(heap *h, u32 i, u32 addr, u32 len) {
  if (h->nlarge == h->large_cap) { h->large_cap = h->large_cap ? h->large_cap * 2 : 64; h->large = xrealloc(h->large, h->large_cap * sizeof(range)); }
  memmove(&h->large[i + 1], &h->large[i], (h->nlarge - i) * sizeof(range));
  h->large[i].addr = addr; h->large[i].len = len;
  h->nlarge++;
}
static void lremove(heap *h, u32 i) {
  memmove(&h->large[i], &h->large[i + 1], (h->nlarge - i - 1) * sizeof(range));
  h->nlarge--;
}

static u32 take_large(heap *h, u32 span) {
  for (u32 i = 0; i < h->nlarge; i++) {
    range *r = &h->large[i];
    if (r->len < span) continue;
    u32 a = r->addr;
    if (r->len == span) lremove(h, i);
    else { r->addr += span; r->len -= span; }
    return a;
  }
  return 0;
}
static u32 take_high(heap *h, u32 span) {
  int i = (int)h->nlarge - 1;
  for (; i >= 0 && h->large[i].addr >= h->top; i--) {
    range *r = &h->large[i];
    if (r->len < span) continue;
    r->len -= span;
    u32 a = r->addr + r->len;
    if (!r->len) lremove(h, (u32)i);
    return a;
  }
  if (h->top - span >= h->brk && h->top >= span) { h->top -= span; return h->top; }
  for (; i >= 0; i--) {
    range *r = &h->large[i];
    if (r->len < span) continue;
    r->len -= span;
    u32 a = r->addr + r->len;
    if (!r->len) lremove(h, (u32)i);
    return a;
  }
  return 0;
}

static int rcmp(const void *a, const void *b) {
  u32 x = ((const range *)a)->addr, y = ((const range *)b)->addr;
  return x < y ? -1 : x > y;
}
static u32 consolidate(heap *h) {
  u32 n = h->nlarge;
  for (int c = 0; c < NCLASS; c++) n += h->small[c].n;
  if (n == h->nlarge) return 0;
  range *all = xmalloc(n * sizeof(range));
  u32 k = 0, smalls = 0;
  for (int oi = 0; oi < h->nclass_seen; oi++) {
    int c = h->class_order[oi];
    for (u32 j = 0; j < h->small[c].n; j++) { all[k].addr = h->small[c].v[j]; all[k].len = 16u << c; k++; smalls++; }
    h->small[c].n = 0;
  }
  for (u32 j = 0; j < h->nlarge; j++) all[k++] = h->large[j];
  qsort(all, k, sizeof(range), rcmp);
  u32 m = 0;
  for (u32 j = 0; j < k; j++) {
    if (m && all[m - 1].addr + all[m - 1].len == all[j].addr) all[m - 1].len += all[j].len;
    else all[m++] = all[j];
  }
  /* back to the bump pointers: one pass, in order, as heap.js's splice loop */
  h->nlarge = 0;
  for (u32 j = 0; j < m; j++) {
    if (all[j].addr + all[j].len == h->brk) { h->brk = all[j].addr; continue; }
    if (all[j].addr == h->top) { h->top = all[j].addr + all[j].len; continue; }
    linsert(h, h->nlarge, all[j].addr, all[j].len);
  }
  xfree(all);
  return smalls + 0 * m;
}

static void give_large(heap *h, u32 addr, u32 len) {
  u32 lo = 0, hi = h->nlarge;
  while (lo < hi) { u32 mid = (lo + hi) >> 1; if (h->large[mid].addr < addr) lo = mid + 1; else hi = mid; }
  linsert(h, lo, addr, len);
  range *L = h->large;
  if (lo + 1 < h->nlarge && L[lo].addr + L[lo].len == L[lo + 1].addr) { L[lo].len += L[lo + 1].len; lremove(h, lo + 1); L = h->large; }
  if (lo > 0 && L[lo - 1].addr + L[lo - 1].len == L[lo].addr) { L[lo - 1].len += L[lo].len; lremove(h, lo); L = h->large; lo--; }
  range r = L[lo];
  if (r.addr + r.len == h->brk) { h->brk = r.addr; lremove(h, lo); }
  else if (r.addr == h->top) { h->top = r.addr + r.len; lremove(h, lo); }
}

static u32 take(heap *h, i32 n) {
  u32 a = 0, span = 0;
  int ci = -1;
  if (n <= SMALL_MAX) {
    ci = class_index((u32)n, &span);
    stack *s = &h->small[ci];
    if (s->n) a = s->v[--s->n];
  } else {
    span = ((u32)n + 7) & ~7u;
    a = h->high ? take_high(h, span) : take_large(h, span);
  }
  if (!a) {
    if ((i64)h->brk + span <= h->top) { a = h->brk; h->brk += span; }
    else if (span <= SMALL_MAX) a = take_large(h, span);
    if (!a) return 0;
  }
  blk *b = tadd(h, a);
  b->size = n; b->span = span; b->gen = h->gen;
  if (h->gen) b->seq = ++h->seq;
  h->used += n;
  h->count++;
  if (h->used > h->peak) h->peak = h->used;
  return a;
}

static u32 alloc_(heap *h, i32 n) {
  if (n <= 0) return 0;
  u32 a = take(h, n);
  if (a || !consolidate(h)) return a;
  return take(h, n);
}

static void free_(heap *h, u32 p) {
  if (!p) return;
  blk *b = tfind(h, p);
  if (!b) return;
  u32 span = b->span;
  h->used -= b->size;
  h->count--;
  tdel(h, b);
  if (span <= SMALL_MAX) { u32 s; spush(h, class_index(span, &s), p); }
  else give_large(h, p, span);
}

static u32 realloc_(heap *h, u8 *mem, u32 p, i32 n) {
  if (!p) return alloc_(h, n);
  if (n <= 0) { free_(h, p); return 0; }
  blk *b = tfind(h, p);
  if (!b) return 0;
  i32 old = b->size;
  if ((u32)n <= b->span) {
    h->used += n - old;
    if (h->used > h->peak) h->peak = h->used;
    b->size = n;
    return p;
  }
  i32 g = b->gen;
  u32 q = alloc_(h, n);
  if (!q) return 0;
  blk *nb = tfind(h, q);
  if (g != h->gen) { nb->gen = g; nb->seq = g ? ++h->seq : 0; }
  memmove(mem + q, mem + p, (size_t)(old < n ? old : n));
  free_(h, p);
  return q;
}

void heap_set_high(heap *h, int on) { h->high = !!on; }

static u32 largest_(heap *h) {
  consolidate(h);
  u32 n = h->top - h->brk;
  for (u32 i = 0; i < h->nlarge; i++) if (h->large[i].len > n) n = h->large[i].len;
  return n;
}

static int seqcmp(const void *a, const void *b) {
  u32 x = ((const blk *)a)->seq, y = ((const blk *)b)->seq;
  return x < y ? -1 : x > y;
}
static i32 release_(heap *h, i32 g) {
  h->released = 0;
  if (!g) return 0;
  u32 k = 0;
  for (u32 i = 0; i < h->tcap; i++) if (h->t[i].st == 1 && h->t[i].gen == g) k++;
  if (!k) return 0;
  blk *list = xmalloc(k * sizeof(blk));
  k = 0;
  for (u32 i = 0; i < h->tcap; i++) if (h->t[i].st == 1 && h->t[i].gen == g) list[k++] = h->t[i];
  qsort(list, k, sizeof(blk), seqcmp);
  i32 before = h->used;
  for (u32 i = 0; i < k; i++) free_(h, list[i].addr);
  xfree(list);
  h->released = before - h->used;
  return h->released;
}

i32 heap_used(heap *h) { return h->used; }
i32 heap_peak(heap *h) { return h->peak; }
i32 heap_gen(heap *h) { return h->gen; }
i32 heap_released(heap *h) { return h->released; }
void heap_set_gen(heap *h, i32 g) { h->gen = g; }
u32 heap_base(heap *h) { return h->base; }
u32 heap_end(heap *h) { return h->end; }

/* ---- the public calls: under a lock while CPUs share the heap (par.c) ---- */
static void lock(heap *h) {
  if (!h->locked) return;
  while (__atomic_exchange_n(&h->lk, 1, __ATOMIC_ACQUIRE)) while (__atomic_load_n(&h->lk, __ATOMIC_RELAXED)) __builtin_ia32_pause();
}
static void unlock(heap *h) { if (h->locked) __atomic_store_n(&h->lk, 0, __ATOMIC_RELEASE); }
void heap_set_locked(heap *h, int on) { h->locked = on; }
u32 heap_alloc(heap *h, i32 n) { lock(h); u32 r = alloc_(h, n); unlock(h); return r; }
void heap_free(heap *h, u32 p) { lock(h); free_(h, p); unlock(h); }
u32 heap_realloc(heap *h, u8 *mem, u32 p, i32 n) { lock(h); u32 r = realloc_(h, mem, p, n); unlock(h); return r; }
u32 heap_largest(heap *h) { lock(h); u32 r = largest_(h); unlock(h); return r; }
i32 heap_release(heap *h, i32 g) { lock(h); i32 r = release_(h, g); unlock(h); return r; }
