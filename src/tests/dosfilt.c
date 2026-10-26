// dosfilt.c -- a filter for C4DOS's redirection (DEVICE=REDIR.SYS): lines
// in, the same lines upper-cased out, and a count at the end.
//
//   dosfilt       reads and writes through DOS's console slots (17 and
//                 16), as a bare.c program does, so redirection reaches
//                 it on every machine C4DOS runs on
//   dosfilt -r    reads fd 0 with READ and prints with printf, as a plain
//                 c4cc program does: redirected only through the
//                 machine's console tap (REDIR.SYS TAP)
//
// Strict c4 and include/c4dos.h, like dosexec.c:
//   ./c4cc -o dosfilt.c4r include/c4dos.h src/tests/dosfilt.c

int up (int c) { if (c >= 'a' && c <= 'z') return c - 32; return c; }

// "dosfilt: N lines\n" into d; its length
int count_line (char *d, int n) {
  char *s;
  int k, m, p;
  s = "dosfilt: ";
  k = 0;
  while (*s) { d[k] = *s; ++k; ++s; }
  p = 1;
  m = n;
  while (m >= 10) { p = p * 10; m = m / 10; }
  while (p > 0) { d[k] = '0' + (n / p) % 10; ++k; p = p / 10; }
  s = n == 1 ? " line\n" : " lines\n";
  while (*s) { d[k] = *s; ++k; ++s; }
  return k;
}

int main (int argc, char **argv) {
  char *buf, *out;
  int n, i, lines, raw;
  raw = argc > 1 && argv[1][0] == '-' && argv[1][1] == 'r';
  buf = malloc(520);
  out = malloc(520);
  lines = 0;
  if (raw) {
    while ((n = read(0, buf, 511)) > 0) {
      i = 0;
      while (i < n) { if (buf[i] == 10) ++lines; buf[i] = up(buf[i]); ++i; }
      buf[n] = 0;
      printf("%s", buf);
    }
    printf("dosfilt: %d lines (read)\n", lines);
    return 0;
  }
  while ((n = dos_conread(buf, 511)) > 0) {
    out[0] = '>'; out[1] = ' ';
    i = 0;
    while (i < n) { out[i + 2] = up(buf[i]); ++i; }
    if (buf[n - 1] != 10) { out[n + 2] = 10; ++n; }
    dos_conwrite(out, n + 2);
    ++lines;
  }
  dos_conwrite(out, count_line(out, lines));
  return 0;
}
