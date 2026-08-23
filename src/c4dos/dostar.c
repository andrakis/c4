// dostar -- unpack an archive onto the C4DOS RAM disk.
//
//   RUN dostar.c4r x archive.tar     extract every member
//   RUN dostar.c4r t archive.tar     list members, extract nothing
//
// Building anything real in-machine means getting a lot of small files
// in there first -- C4KE alone wants its source, three extensions, and
// two dozen headers -- and C4DOS has no way to receive files except by
// reading one off the disk. One archive read once is the difference
// between a build you can run and a build you can describe.
//
// The format is deliberately dull, so that a shell one-liner can WRITE
// one and strict c4 can READ one:
//
//   C4TAR1\n
//   <name> <decimal length>\n
//   <exactly that many raw bytes>
//   ... repeated ...
//
// No compression. The archive is read off a read-only disk where the
// bytes already sit; squeezing them would cost a decompressor and buy
// nothing but a smaller file on a disk that is not short of room.
//
// Dialect: strict c4 (no switch/break/continue/for, no structs, no
// local arrays, locals at top, single-word globals, literal enums,
// definition before use).

#include "c4dos_native.h"

enum { TARMAX = 8388608 };   // 8MB: the whole archive is read at once

char *g_buf;
int   g_len;

int xlen (char *s) { int n; n = 0; while (s[n]) ++n; return n; }

int slurp (char *path) {
  int fd, n, total;
  total = 0;
  // The RAM disk first, same as everything else on this system, so an
  // archive that was itself unpacked from another one still works.
  if (dos_readable()) {
    total = dos_slurp(path, g_buf, TARMAX - 1);
    if (total >= 0) { g_len = total; return 1; }
  }
  if ((fd = open(path, 0)) < 0) return 0;
  n = 1;
  while (n > 0 && total < TARMAX - 65536) {
    if ((n = read(fd, g_buf + total, 65536)) > 0) total = total + n;
  }
  close(fd);
  g_len = total;
  return 1;
}

// walk one entry: p points at a header line. Returns the next header,
// or 0 at the end. Fills g_name / g_size.
char *g_name;
int   g_size;

char *entry (char *p, char *end) {
  char *q;
  int n;
  if (p >= end) return 0;
  // name, up to the space
  q = p;
  while (q < end && *q != ' ' && *q != 10) ++q;
  if (q >= end) return 0;
  n = q - p;
  if (n > 250) return 0;
  g_name = p;
  *q = 0;                       // the archive is scratch: terminate in place
  ++q;
  // decimal length, up to the newline
  g_size = 0;
  while (q < end && *q >= '0' && *q <= '9') {
    g_size = g_size * 10 + (*q - '0');
    ++q;
  }
  while (q < end && *q != 10) ++q;
  if (q >= end) return 0;
  ++q;                          // past the newline: payload starts here
  if (q + g_size > end) return 0;
  return q;
}

int main (int argc, char **argv) {
  char *p, *end, *data, *mode;
  int n, extract, count, total;

  if (argc < 3) {
    printf("usage: dostar x|t archive.tar\n");
    printf("  x  extract every member onto the RAM disk\n");
    printf("  t  list members\n");
    return 1;
  }
  mode = argv[1];
  extract = (*mode == 'x' || *mode == 'X');
  if (extract && !dos_can_write()) {
    printf("dostar: extracting needs DEVICE=RAMDISK.SYS in CONFIG.SYS\n");
    return 1;
  }

  if (!(g_buf = malloc(TARMAX))) { printf("dostar: out of memory\n"); return 1; }
  if (!slurp(argv[2])) { printf("dostar: cannot open %s\n", argv[2]); return 1; }
  if (g_len < 8) { printf("dostar: %s is empty\n", argv[2]); return 1; }

  p = g_buf;
  end = g_buf + g_len;
  if (!(p[0] == 'C' && p[1] == '4' && p[2] == 'T' && p[3] == 'A' &&
        p[4] == 'R' && p[5] == '1')) {
    printf("dostar: %s is not a C4TAR1 archive\n", argv[2]);
    return 1;
  }
  while (p < end && *p != 10) ++p;
  if (p < end) ++p;

  count = 0;
  total = 0;
  while (p < end) {
    if (!(data = entry(p, end))) p = end;
    else {
      if (extract) {
        if (dos_put(g_name, data, g_size) < 0) {
          printf("dostar: could not write %s\n", g_name);
          return 1;
        }
      } else {
        printf("%s  %d\n", g_name, g_size);
      }
      ++count;
      total = total + g_size;
      p = data + g_size;
    }
  }
  if (extract) printf("dostar: extracted %d files, %d bytes\n", count, total);
  else printf("dostar: %d files, %d bytes\n", count, total);
  return 0;
}
