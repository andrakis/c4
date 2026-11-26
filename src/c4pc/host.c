/* host.c -- c4pc-host: the c4pc engine as a Linux program, with
 * libjs/cli.js's flags, so that one can stand in for the other in a test:
 *
 *   c4pc-host [-m MB] [-c cycles] [-s] [-d dir] [-w dir] [-i] [-hz MHz]
 *             [--fpu|--no-fpu] [--mmu|--no-mmu] [--net udp:LPORT:[HOST:]PPORT
 *             [--net-id N]] program.c4r [args...]
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
#include <pthread.h>
#include <limits.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include "snd.h"

/* ---- the parallel engine's CPUs (par.c): a thread each, and a futex ---- */
int plat_cpus(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
struct thr { int k; void (*fn)(int); };
static void *thr_main(void *a) { struct thr t = *(struct thr *)a; free(a); t.fn(t.k); return 0; }
int plat_cpu_start(int k, void (*fn)(int)) {
  struct thr *t = malloc(sizeof *t);
  t->k = k; t->fn = fn;
  pthread_t th;
  if (pthread_create(&th, 0, thr_main, t)) { free(t); return 0; }
  pthread_detach(th);
  return 1;
}
void plat_wait(int k, volatile int *kick, int seen, int ms) {
  (void)k;
  struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };
  syscall(SYS_futex, kick, FUTEX_WAIT_PRIVATE, seen, ms < 0 ? 0 : &ts, 0, 0);
}
void plat_kick(int k, volatile int *kick) {
  (void)k;
  __atomic_add_fetch(kick, 1, __ATOMIC_SEQ_CST);
  syscall(SYS_futex, kick, FUTEX_WAKE_PRIVATE, INT_MAX, 0, 0, 0);
}

i64 plat_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (i64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
double plat_now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e6 + ts.tv_nsec / 1000.0;
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
 *   keydown CODE CHAR MODS | keyup CODE CHAR MODS (a key held: Alt+Tab) |
 *   type TEXT | shot FILE.png | quit |
 *   stats: the display's counts so far, a line on stderr ("c4pc: at MS
 *   ms: screen S, N presents, N flips, N commands, N switches") |
 *   packet DX DY BUTTONS: a PS/2 packet's worth, motion and buttons at
 *   once (pc/screen.c mouse_poll), BUTTONS a mask |
 *   mouse: wait until a ring on the display has asked for the mouse's
 *   moves (the desktop up, however long the boot took), and put off what
 *   follows by as long as that wait */
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
    if (!strcmp(e->what, "mouse")) {
      if (!gui_wants_mouse(g)) { gev_at--; return 0; }
      for (int k = gev_at; k < ngev; k++) gevs[k].ms += now - e->ms;
    }
    else if (!strcmp(e->what, "move")) gui_mouse_at(g, e->a, e->b, 0);
    else if (!strcmp(e->what, "down")) { gui_mouse_at(g, e->a, e->b, 0); gui_mouse(g, 0, 0, 1 << e->c, 0); }
    else if (!strcmp(e->what, "up")) { gui_mouse_at(g, e->a, e->b, 1 << e->c); gui_mouse(g, 0, 0, 0, 0); }
    else if (!strcmp(e->what, "click")) { gui_mouse_at(g, e->a, e->b, 0); gui_mouse(g, 0, 0, 1, 0); gui_mouse(g, 0, 0, 0, 0); }
    else if (!strcmp(e->what, "packet")) gui_mouse(g, e->a * 256, e->b * 256, e->c, 0);
    else if (!strcmp(e->what, "key")) { gui_key(g, 1, e->a, e->b, e->c); gui_key(g, 0, e->a, e->b, e->c); }
    else if (!strcmp(e->what, "keydown")) gui_key(g, 1, e->a, e->b, e->c);
    else if (!strcmp(e->what, "keyup")) gui_key(g, 0, e->a, e->b, e->c);
    else if (!strcmp(e->what, "stats")) {
      u32 c, p, ev, fl; gui_stats(g, &c, &p, &ev, &fl);
      fprintf(stderr, "c4pc: at %lld ms: screen %d, %u presents, %u flips, %u commands, %u switches\n",
              (long long)now, gui_screen(g), p, fl, c, gui_switches(g));
    }
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
  fprintf(stderr, "usage: c4pc-host [-m MB] [-c cycles] [-s] [-d dir] [-w dir] [-i] [-hz MHz] [--no-fpu] [--mmu] [--cpus N [-q N] [--parallel]] [--cdrom FILE.iso] [--disk FILE.img] [--tar FILE.tar] [--no-accel] [--no-osaccel] [--wav FILE] [--gui WxH [--events FILE] [--shot FILE.png] [--texts FILE]] [--c4m] [--net udp:LPORT:[HOST:]PPORT [--net-id N]] program.c4r [args...]\n");
  exit(1);
}

/* --cdrom FILE: an ISO image as a CD drive (cdrom.c), its sectors read
 * from the file as they are wanted */
static int iso_rd(void *ctx, u32 lba, u32 n, u8 *buf) {
  FILE *f = ctx;
  if (fseeko(f, (off_t)lba * 2048, SEEK_SET)) return -1;
  size_t got = fread(buf, 2048, n, f);
  if (got < n) memset(buf + got * 2048, 0, (size_t)(n - got) * 2048);
  return 0;
}

/* --disk FILE: a disk image as a block device (dev.c BLK_*), read and
 * written in place */
static int img_rw(blkdev *b, int write, u32 lba, u32 n, u8 *buf) {
  FILE *f = b->ctx;
  if (fseeko(f, (off_t)lba * 512, SEEK_SET)) return -1;
  if (write) return fwrite(buf, 512, n, f) == n ? 0 : -1;
  size_t got = fread(buf, 512, n, f);
  if (got < n) memset(buf + got * 512, 0, (size_t)(n - got) * 512);
  return 0;
}
static int img_flush(blkdev *b) { return fflush((FILE *)b->ctx) ? -1 : 0; }

/* --net udp:LPORT:[HOST:]PPORT (docs/c4pc-network.md, N2): a network card,
 * on a switch of its own whose uplink is a UDP socket -- each frame a
 * datagram from LPORT to HOST:PPORT (HOST 127.0.0.1 if not given), and
 * what comes to LPORT a frame in. Two c4pc-hosts, or one and cli.js,
 * pointed at each other are machines on one cable. --net-id N: its id
 * there, its MAC 52:54:00:c4:00:N and address 10.4.0.N+1 (1, 10.4.0.2, if
 * not given). The machine runs on the real clock then (TIME, as pc.c's
 * paced machine), its naps waited out on the socket, so that its timers
 * and its peer's agree. */
static int net_fd = -1;
static struct sockaddr_in net_peer;
static void udp_out(const u8 *f, u32 len) {
  if (sendto(net_fd, f, len, 0, (struct sockaddr *)&net_peer, sizeof net_peer) < 0) { /* dropped, as a cable would */ }
}
static void udp_drain(void) {
  u8 b[1600];
  ssize_t n;
  while (net_fd >= 0 && (n = recv(net_fd, b, sizeof b, MSG_DONTWAIT)) > 0) net_inject(b, (u32)n);
}
static int udp_open(const char *spec) {
  char host[256] = "127.0.0.1";
  int lport, pport;
  if (strncmp(spec, "udp:", 4)) return -1;
  const char *p = spec + 4;
  lport = atoi(p);
  if (!(p = strchr(p, ':'))) return -1;
  p++;
  const char *c = strrchr(p, ':');
  if (c) {
    size_t l = (size_t)(c - p);
    if (l >= sizeof host) return -1;
    memcpy(host, p, l); host[l] = 0;
    pport = atoi(c + 1);
  } else pport = atoi(p);
  if (lport <= 0 || lport > 65535 || pport <= 0 || pport > 65535) return -1;
  struct addrinfo hints, *res;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
  if (getaddrinfo(host, 0, &hints, &res) || !res) return -1;
  memcpy(&net_peer, res->ai_addr, sizeof net_peer);
  net_peer.sin_port = htons((u16)pport);
  freeaddrinfo(res);
  struct sockaddr_in me;
  memset(&me, 0, sizeof me);
  me.sin_family = AF_INET; me.sin_port = htons((u16)lport); me.sin_addr.s_addr = htonl(INADDR_ANY);
  if ((net_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0 || bind(net_fd, (struct sockaddr *)&me, sizeof me)) return -1;
  net_uplink_fn = udp_out;
  return 0;
}

/* SIGTERM: the machine's power button, SIGTERM to the program (C4IX shuts
 * down, sched.c); a second, or a program that takes none, ends it here */
static volatile sig_atomic_t host_terms;
static void on_term(int s) { (void)s; host_terms++; }

int main(int argc, char **argv) {
  u32 mb = 32; double maxc = -1, mhz = 20; int cpus = 0, quantum = 0, parallel = 0, stats = 0, inter = 0, nofpu = 0, mmu_on = 0, noaccel = 0, noosaccel = 0, gw = 0, gh = 0, c4m_info = 0;
  const char *shot = 0, *wav = 0;
  int modes = 0;
  const char *net = 0; int net_id = 1;
  drive *drives[16]; int nd = 0;
  const char *cds[4]; int ncd = 0;
  const char *disks[8]; int ndisk = 0;
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
    else if (!strcmp(f, "--cpus") && i + 1 < argc) cpus = atoi(argv[++i]);   /* cli.js's: N processors (smp.c) */
    else if (!strcmp(f, "-q") && i + 1 < argc) quantum = atoi(argv[++i]);    /* ... each -q instructions in turn */
    else if (!strcmp(f, "--parallel")) parallel = 1;                         /* ... each on a thread (par.c) */
    else if (!strcmp(f, "--cdrom") && i + 1 < argc && ncd < 4) cds[ncd++] = argv[++i];   /* an ISO image as a CD drive, from drive 2 */
    else if (!strcmp(f, "--tar") && i + 1 < argc && nd < 16) {
      /* a ustar archive as a drive, as a PC's bootloader hands c4pc one
       * (drive.c: its .c4pcz members compressed, inflated when read) */
      FILE *tf = fopen(argv[++i], "rb");
      if (!tf) { fprintf(stderr, "c4pc-host: cannot open %s\n", argv[i]); return 1; }
      fseek(tf, 0, SEEK_END); long tl = ftell(tf); fseek(tf, 0, SEEK_SET);
      u8 *tb = malloc((size_t)tl + 1);
      if (!tb || fread(tb, 1, (size_t)tl, tf) != (size_t)tl) { fprintf(stderr, "c4pc-host: cannot read %s\n", argv[i]); return 1; }
      fclose(tf);
      drive *d = drive_new(0);
      drive_add_tar(d, tb, (u32)tl);
      drives[nd++] = d;
    }
    else if (!strcmp(f, "--disk") && i + 1 < argc && ndisk < 8) disks[ndisk++] = argv[++i];  /* a disk image as a block device */
    else if (!strcmp(f, "--gui") && i + 1 < argc) { if (sscanf(argv[++i], "%dx%d", &gw, &gh) != 2) usage(); }   /* fit a display (gui.c) */
    else if (!strcmp(f, "--modes")) modes = 1;      /* ... and give it a PC's list of modes (gui.c MODE), taken as asked */
    else if (!strcmp(f, "--events") && i + 1 < argc) load_events(argv[++i]);
    else if (!strcmp(f, "--shot") && i + 1 < argc) shot = argv[++i];    /* the display as a PNG at the end */
    else if (!strcmp(f, "--wav") && i + 1 < argc) wav = argv[++i];      /* cli.js's: fit a sound card, write what it played */
    else if (!strcmp(f, "--texts") && i + 1 < argc) {
      extern void (*gui_text_hook)(int, int, const char *, int);
      if (!(texts = fopen(argv[++i], "w"))) { fprintf(stderr, "c4pc: cannot write %s\n", argv[i]); return 1; }
      gui_text_hook = text_hook;
    }
    else if (!strcmp(f, "--no-mmu")) mmu_on = 0;
    else if (!strcmp(f, "--net") && i + 1 < argc) net = argv[++i];        /* a network card, its uplink a UDP socket */
    else if (!strcmp(f, "--net-id") && i + 1 < argc) net_id = atoi(argv[++i]);
    else if (!strcmp(f, "--c4m")) c4m_info = 1;          /* INFO as native c4m's: no devices (engine.c machine_info) */
    else if (!strcmp(f, "--fast") || !strcmp(f, "--no-accel") || !strcmp(f, "--no-osaccel")) ;
    else if (!strcmp(f, "--")) { i++; break; }
    else { fprintf(stderr, "c4pc: unknown option %s\n", f); usage(); }
  }
  if (i >= argc) usage();
  if (!nd) drives[nd++] = drive_new(0);
  /* CD drives from 2 on, as on a PC (pc.c): drive 1 is persist's, an
   * empty read-only one where -w gave none */
  for (int k = 0; k < ncd && nd < 16; k++) {
    FILE *f = fopen(cds[k], "rb");
    if (!f) { fprintf(stderr, "c4pc: cannot read %s\n", cds[k]); return 1; }
    fseeko(f, 0, SEEK_END);
    u32 blocks = (u32)(ftello(f) / 2048);
    if (nd < 2) { drive *e = drive_new(0); e->kind = "none"; drives[nd++] = e; }
    drives[nd++] = cdrom_image(iso_rd, f, blocks, cds[k]);
  }
  u32 len; u8 *img = read_file(argv[i], &len);
  if (!img) { fprintf(stderr, "c4pc: cannot read %s\n", argv[i]); return 1; }

  machine *m = machine_new(mb * 1024 * 1024, drives, nd, mhz);
  m->on_byte = on_byte;
  m->no_fpu = nofpu;
  m->c4m_info = c4m_info;
  if (noaccel) m->accel = 0;
  if (noosaccel) m->osaccel = 0;
  if (mmu_on) mmu_new(m);
  for (int k = 0; k < ndisk; k++) {
    FILE *f = fopen(disks[k], "r+b");
    int ro = 0;
    if (!f) { f = fopen(disks[k], "rb"); ro = 1; }
    if (!f) { fprintf(stderr, "c4pc: cannot open %s\n", disks[k]); return 1; }
    fseeko(f, 0, SEEK_END);
    blkdev *b = calloc(1, sizeof *b);
    b->sectors = (u32)(ftello(f) / 512); b->flags = ro; b->rw = img_rw; b->flush = img_flush; b->ctx = f;
    snprintf(b->what, sizeof b->what, "%s", disks[k]);
    m->blk[m->nblk++] = b;
  }
  if (cpus > 0) smp_new(m, cpus, quantum);
  if (net) {
    if (net_id < 1 || net_id > 253 || udp_open(net)) { fprintf(stderr, "c4pc-host: --net %s: not udp:LPORT:[HOST:]PPORT, or that port is taken\n", net); return 1; }
    char nm[16]; snprintf(nm, sizeof nm, "vm%d", net_id);
    net_attach(m, net_id, nm);
    m->dv->wall = 1; m->dv->wall_base = plat_now_ms();
  }
  if (gw > 0 && gh > 0) m->gui = gui_new(m, gw, gh);
  if (m->gui && modes) {
    static const int mw[] = { 640, 800, 1024, 1152, 1280, 1280, 1280, 1366, 1440, 1600, 1680, 1920, 1920 };
    static const int mh[] = { 480, 600, 768, 864, 720, 800, 1024, 768, 900, 1200, 1050, 1080, 1200 };
    gui_set_modes(m->gui, mw, mh, (int)(sizeof mw / sizeof mw[0]), gw, gh);
  }
  if (wav) m->snd = snd_new(m, 44100, 0);
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
  /* on threads: one for each CPU but 0, each waiting for a CPUS */
  if (cpus > 1 && parallel) par_start(m->smp);

  i64 t0 = plat_now_ms();
  i64 limit = maxc < 0 ? INT64_MAX : (i64)maxc;
  int status, terms_seen = 0;
  i64 term_at = 0;
  signal(SIGTERM, on_term);
  for (;;) {
    if (host_terms != terms_seen) {
      terms_seen = host_terms;
      /* a second a second later: the one a signal to the process group
       * also sends (timeout(1)) is the same press */
      if (!m->sigh[15] || (term_at && plat_now_ms() - term_at >= 1000)) { flush_out(); fprintf(stderr, "c4pc-host: terminated\n"); status = 143; break; }
      if (!term_at) { term_at = plat_now_ms(); m->pending_signal = 15; }
    }
    /* the machine's time: with several CPUs, the round robin's, not CPU 0's */
    i64 left = limit - machine_clock(m);
    if (left <= 0) { flush_out(); fprintf(stderr, "libjs: stopped at cycle budget (%lld cycles)\n", (long long)machine_clock(m)); status = 9; break; }
    i64 slice = m->gui || net ? 1000000 : 50000000;
    int r = machine_run(m, left < slice ? left : slice);
    flush_out();
    udp_drain();
    /* the parallel engine: CPU 0 answers the others between runs, and
     * waits for them when it is parked or halted (cli.js) */
    if (m->smp && m->smp->par && !m->halted) {
      int k = par_after(m->smp, r, &status);
      flush_out();
      if (k == 2) break;
      if (k == 1) continue;
    }
    if (m->gui) {
      gui_tick(m->gui);
      { int w, h; if (gui_mode_request(m->gui, &w, &h)) gui_mode_now(m->gui, w, h); }   /* --modes: any it lists */
      if (run_events(m)) { status = 0; break; }
      if (r == STOP_INPUT && !inter) { struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); continue; }
    }
    if (r == STOP_HALT) { status = m->status; break; }
    /* --net: a nap on the real clock, until its time, or a frame comes; none
     * with a frame already waiting for the card (hv.c's guests' rule) */
    if (net && r == STOP_SLEEP) {
      udp_drain();
      i64 w = m->dv->wall_base + m->dv->nap_until - plat_now_ms();
      if (m->pit_ms && m->pit_next - plat_now_ms() < w) w = m->pit_next - plat_now_ms();
      if (w > 100) w = 100;
      if (w > 0 && !net_waiting(m)) { struct pollfd nf = { net_fd, POLLIN, 0 }; poll(&nf, 1, (int)w); }
      udp_drain();
      continue;
    }
    if (r == STOP_INPUT) {
      if (!inter) { fprintf(stderr, "libjs: blocked on input with nothing left to read\n"); status = 8; break; }
      struct pollfd pfs[2] = { { 0, POLLIN, 0 }, { net_fd, POLLIN, 0 } };
      int wait = 100;
      if (m->pit_ms) { i64 w = m->pit_next - plat_now_ms(); wait = w < 1 ? 1 : (int)w; }
      if (poll(pfs, net_fd >= 0 ? 2 : 1, wait) > 0 && (pfs[0].revents & (POLLIN | POLLHUP))) {
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
  if (m->smp && m->smp->par) par_shutdown(m->smp);
  if (raw) tcsetattr(0, TCSANOW, &saved);
  if (m->snd) {
    u32 n, w, p; u8 *b = snd_wav(m->snd, &n);
    FILE *f = fopen(wav, "wb");
    if (!f || fwrite(b, 1, n, f) != n) fprintf(stderr, "c4pc: cannot write %s\n", wav);
    if (f) fclose(f);
    free(b);
    snd_stats(m->snd, &w, &p);
    if (stats) fprintf(stderr, "c4pc: sound: %u OPL writes, %u samples played\n", w, p);
  }
  if (m->gui && shot) { int w, h; u32 *px = gui_front(m->gui, &w, &h); write_png(shot, px, w, h); }
  if (m->gui && stats) { u32 c, p, e, fl; gui_stats(m->gui, &c, &p, &e, &fl); fprintf(stderr, "c4pc: display: %u commands, %u presents, %u flips, %u events\n", c, p, fl, e); }
  if (stats) {
    double dt = (double)(plat_now_ms() - t0) / 1000;
    i64 ran = m->cycle - m->skipped;
    fprintf(stderr, "c4pc: %lld cycles in %.2fs (%lldk inst/s), %lld missed traps, status %d\n",
            (long long)ran, dt, (long long)(dt > 0 ? ran / dt / 1000 : 0), (long long)m->missed_traps, status);
    if (m->smp && m->smp->par) { char b[512]; par_stats(m->smp, b, sizeof b); fprintf(stderr, "c4pc: %s\n", b); }
    { extern unsigned long long blk_counts[4];
      if (blk_counts[0] || blk_counts[1])
        fprintf(stderr, "c4pc: disks: %llu reads (%llu KB), %llu writes (%llu KB)\n", (unsigned long long)blk_counts[0],
                (unsigned long long)blk_counts[2] / 2, (unsigned long long)blk_counts[1], (unsigned long long)blk_counts[3] / 2); }
    fprintf(stderr, "c4pc: heap %d KB in use at exit, %d KB at most\n", heap_used(m->heap) / 1024, heap_peak(m->heap) / 1024);
  }
  return status & 0xff;
}
