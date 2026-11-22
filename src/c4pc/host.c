/* host.c -- c4pc-host: the c4pc engine as a Linux program, with
 * libjs/cli.js's flags, so that one can stand in for the other in a test:
 *
 *   c4pc-host [-m MB] [-c cycles] [-s] [-d dir] [-w dir] [-i] [-hz MHz]
 *             [--fpu|--no-fpu] [--mmu|--no-mmu] program.c4r [args...]
 *
 * Piped stdin without -i is fed to the keyboard up front with end of
 * input, as cli.js does. The PC's own front end is pc/pc.c.
 */
#define _GNU_SOURCE
#include "c4pc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <termios.h>
#include <errno.h>

i64 plat_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (i64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
void plat_fatal(const char *msg) { fflush(stdout); fprintf(stderr, "%s\n", msg); exit(70); }

static u8 outbuf[4096]; static u32 outn;
static void flush_out(void) { if (outn) { fwrite(outbuf, 1, outn, stdout); fflush(stdout); outn = 0; } }
static void on_byte(machine *m, int b) {
  (void)m;
  outbuf[outn++] = (u8)b;
  if (outn == sizeof outbuf || b == 10) flush_out();
}

static u8 *read_file(const char *path, u32 *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  u8 *b = malloc((size_t)n + 1);
  if (n && fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return 0; }
  fclose(f);
  *len = (u32)n;
  return b;
}

/* every file under dir, by its '/'-joined relative path (cli.js loadDisk) */
static void load_dir(drive *d, const char *dir, const char *rel) {
  char base[4096];
  snprintf(base, sizeof base, "%s%s%s", dir, rel[0] ? "/" : "", rel);
  DIR *dp = opendir(base);
  if (!dp) return;
  struct dirent *e;
  while ((e = readdir(dp))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char p[4096], key[4096];
    snprintf(p, sizeof p, "%s/%s", base, e->d_name);
    snprintf(key, sizeof key, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
    struct stat st;
    if (stat(p, &st)) continue;
    if (S_ISDIR(st.st_mode)) load_dir(d, dir, key);
    else {
      u32 n; u8 *b = read_file(p, &n);
      if (b) drive_put(d, key, blob_static(b, n));
    }
  }
  closedir(dp);
}

static void mkdirs(const char *path) {
  char t[4096];
  snprintf(t, sizeof t, "%s", path);
  for (char *s = t + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(t, 0777); *s = '/'; }
}
static void dir_sink(drive *d, const char *name, blob *b) {
  char p[4096];
  snprintf(p, sizeof p, "%s/%s", (char *)d->ctx, name);
  mkdirs(p);
  FILE *f = fopen(p, "wb");
  if (f) { fwrite(b->bytes, 1, b->len, f); fclose(f); }
}
static void dir_remove(drive *d, const char *name) {
  char p[4096];
  snprintf(p, sizeof p, "%s/%s", (char *)d->ctx, name);
  unlink(p);
}

static void usage(void) {
  fprintf(stderr, "usage: c4pc-host [-m MB] [-c cycles] [-s] [-d dir] [-w dir] [-i] [-hz MHz] [--no-fpu] [--mmu] program.c4r [args...]\n");
  exit(1);
}

int main(int argc, char **argv) {
  u32 mb = 32; double maxc = -1, mhz = 20; int stats = 0, inter = 0, nofpu = 0, mmu_on = 0;
  drive *drives[16]; int nd = 0;
  int i = 1;
  for (; i < argc && argv[i][0] == '-'; i++) {
    const char *f = argv[i];
    if (!strcmp(f, "-m") && i + 1 < argc) mb = (u32)atoi(argv[++i]);
    else if (!strcmp(f, "-c") && i + 1 < argc) maxc = atof(argv[++i]);
    else if (!strcmp(f, "-s")) stats = 1;
    else if ((!strcmp(f, "-d") || !strcmp(f, "-w")) && i + 1 < argc && nd < 16) {
      int w = f[1] == 'w';
      const char *dir = argv[++i];
      drive *d = drive_new(w);
      if (w) { mkdir(dir, 0777); d->sink = dir_sink; d->remove = dir_remove; d->ctx = (void *)dir; }
      load_dir(d, dir, "");
      drives[nd++] = d;
    }
    else if (!strcmp(f, "-i")) inter = 1;
    else if (!strcmp(f, "-hz") && i + 1 < argc) mhz = atof(argv[++i]);
    else if (!strcmp(f, "--no-fpu")) nofpu = 1;
    else if (!strcmp(f, "--mmu")) mmu_on = 1;            /* cli.js's: fit an MMU (mmu.c) */
    else if (!strcmp(f, "--no-mmu")) mmu_on = 0;
    else if (!strcmp(f, "--fast") || !strcmp(f, "--no-accel") || !strcmp(f, "--no-osaccel")) ;
    else if (!strcmp(f, "--")) { i++; break; }
    else { fprintf(stderr, "c4pc: unknown option %s\n", f); usage(); }
  }
  if (i >= argc) usage();
  if (!nd) drives[nd++] = drive_new(0);
  u32 len; u8 *img = read_file(argv[i], &len);
  if (!img) { fprintf(stderr, "c4pc: cannot read %s\n", argv[i]); return 1; }

  machine *m = machine_new(mb * 1024 * 1024, drives, nd, mhz);
  m->on_byte = on_byte;
  m->no_fpu = nofpu;
  if (mmu_on) mmu_new(m);
  if (!inter && !isatty(0)) {
    int c;
    while ((c = getchar()) != EOF) kbd_push(m, c);
    m->rx_eof = 1;
  } else if (!inter) m->rx_eof = 1;
  struct termios saved;
  int raw = inter && isatty(0);
  if (raw) {
    struct termios t; tcgetattr(0, &saved); t = saved;
    t.c_lflag &= ~(ICANON | ECHO); tcsetattr(0, TCSANOW, &t);
  }
  if (machine_boot(m, img, len, argc - i, argv + i)) { flush_out(); return 1; }

  i64 t0 = plat_now_ms();
  i64 limit = maxc < 0 ? INT64_MAX : (i64)maxc;
  int status;
  for (;;) {
    i64 left = limit - m->cycle;
    if (left <= 0) { flush_out(); fprintf(stderr, "libjs: stopped at cycle budget (%lld cycles)\n", (long long)m->cycle); status = 9; break; }
    i64 slice = 50000000;
    int r = machine_run(m, left < slice ? left : slice);
    flush_out();
    if (r == STOP_HALT) { status = m->status; break; }
    if (r == STOP_INPUT) {
      if (!inter) { fprintf(stderr, "libjs: blocked on input with nothing left to read\n"); status = 8; break; }
      struct pollfd pf = { 0, POLLIN, 0 };
      int wait = 100;
      if (m->pit_ms) { i64 w = m->pit_next - plat_now_ms(); wait = w < 1 ? 1 : (int)w; }
      if (poll(&pf, 1, wait) > 0) {
        u8 b[256]; ssize_t n = read(0, b, sizeof b);
        if (n <= 0) m->rx_eof = 1;
        for (ssize_t k = 0; k < n; k++) {
          int c = b[k];
          if (c == 4) { m->rx_eof = 1; continue; }
          if (m->raw_kbd > 0) { kbd_push(m, c); continue; }
          if (c == 127 || c == 8) {
            if (m->rx_tail > m->rx_head && m->rx[m->rx_tail - 1] != 10) { m->rx_tail--; fputs("\b \b", stdout); fflush(stdout); }
            continue;
          }
          if (c == 13) c = 10;
          kbd_push(m, c);
          putchar(c); fflush(stdout);
        }
      }
    }
  }
  flush_out();
  if (raw) tcsetattr(0, TCSANOW, &saved);
  if (stats) {
    double dt = (double)(plat_now_ms() - t0) / 1000;
    i64 ran = m->cycle - m->skipped;
    fprintf(stderr, "c4pc: %lld cycles in %.2fs (%lldk inst/s), %lld missed traps, status %d\n",
            (long long)ran, dt, (long long)(dt > 0 ? ran / dt / 1000 : 0), (long long)m->missed_traps, status);
    fprintf(stderr, "c4pc: heap %d KB in use at exit, %d KB at most\n", heap_used(m->heap) / 1024, heap_peak(m->heap) / 1024);
  }
  return status & 0xff;
}
