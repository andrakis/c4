/* util.c -- allocation, the maps c4pc keeps its books in, file blobs. */
#include "c4pc.h"
#include "clib.h"

void *xmalloc(size_t n) {
  void *p = calloc(1, n ? n : 1);
  if (!p) plat_fatal("c4pc: out of host memory");
  return p;
}
void *xrealloc(void *p, size_t n) {
  void *q = realloc(p, n ? n : 1);
  if (!q) plat_fatal("c4pc: out of host memory");
  return q;
}
char *xstrdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = xmalloc(n);
  memcpy(p, s, n);
  return p;
}
void xfree(void *p) { free(p); }

/* ---- blobs ---- */
blob *blob_new(u32 len) {
  blob *b = xmalloc(sizeof *b);
  b->refs = 1;
  b->len = len;
  b->bytes = xmalloc(len ? len : 1);
  return b;
}
blob *blob_static(u8 *bytes, u32 len) {
  blob *b = xmalloc(sizeof *b);
  b->refs = -1;
  b->len = len;
  b->bytes = bytes;
  return b;
}
void blob_ref(blob *b) { if (b && b->refs > 0) b->refs++; }
void blob_unref(blob *b) {
  if (!b || b->refs < 0) return;
  if (--b->refs == 0) { xfree(b->bytes); xfree(b); }
}

/* ---- string -> pointer: open addressing, linear probing ---- */
struct smap { u32 cap, n, used; char **k; void **v; };
static char TOMB[1];

static u32 shash(const char *s) {
  u32 h = 2166136261u;
  while (*s) { h ^= (u8)*s++; h *= 16777619u; }
  return h;
}
smap *smap_new(void) {
  smap *m = xmalloc(sizeof *m);
  m->cap = 64;
  m->k = xmalloc(m->cap * sizeof *m->k);
  m->v = xmalloc(m->cap * sizeof *m->v);
  return m;
}
static void smap_grow(smap *m) {
  u32 oc = m->cap;
  char **ok = m->k; void **ov = m->v;
  m->cap = m->n * 4 > oc ? oc * 2 : oc;
  m->k = xmalloc(m->cap * sizeof *m->k);
  m->v = xmalloc(m->cap * sizeof *m->v);
  m->n = m->used = 0;
  for (u32 i = 0; i < oc; i++) if (ok[i] && ok[i] != TOMB) {
    u32 j = shash(ok[i]) & (m->cap - 1);
    while (m->k[j]) j = (j + 1) & (m->cap - 1);
    m->k[j] = ok[i]; m->v[j] = ov[i]; m->n++; m->used++;
  }
  xfree(ok); xfree(ov);
}
static int smap_find(smap *m, const char *k) {
  u32 j = shash(k) & (m->cap - 1);
  while (m->k[j]) {
    if (m->k[j] != TOMB && !strcmp(m->k[j], k)) return (int)j;
    j = (j + 1) & (m->cap - 1);
  }
  return -1;
}
void *smap_get(smap *m, const char *k) { int j = smap_find(m, k); return j < 0 ? 0 : m->v[j]; }
void smap_set(smap *m, const char *k, void *v) {
  int j = smap_find(m, k);
  if (j >= 0) { m->v[j] = v; return; }
  if ((m->used + 1) * 2 > m->cap) smap_grow(m);
  u32 i = shash(k) & (m->cap - 1);
  while (m->k[i] && m->k[i] != TOMB) i = (i + 1) & (m->cap - 1);
  if (!m->k[i]) m->used++;
  m->k[i] = xstrdup(k); m->v[i] = v; m->n++;
}
void *smap_del(smap *m, const char *k) {
  int j = smap_find(m, k);
  if (j < 0) return 0;
  void *v = m->v[j];
  xfree(m->k[j]);
  m->k[j] = TOMB; m->v[j] = 0; m->n--;
  return v;
}

/* ---- u32 -> pointer: the same, keyed by number (0 is a key too) ---- */
struct imap { u32 cap, n, used; u32 *k; u8 *st; void **v; };   /* st: 0 empty, 1 full, 2 tomb */
static u32 ihash(u32 x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
imap *imap_new(void) {
  imap *m = xmalloc(sizeof *m);
  m->cap = 64;
  m->k = xmalloc(m->cap * 4); m->st = xmalloc(m->cap); m->v = xmalloc(m->cap * sizeof(void *));
  return m;
}
static void imap_grow(imap *m) {
  u32 oc = m->cap, *ok = m->k; u8 *os = m->st; void **ov = m->v;
  m->cap = m->n * 4 > oc ? oc * 2 : oc;
  m->k = xmalloc(m->cap * 4); m->st = xmalloc(m->cap); m->v = xmalloc(m->cap * sizeof(void *));
  m->n = m->used = 0;
  for (u32 i = 0; i < oc; i++) if (os[i] == 1) {
    u32 j = ihash(ok[i]) & (m->cap - 1);
    while (m->st[j]) j = (j + 1) & (m->cap - 1);
    m->k[j] = ok[i]; m->st[j] = 1; m->v[j] = ov[i]; m->n++; m->used++;
  }
  xfree(ok); xfree(os); xfree(ov);
}
static int imap_find(imap *m, u32 k) {
  u32 j = ihash(k) & (m->cap - 1);
  while (m->st[j]) {
    if (m->st[j] == 1 && m->k[j] == k) return (int)j;
    j = (j + 1) & (m->cap - 1);
  }
  return -1;
}
void *imap_get(imap *m, u32 k) { int j = imap_find(m, k); return j < 0 ? 0 : m->v[j]; }
void imap_set(imap *m, u32 k, void *v) {
  int j = imap_find(m, k);
  if (j >= 0) { m->v[j] = v; return; }
  if ((m->used + 1) * 2 > m->cap) imap_grow(m);
  u32 i = ihash(k) & (m->cap - 1);
  while (m->st[i] == 1) i = (i + 1) & (m->cap - 1);
  if (!m->st[i]) m->used++;
  m->k[i] = k; m->st[i] = 1; m->v[i] = v; m->n++;
}
void *imap_del(imap *m, u32 k) {
  int j = imap_find(m, k);
  if (j < 0) return 0;
  void *v = m->v[j];
  m->st[j] = 2; m->v[j] = 0; m->n--;
  return v;
}
void imap_clear(imap *m, void (*f)(void *)) {
  for (u32 i = 0; i < m->cap; i++) {
    if (m->st[i] == 1 && f) f(m->v[i]);
    m->st[i] = 0; m->v[i] = 0;
  }
  m->n = m->used = 0;
}
