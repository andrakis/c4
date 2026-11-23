/* libc.c -- the little C library c4pc needs on a bare PC: string and
 * memory functions, a malloc over the memory the bootloader reported,
 * and qsort. */
#include "../c4pc.h"
#include "../clib.h"

void *memcpy(void *d, const void *s, size_t n) {
  u8 *a = d; const u8 *b = s;
  if (!(((uintptr_t)a | (uintptr_t)b) & 3)) {
    size_t w = n >> 2;
    __asm__ volatile("rep movsl" : "+D"(a), "+S"(b), "+c"(w) : : "memory");
    n &= 3;
  }
  __asm__ volatile("rep movsb" : "+D"(a), "+S"(b), "+c"(n) : : "memory");
  return d;
}
void *memmove(void *d, const void *s, size_t n) {
  u8 *a = d; const u8 *b = s;
  if (a == b || !n) return d;
  if (a < b || a >= b + n) return memcpy(d, s, n);
  a += n - 1; b += n - 1;
  __asm__ volatile("std; rep movsb; cld" : "+D"(a), "+S"(b), "+c"(n) : : "memory");
  return d;
}
void *memset(void *d, int c, size_t n) {
  u8 *a = d;
  if (!((uintptr_t)a & 3) && n >= 16) {
    u32 v = (u8)c; v |= v << 8; v |= v << 16;
    size_t w = n >> 2;
    __asm__ volatile("rep stosl" : "+D"(a), "+c"(w) : "a"(v) : "memory");
    n &= 3;
  }
  __asm__ volatile("rep stosb" : "+D"(a), "+c"(n) : "a"(c) : "memory");
  return d;
}
int memcmp(const void *x, const void *y, size_t n) {
  const u8 *a = x, *b = y;
  for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return a[i] - b[i];
  return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
  while (*a && *a == *b) a++, b++;
  return (u8)*a - (u8)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (a[i] != b[i] || !a[i]) return (u8)a[i] - (u8)b[i];
  }
  return 0;
}
char *strchr(const char *s, int c) {
  for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; }
}
char *strrchr(const char *s, int c) {
  const char *r = 0;
  for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char *)r; }
}

/* ---- malloc: first fit over a free list, with boundary tags so that a
 * freed block merges with its neighbours. A block is a header word (its
 * size, bit 0 set while in use), the payload, and a footer word the same. */
typedef struct fb { u32 size; u32 pad; struct fb *next, *prev; } fb;
static fb *free_head;
static u8 *arena_lo, *arena_hi;
u32 pc_heap_free_bytes;

#define HDR 8
#define FTR 8
#define MINB 32
static u32 *footer(fb *b) { return (u32 *)((u8 *)b + (b->size & ~7u) - FTR); }
static void list_add(fb *b) { b->next = free_head; b->prev = 0; if (free_head) free_head->prev = b; free_head = b; }
static void list_del(fb *b) {
  if (b->prev) b->prev->next = b->next; else free_head = b->next;
  if (b->next) b->next->prev = b->prev;
}
static void set_block(fb *b, u32 size, int used) { b->size = size | (u32)used; *footer(b) = size | (u32)used; }

void pc_malloc_init(void *lo, void *hi) {
  u8 *a = (u8 *)(((uintptr_t)lo + 15) & ~(uintptr_t)15), *z = (u8 *)((uintptr_t)hi & ~(uintptr_t)15);
  /* a used word at each end stops the merging */
  *(u32 *)a = 1; a += 8;
  z -= 8; *(u32 *)z = 1; ((u32 *)z)[1] = 1;
  arena_lo = a; arena_hi = z;
  fb *b = (fb *)a;
  set_block(b, (u32)(z - a), 0);
  list_add(b);
  pc_heap_free_bytes = b->size;
}

static void *malloc_(size_t n) {
  if (n > 0x7fffff00u) return 0;
  u32 need = ((u32)n + HDR + FTR + 15) & ~15u;
  if (need < MINB) need = MINB;
  for (fb *b = free_head; b; b = b->next) {
    if (b->size < need) continue;
    list_del(b);
    u32 rest = b->size - need;
    if (rest >= MINB) {
      fb *r = (fb *)((u8 *)b + need);
      set_block(r, rest, 0);
      list_add(r);
      set_block(b, need, 1);
    } else set_block(b, b->size, 1);
    pc_heap_free_bytes -= b->size & ~7u;
    return (u8 *)b + HDR;
  }
  return 0;
}
static void free_(void *p) {
  if (!p) return;
  fb *b = (fb *)((u8 *)p - HDR);
  u32 size = b->size & ~7u;
  pc_heap_free_bytes += size;
  /* the next block */
  fb *n = (fb *)((u8 *)b + size);
  if ((u8 *)n < arena_hi && !(n->size & 1)) { list_del(n); size += n->size; }
  /* the previous: its footer is the word before this header */
  u32 pf = *((u32 *)b - 2);
  if ((u8 *)b > arena_lo && !(pf & 1)) {
    fb *pb = (fb *)((u8 *)b - (pf & ~7u));
    list_del(pb);
    size += pb->size;
    b = pb;
  }
  set_block(b, size, 0);
  list_add(b);
}
/* the processors share it (ap.c, par.c): a lock */
static volatile int mlk;
static void mlock(void) { while (__atomic_exchange_n(&mlk, 1, __ATOMIC_ACQUIRE)) while (__atomic_load_n(&mlk, __ATOMIC_RELAXED)) __builtin_ia32_pause(); }
static void munlock(void) { __atomic_store_n(&mlk, 0, __ATOMIC_RELEASE); }
void *malloc(size_t n) { mlock(); void *p = malloc_(n); munlock(); return p; }
void free(void *p) { mlock(); free_(p); munlock(); }
void *calloc(size_t n, size_t k) {
  size_t t = n * k;
  void *p = malloc(t);
  if (p) memset(p, 0, t);
  return p;
}
void *realloc(void *p, size_t n) {
  if (!p) return malloc(n);
  fb *b = (fb *)((u8 *)p - HDR);
  size_t have = (b->size & ~7u) - HDR - FTR;
  if (n <= have) return p;
  void *q = malloc(n);
  if (!q) return 0;
  memcpy(q, p, have);
  free(p);
  return q;
}

/* heapsort: in place, no recursion */
static void swapb(u8 *a, u8 *b, size_t n) { while (n--) { u8 t = *a; *a++ = *b; *b++ = t; } }
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
  u8 *v = base;
  if (n < 2) return;
  for (size_t start = n / 2; start-- > 0;) {
    size_t root = start;
    for (;;) {
      size_t c = 2 * root + 1;
      if (c >= n) break;
      if (c + 1 < n && cmp(v + c * sz, v + (c + 1) * sz) < 0) c++;
      if (cmp(v + root * sz, v + c * sz) >= 0) break;
      swapb(v + root * sz, v + c * sz, sz); root = c;
    }
  }
  for (size_t end = n - 1; end > 0; end--) {
    swapb(v, v + end * sz, sz);
    size_t root = 0;
    for (;;) {
      size_t c = 2 * root + 1;
      if (c >= end) break;
      if (c + 1 < end && cmp(v + c * sz, v + (c + 1) * sz) < 0) c++;
      if (cmp(v + root * sz, v + c * sz) >= 0) break;
      swapb(v + root * sz, v + c * sz, sz); root = c;
    }
  }
}

/* 64-bit division, which gcc leaves to libgcc on i386. Not libgcc's own:
 * Ubuntu builds it with CET, and DOSBox's cores know no ENDBR32. A
 * divisor that fits in 32 bits (every one c4pc has) is two DIVs. */
static uint64_t udivmod64(uint64_t n, uint64_t d, uint64_t *rem) {
  if (!(d >> 32)) {
    u32 dd = (u32)d, hi = (u32)(n >> 32), lo = (u32)n, qhi, qlo, r;
    if (!dd) { if (rem) *rem = 0; return 0; }
    qhi = hi / dd; r = hi % dd;
    __asm__("divl %4" : "=a"(qlo), "=d"(r) : "a"(lo), "d"(r), "rm"(dd));
    if (rem) *rem = r;
    return (uint64_t)qhi << 32 | qlo;
  }
  uint64_t q = 0, r = 0;
  for (int i = 63; i >= 0; i--) {
    r = r << 1 | ((n >> i) & 1);
    if (r >= d) { r -= d; q |= 1ull << i; }
  }
  if (rem) *rem = r;
  return q;
}
uint64_t __udivdi3(uint64_t n, uint64_t d) { return udivmod64(n, d, 0); }
uint64_t __umoddi3(uint64_t n, uint64_t d) { uint64_t r; udivmod64(n, d, &r); return r; }
int64_t __divdi3(int64_t n, int64_t d) {
  int neg = (n < 0) != (d < 0);
  uint64_t q = udivmod64(n < 0 ? 0 - (uint64_t)n : (uint64_t)n, d < 0 ? 0 - (uint64_t)d : (uint64_t)d, 0);
  return neg ? (int64_t)(0 - q) : (int64_t)q;
}
int64_t __moddi3(int64_t n, int64_t d) {
  uint64_t r;
  udivmod64(n < 0 ? 0 - (uint64_t)n : (uint64_t)n, d < 0 ? 0 - (uint64_t)d : (uint64_t)d, &r);
  return n < 0 ? (int64_t)(0 - r) : (int64_t)r;
}
int64_t __divmoddi4(int64_t n, int64_t d, int64_t *rem) {
  int64_t q = __divdi3(n, d);
  if (rem) *rem = n - q * d;
  return q;
}
