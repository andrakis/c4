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


i32 plat_clock_secs(void) {
  time_t t = time(0);
  struct tm tm;
  localtime_r(&t, &tm);
  return tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
}

/* ---- the display, for tests: a PNG of what it shows, and events on cue ---- */
static u32 crc_tab[256];
static u32 png_crc(u32 c, const u8 *b, size_t n) {
  if (!crc_tab[1]) for (u32 i = 0; i < 256; i++) { u32 v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1; crc_tab[i] = v; }
  c = ~c;
  for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ b[i]) & 255] ^ (c >> 8);
  return ~c;
}
static void be32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *t, const u8 *d, u32 n) {
  u8 h[8]; be32(h, n); memcpy(h + 4, t, 4);
  fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
  u32 c = png_crc(0, (const u8 *)t, 4); c = png_crc(c, d, n);
  u8 cb[4]; be32(cb, c); fwrite(cb, 1, 4, f);
}
/* stored (uncompressed) deflate: a PNG anything reads, made with no zlib */
static void write_png(const char *path, const u32 *px, int w, int h) {
  FILE *f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "c4pc: cannot write %s\n", path); return; }
  fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
  u8 ih[13]; be32(ih, (u32)w); be32(ih + 4, (u32)h); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
  chunk(f, "IHDR", ih, 13);
  size_t raw_n = (size_t)h * (1 + (size_t)w * 3);
  u8 *raw = malloc(raw_n);
  for (int y = 0; y < h; y++) {
    u8 *r = raw + (size_t)y * (1 + (size_t)w * 3); *r++ = 0;
    for (int x = 0; x < w; x++) { u32 v = px[(size_t)y * w + x]; *r++ = v >> 16; *r++ = v >> 8; *r++ = v; }
  }
  size_t nblk = (raw_n + 65534) / 65535, zn = 2 + raw_n + nblk * 5 + 4;
  u8 *z = malloc(zn), *q = z;
  *q++ = 0x78; *q++ = 0x01;
  u32 a = 1, b = 0;
  for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
  for (size_t off = 0; off < raw_n; off += 65535) {
    size_t n = raw_n - off < 65535 ? raw_n - off : 65535;
    *q++ = off + n >= raw_n; *q++ = n & 255; *q++ = n >> 8; *q++ = ~n & 255; *q++ = (~n >> 8) & 255;
    memcpy(q, raw + off, n); q += n;
  }
  be32(q, b << 16 | a); q += 4;
  chunk(f, "IDAT", z, (u32)(q - z));
  chunk(f, "IEND", 0, 0);
  fclose(f);
  free(raw); free(z);
}

/* --events FILE: a line each, "MS what ...", MS the machine's own
 * milliseconds (machine_sim_ms), so a run is the same every time:
 *   move X Y | down X Y B | up X Y B | click X Y | key CODE CHAR MODS |
 *   type TEXT | shot FILE.png | quit */
typedef struct { i64 ms; char what[16]; int a, b, c; char s[256]; } gev;
static gev *gevs; static int ngev, gev_at;
static void load_events(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) { fprintf(stderr, "c4pc: cannot read %s\n", path); exit(1); }
  char line[512];
  while (fgets(line, sizeof line, f)) {
    gev e; memset(&e, 0, sizeof e);
    long long ms; int off = 0;
    if (line[0] == '#' || sscanf(line, "%lld %15s %n", &ms, e.what, &off) < 2) continue;
    e.ms = ms;
    char *rest = line + off; rest[strcspn(rest, "\n")] = 0;
    sscanf(rest, "%d %d %d", &e.a, &e.b, &e.c);
    snprintf(e.s, sizeof e.s, "%s", rest);
    gevs = realloc(gevs, sizeof *gevs * (ngev + 1)); gevs[ngev++] = e;
  }
  fclose(f);
}
/* a character as the page's keydown gives it: the DOM's keyCode, the character */
static void type_char(gui *g, int c) {
  int code = c >= 'a' && c <= 'z' ? c - 32 : c == '\n' ? 13 : c == '\b' ? 8 : c;
  if (c >= 'A' && c <= 'Z') { gui_key(g, 1, 16, 0, 1); gui_key(g, 1, c, c, 1); gui_key(g, 0, c, c, 1); gui_key(g, 0, 16, 0, 0); return; }
  gui_key(g, 1, code, c == '\n' ? 10 : c, 0); gui_key(g, 0, code, c == '\n' ? 10 : c, 0);
}
/* the events due by now; 1 at quit */
static int run_events(machine *m) {
  i64 now = machine_sim_ms(m);
  while (gev_at < ngev && gevs[gev_at].ms <= now) {
    gev *e = &gevs[gev_at++];
    gui *g = m->gui;
    if (!strcmp(e->what, "move")) gui_mouse_at(g, e->a, e->b, 0);
    else if (!strcmp(e->what, "down")) { gui_mouse_at(g, e->a, e->b, 0); gui_mouse(g, 0, 0, 1 << e->c, 0); }
    else if (!strcmp(e->what, "up")) { gui_mouse_at(g, e->a, e->b, 1 << e->c); gui_mouse(g, 0, 0, 0, 0); }
    else if (!strcmp(e->what, "click")) { gui_mouse_at(g, e->a, e->b, 0); gui_mouse(g, 0, 0, 1, 0); gui_mouse(g, 0, 0, 0, 0); }
    else if (!strcmp(e->what, "key")) { gui_key(g, 1, e->a, e->b, e->c); gui_key(g, 0, e->a, e->b, e->c); }
    else if (!strcmp(e->what, "type")) { for (char *p = e->s; *p; p++) type_char(g, *p == '|' ? '\n' : *p); }
    else if (!strcmp(e->what, "shot")) { int w, h; u32 *px = gui_front(g, &w, &h); write_png(e->s, px, w, h); fprintf(stderr, "c4pc: %s at %lld ms\n", e->s, (long long)now); }
    else if (!strcmp(e->what, "quit")) return 1;
  }
  return 0;
}

/* --texts FILE: the strings the display draws, a line each: x y string */
static FILE *texts;
static void text_hook(int x, int y, const char *s, int n) { fprintf(texts, "%d %d %.*s\n", x, y, n, s); }

static void usage(void) {
  fprintf(stderr, "usage: c4pc-host [-m MB] [-c cycles] [-s] [-d dir] [-w dir] [-i] [-hz MHz] [--no-fpu] [--mmu] [--no-accel] [--no-osaccel] [--gui WxH [--events FILE] [--shot FILE.png] [--texts FILE]] program.c4r [args...]\n");
  exit(1);
}

int main(int argc, char **argv) {
  u32 mb = 32; double maxc = -1, mhz = 20; int stats = 0, inter = 0, nofpu = 0, mmu_on = 0, noaccel = 0, noosaccel = 0, gw = 0, gh = 0;
  const char *shot = 0;
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
    else if (!strcmp(f, "--no-accel")) noaccel = 1;      /* cli.js's: no accelerators at all */
    else if (!strcmp(f, "--no-osaccel")) noosaccel = 1;  /* ... nor C4IX's own (accel.c) */
    else if (!strcmp(f, "--mmu")) mmu_on = 1;            /* cli.js's: fit an MMU (mmu.c) */
    else if (!strcmp(f, "--gui") && i + 1 < argc) { if (sscanf(argv[++i], "%dx%d", &gw, &gh) != 2) usage(); }   /* fit a display (gui.c) */
    else if (!strcmp(f, "--events") && i + 1 < argc) load_events(argv[++i]);
    else if (!strcmp(f, "--shot") && i + 1 < argc) shot = argv[++i];    /* the display as a PNG at the end */
    else if (!strcmp(f, "--texts") && i + 1 < argc) {
      extern void (*gui_text_hook)(int, int, const char *, int);
      if (!(texts = fopen(argv[++i], "w"))) { fprintf(stderr, "c4pc: cannot write %s\n", argv[i]); return 1; }
      gui_text_hook = text_hook;
    }
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
  if (noaccel) m->accel = 0;
  if (noosaccel) m->osaccel = 0;
  if (mmu_on) mmu_new(m);
  if (gw > 0 && gh > 0) m->gui = gui_new(m, gw, gh);
  if (!inter && !isatty(0)) {
    int c;
    while ((c = getchar()) != EOF) kbd_push(m, c);
    m->rx_eof = 1;
  } else if (!inter && !m->gui) m->rx_eof = 1;
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
    i64 slice = m->gui ? 1000000 : 50000000;
    int r = machine_run(m, left < slice ? left : slice);
    flush_out();
    if (m->gui) {
      gui_tick(m->gui);
      if (run_events(m)) { status = 0; break; }
      if (r == STOP_INPUT && !inter) { struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); continue; }
    }
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
  if (m->gui && shot) { int w, h; u32 *px = gui_front(m->gui, &w, &h); write_png(shot, px, w, h); }
  if (m->gui && stats) { u32 c, p, e, fl; gui_stats(m->gui, &c, &p, &e, &fl); fprintf(stderr, "c4pc: display: %u commands, %u presents, %u flips, %u events\n", c, p, fl, e); }
  if (stats) {
    double dt = (double)(plat_now_ms() - t0) / 1000;
    i64 ran = m->cycle - m->skipped;
    fprintf(stderr, "c4pc: %lld cycles in %.2fs (%lldk inst/s), %lld missed traps, status %d\n",
            (long long)ran, dt, (long long)(dt > 0 ? ran / dt / 1000 : 0), (long long)m->missed_traps, status);
    fprintf(stderr, "c4pc: heap %d KB in use at exit, %d KB at most\n", heap_used(m->heap) / 1024, heap_peak(m->heap) / 1024);
  }
  return status & 0xff;
}
