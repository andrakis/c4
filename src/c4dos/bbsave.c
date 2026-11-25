// bbsave -- write the C4DOS RAM disk onto another drive.
//
//   RUN bbsave.c4r B:          every RAM-disk file onto drive 1
//   RUN bbsave.c4r B: c4ke.c4r init.c4r     just those
//
// This is the thing that makes the climb survive a power cut. C4DOS
// builds a kernel into its RAM disk and the RAM disk dies with the
// session; put it on a medium instead and you can eject the C4DOS
// floppy, boot drive 1, and be in the system you compiled yesterday.
//
// Board only: it drives c4bb's disk registers directly (include/c4bb.h),
// which is the same rung dostar and dosload sit on -- a transient, not
// something C4DOS itself has to know about. docs/c4bb-storage.md.
//
// Dialect: strict c4, like everything else that has to run here.
#include "c4bb.h"

int xlen (char *s) { int n; n = 0; while (s[n]) ++n; return n; }

int nameeq (char *a, char *b) {
  int i;
  i = 0;
  while (a[i] && a[i] == b[i]) ++i;
  return a[i] == b[i];
}

// "1:" or "B:" -> the drive number, or -1.
int drivenum (char *s) {
  int c;
  if (!s[0] || s[1] != ':' || s[2]) return 0 - 1;
  c = s[0];
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a';
  return 0 - 1;
}

int wanted (int argc, char **argv, char *name) {
  int i;
  if (argc <= 2) return 1;            // no list: everything
  i = 2;
  while (i < argc) { if (nameeq(argv[i], name)) return 1; ++i; }
  return 0;
}

int main (int argc, char **argv) {
  int drv, n, i, saved, bytes, len, keep;
  char *name, *data;

  if (argc < 2) {
    printf("usage: bbsave <drive>: [file...]\n");
    printf("  writes the RAM disk onto that drive; no files means all\n");
    return 1;
  }
  if ((drv = drivenum(argv[1])) < 0) {
    printf("bbsave: '%s' is not a drive -- try 1: or B:\n", argv[1]);
    return 1;
  }
  if (drv >= bb_drives()) {
    printf("bbsave: this machine has %d drive(s), no %d\n", bb_drives(), drv);
    return 1;
  }
  keep = bb_drive();
  bb_select(drv);
  if (bb_readonly()) {
    printf("bbsave: drive %d is read-only -- nothing to write to\n", drv);
    bb_select(keep);
    return 1;
  }

  if (!dos_can_enum()) {
    printf("bbsave: this C4DOS cannot enumerate its RAM disk (needs API v2)\n");
    bb_select(keep);
    return 1;
  }

  n = dos_count();
  i = 0;
  saved = 0;
  bytes = 0;
  while (i < n) {
    name = dos_entname(i);
    len  = dos_entsize(i);
    data = dos_entdata(i);
    if (name && len >= 0 && wanted(argc, argv, name)) {
      if (bb_put(name, data, len) < 0) {
        printf("bbsave: could not write %s\n", name);
        bb_select(keep);
        return 1;
      }
      printf("  %s  %d\n", name, len);
      ++saved;
      bytes = bytes + len;
    }
    ++i;
  }
  bb_select(keep);
  printf("bbsave: %d files, %d bytes onto drive %d\n", saved, bytes, drv);
  return 0;
}
