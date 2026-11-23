/* drive.c -- a drive: named files in memory (c4bb's drives, devices.js).
 * On the PC a drive comes from a ustar archive the bootloader loaded as a
 * module (make c4pc-drives); its files stay where the module is.
 *
 * A member named NAME.c4pcz is NAME, compressed (pc/packdisk.py: the
 * ISO's boot disk, a third of its size): its size is the gzip trailer's,
 * and it is inflated (inflate.c) the first time it is read, into memory of
 * its own (blob_ready, which dev.c's disk_find asks). What is inflated is
 * kept while it fits Z_BUDGET; past that the one least recently read and
 * not open is let go of, to be inflated again if it is wanted. */
#include "c4pc.h"
#include "clib.h"

drive *drive_new(int writable) {
  drive *d = xmalloc(sizeof *d);
  d->files = smap_new();
  d->writable = writable;
  return d;
}

void drive_put(drive *d, const char *name, blob *b) {
  blob *old = smap_get(d->files, name);
  smap_set(d->files, name, b);
  if (old) blob_unref(old);
}

/* ---- the compressed files ---- */
enum { Z_BUDGET = 96 << 20 };
static blob **zs; static int nz, zcap;
static u32 z_clock, z_held;

static blob *blob_z(const u8 *z, u32 zlen, u32 len) {
  blob *b = xmalloc(sizeof *b);
  b->refs = 1;                               /* the drive's; an open file's on top */
  b->len = len;
  b->z = z; b->zlen = zlen;
  if (nz == zcap) { zcap = zcap ? zcap * 2 : 256; zs = xrealloc(zs, (size_t)zcap * sizeof *zs); }
  b->zslot = nz;
  zs[nz++] = b;
  return b;
}
/* its place among them let go (blob_unref, when its last reference goes) */
void drive_zforget(blob *b) {
  if (b->zslot >= 0 && b->zslot < nz && zs[b->zslot] == b) zs[b->zslot] = 0;
  if (b->bytes) z_held -= b->len;
}
/* what is inflated past the budget let go of: the least recently read
 * first, never one open (a reference besides the drive's) nor KEEP */
static void z_trim(blob *keep, u32 budget) {
  while (z_held > budget) {
    blob *old = 0;
    for (int i = 0; i < nz; i++) {
      blob *c = zs[i];
      if (c && c != keep && c->bytes && c->refs == 1 && (!old || c->used < old->used)) old = c;
    }
    if (!old) return;
    xfree(old->bytes);
    old->bytes = 0;
    z_held -= old->len;
  }
}
int blob_ready(blob *b) {
  if (!b) return 0;
  if (b->bytes) { if (b->z) b->used = ++z_clock; return 1; }
  if (!b->z) return 0;
  u8 *o = malloc(b->len ? b->len : 1);
  /* no room: every inflated one not open let go of, and once more */
  if (!o) { z_trim(0, 0); if (!(o = malloc(b->len ? b->len : 1))) return 0; }
  if (gz_inflate(b->z, b->zlen, o, b->len)) { free(o); return 0; }
  b->bytes = o;
  b->used = ++z_clock;
  z_held += b->len;
  z_trim(b, Z_BUDGET);
  return 1;
}

static u32 octal(const u8 *p, int n) {
  u32 v = 0;
  for (int i = 0; i < n && p[i]; i++) if (p[i] >= '0' && p[i] <= '7') v = v * 8 + (u32)(p[i] - '0');
  return v;
}

/* ustar, with GNU's long names ('L'): regular files only, by their path
 * less any leading "./" (cli.js loadDisk's keys) */
int drive_add_tar(drive *d, u8 *tar, u32 len) {
  u32 p = 0; int n = 0;
  char longname[1024]; int have_long = 0;
  while (p + 512 <= len) {
    const u8 *h = tar + p;
    if (!h[0]) break;
    u32 size = octal(h + 124, 12);
    int type = h[156];
    u32 data = p + 512;
    if (data + size > len) break;
    char name[1024];
    if (type == 'L') {
      u32 k = size < sizeof longname - 1 ? size : sizeof longname - 1;
      memcpy(longname, tar + data, k); longname[k] = 0;
      have_long = 1;
    } else {
      if (have_long) { memcpy(name, longname, sizeof name); have_long = 0; }
      else {
        int k = 0;
        if (!memcmp(h + 257, "ustar", 5) && h[345]) {
          for (int i = 0; i < 155 && h[345 + i]; i++) name[k++] = (char)h[345 + i];
          name[k++] = '/';
        }
        for (int i = 0; i < 100 && h[i]; i++) name[k++] = (char)h[i];
        name[k] = 0;
      }
      if (type == '0' || type == 0) {
        char *s = name;
        while (s[0] == '.' && s[1] == '/') s += 2;
        while (*s == '/') s++;
        size_t sl = strlen(s);
        u32 ulen;
        if (sl > 6 && !strcmp(s + sl - 6, ".c4pcz") && !gz_size(tar + data, size, &ulen)) {
          s[sl - 6] = 0;
          drive_put(d, s, blob_z(tar + data, size, ulen));
          n++;
        } else if (*s) { drive_put(d, s, blob_static(tar + data, size)); n++; }
      }
    }
    p = data + ((size + 511) & ~511u);
  }
  return n;
}
