/* drive.c -- a drive: named files in memory (c4bb's drives, devices.js).
 * On the PC a drive comes from a ustar archive the bootloader loaded as a
 * module (make c4pc-drives); its files stay where the module is. */
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
        if (*s) { drive_put(d, s, blob_static(tar + data, size)); n++; }
      }
    }
    p = data + ((size + 511) & ~511u);
  }
  return n;
}
