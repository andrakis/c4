/* dev.c -- the device registers at 0x100-0x1FF: src/c4bb/sim/devices.js
 * in C, register for register (the comments there say why each is what
 * it is). Not here yet: the mailbox's host side (MBOX_* read 0), the
 * provenance port, downloads (download:NAME is refused). */
#include "c4pc.h"
#include "clib.h"

enum {
  UART_TX = 0x100, UART_RX = 0x104, UART_RXAVL = 0x108, TIME_MS = 0x10c, CYCLE_LO = 0x110, CYCLE_HI = 0x114,
  USLP_US = 0x118, SWAP_ADDR = 0x11c, DISK_NAME = 0x120, DISK_FD = 0x124, DISK_ADDR = 0x128, DISK_LEN = 0x12c,
  DISK_CLOSE = 0x130, OPNAME = 0x134, DISK_FLAGS = 0x138, DISK_DRIVE = 0x13c, POWER = 0x140,
  HEAP_BASE = 0x144, HEAP_END = 0x148, INFO_REG = 0x14c, TRAPH_REG = 0x150, INTERVAL_REG = 0x154,
  SWAP_CMD = 0x158, TRESTORE_REG = 0x15c, MODE_REG = 0x160, TT_REG = 0x164, TP_REG = 0x168, HND_REG = 0x16c,
  JMODE_REG = 0x170, JINTERVAL_REG = 0x174, DISK_WNAME = 0x178, DISK_WADDR = 0x17c, DISK_WLEN = 0x180,
  DISK_WCLOSE = 0x184, DISK_COUNT = 0x188, DISK_RO = 0x18c, DISK_EJECT = 0x190, DISK_RESCAN = 0x194,
  RESET = 0x198, RTC_MS = 0x19c, PIT_MS = 0x1a0, UART_TXADDR = 0x1ac, UART_TXLEN = 0x1b0,
  MBOX_BASE = 0x1b4, MBOX_LEN = 0x1b8, MBOX_BELL = 0x1bc, HEAP_USED = 0x1c0, HEAP_GEN = 0x1c4,
  HEAP_RELEASE = 0x1c8, DISK_UNLINK = 0x1cc, CON_TAP = 0x1d0, CON_ADDR = 0x1d4, CON_LEN = 0x1d8,
  CON_IN = 0x1dc, DISK_DIRS = 0x1e0, STACK_BASE = 0x1e4, STACK_TOP = 0x1e8, STACK_GUARD = 0x1ec,
  IMG_CODE = 0x1f0, IMG_DATA = 0x1f4, IMG_BSS = 0x1f8, IMG_END = 0x1fc,
};
#define O_NONBLOCK 0x800
#define FD_KEYBOARD 0
#define TAP_MAX (16u << 20)
#define SWAP_PAGE 4096

enum { FD_FILE = 1, FD_KBD, FD_WRITE };
struct fdent {
  int kind;
  blob *b; u32 off, len, pos;          /* a file: len bytes of b from off */
  int raw, nonblock;                   /* a keyboard */
  drive *wd; char *wname; u8 *wbuf; u32 wlen, wcap;   /* a file being written */
};

/* ---- memory as a device reaches it (src/c4bb/sim/arena.js dma ...) ----
 * Every device that reads or writes guest memory at an address it was
 * handed comes through these, so that a user task under the MMU can only
 * hand it its own memory: the range is checked against the task's space
 * (dma_ctx, mmu_dma_check), and a refusal is the task's page fault. In
 * kernel mode a user address is reached through the window (uwin, udir).
 * dma: where [addr, addr + len) is, one run of memory, or -1. PIN: the
 * device may keep the address, so C4IX must not move the pages. */
static i64 dma(machine *m, u32 addr, i32 len, int write, int pin) {
  if (m->dma_ctx) return mmu_dma_check(m->dma_ctx, addr, len, write, pin);
  if (m->uwin && addr >= m->uwin) {
    if (m->udir) return kernel_phys(m, m->udir, addr, pin);
    u32 off = addr - m->uwin;
    return off < m->size ? (i64)off : -1;
  }
  return addr;
}
/* a page the kernel's map has put out to swap, where kernel mode hands a
 * device a user address: 1 and the host's copy of it (0: a slot never
 * written, zeros, unless WRITE, which makes it); 0 if it is not out */
static int dma_swapped(machine *m, u32 va, int write, u8 **page) {
  if (m->dma_ctx || !m->udir) return 0;
  i32 pte = kernel_pte(m, m->udir, va);
  if ((pte & 1) || !(pte & PTE_SWAP)) return 0;
  u32 slot = (u32)pte >> 12;
  u8 *p = imap_get(m->dv->swap_pages, slot);
  if (!p && write) {
    if (slot >= m->dv->swap_slots) return 0;
    p = xmalloc(SWAP_PAGE);
    imap_set(m->dv->swap_pages, slot, p);
  }
  *page = p;
  return 1;
}
static int dma_plain(machine *m, u32 addr) { return !m->dma_ctx && !(m->uwin && addr >= m->uwin); }
/* a copy of LEN bytes at addr, a page at a time: how many (short past the end) */
static u32 dma_read(machine *m, u32 addr, u32 len, u8 *out) {
  if (dma_plain(m, addr)) {
    if (addr >= m->size) return 0;
    if (len > m->size - addr) len = m->size - addr;
    memcpy(out, m->u8 + addr, len);
    return len;
  }
  for (u32 k = 0; k < len;) {
    u32 va = addr + k, r = len - k < 4096 - (va & 4095) ? len - k : 4096 - (va & 4095);
    i64 p = dma(m, va, (i32)r, 0, 0);
    if (p >= 0 && p + r <= m->size) memcpy(out + k, m->u8 + p, r);
    else {
      u8 *pg;
      if (!dma_swapped(m, va, 0, &pg)) return k;
      if (pg) memcpy(out + k, pg + (va & 4095), r); else memset(out + k, 0, r);
    }
    k += r;
  }
  return len;
}
/* ... and LEN bytes written there: how many were (short where memory ends) */
static u32 dma_write(machine *m, u32 addr, const u8 *b, u32 len) {
  if (dma_plain(m, addr)) {
    if (addr >= m->size) return 0;
    if (len > m->size - addr) len = m->size - addr;
    memcpy(m->u8 + addr, b, len);
    return len;
  }
  for (u32 k = 0; k < len;) {
    u32 va = addr + k, r = len - k < 4096 - (va & 4095) ? len - k : 4096 - (va & 4095);
    i64 p = dma(m, va, (i32)r, 1, 0);
    if (p >= 0 && p + r <= m->size) memcpy(m->u8 + p, b + k, r);
    else {
      u8 *pg;
      if (!dma_swapped(m, va, 1, &pg)) return k;
      memcpy(pg + (va & 4095), b + k, r);
    }
    k += r;
  }
  return len;
}
u32 dma_cstring(machine *m, u32 addr, char *out, u32 max) {
  u32 n = 0;
  i64 p = -1;
  for (u32 k = 0; k < max; k++) {
    u32 va = addr + k;
    if (p < 0 || (va & 4095) == 0) { p = dma(m, va, 1, 0, 1); if (p < 0 || p >= m->size) break; }
    else p++;
    u8 b = m->u8[p];
    if (!b) break;
    out[n++] = (char)b;
  }
  out[n] = 0;
  return n;
}
/* arena.js write8: a byte into the device window is a register write */
static void arena_write8(machine *m, u32 a, int v) {
  if (a >= DEV_BASE && a < DEV_END) { dev_write32(m, a & ~3u, v & 0xff); return; }
  if (a < m->size) m->u8[a] = (u8)v;
}

/* ---- the console ---- */
static void tap_push(machine *m, int b) {
  if (m->dv->tap_len == m->dv->tap_cap) {
    if (m->dv->tap_len >= TAP_MAX) { m->dv->tap_dropped++; return; }
    m->dv->tap_cap = m->dv->tap_cap ? m->dv->tap_cap * 2 : 4096;
    if (m->dv->tap_cap > TAP_MAX) m->dv->tap_cap = TAP_MAX;
    m->dv->tap_buf = xrealloc(m->dv->tap_buf, m->dv->tap_cap);
  }
  m->dv->tap_buf[m->dv->tap_len++] = (u8)b;
}
static void con_byte(machine *m, int b) {
  if (m->dv->tap_on) tap_push(m, b);
  else if (m->dv->on_byte) m->dv->on_byte(m, b);
}
void dev_con_bytes(machine *m, const u8 *b, u32 n) {
  if (m->dv->tap_on) { for (u32 i = 0; i < n; i++) tap_push(m, b[i]); return; }
  if (m->dv->on_byte) for (u32 i = 0; i < n; i++) m->dv->on_byte(m, b[i]);
}
static i32 tap_drain(machine *m, i32 n) {
  if (n < 0) n = 0;
  if ((u32)n > m->dv->tap_len) n = (i32)m->dv->tap_len;
  u32 a = (u32)m->dv->tap_addr;
  int mine = !m->uwin || a < m->uwin;
  if (a < 0x1000 || (mine && a >= m->size)) return 0;
  if (mine && (u32)n > m->size - a) n = (i32)(m->size - a);
  dma_write(m, a, m->dv->tap_buf, (u32)n);
  memmove(m->dv->tap_buf, m->dv->tap_buf + n, m->dv->tap_len - (u32)n);
  m->dv->tap_len -= (u32)n;
  return n;
}
static i32 tap_read(machine *m, u32 addr, i32 len) {
  i32 left = (i32)(m->dv->tap_in_len - m->dv->tap_in_pos);
  i32 n = len < left ? len : left;
  if (n < 0) n = 0;
  dma_write(m, addr, m->dv->tap_in + m->dv->tap_in_pos, (u32)n);
  m->dv->tap_in_pos += (u32)n;
  return n;
}

/* ---- the keyboard's queue ---- */
void kbd_push(machine *m, int b) {
  if (m->dv->rx_tail == m->dv->rx_cap) {
    if (m->dv->rx_head) { memmove(m->dv->rx, m->dv->rx + m->dv->rx_head, m->dv->rx_tail - m->dv->rx_head); m->dv->rx_tail -= m->dv->rx_head; m->dv->rx_head = 0; }
    else { m->dv->rx_cap = m->dv->rx_cap ? m->dv->rx_cap * 2 : 4096; m->dv->rx = xrealloc(m->dv->rx, m->dv->rx_cap); }
  }
  m->dv->rx[m->dv->rx_tail++] = (u8)b;
}
int kbd_pending(machine *m) { return (int)(m->dv->rx_tail - m->dv->rx_head); }
static int kbd_shift(machine *m) { return m->dv->rx[m->dv->rx_head++]; }

static i32 kbd_read(machine *m, u32 addr, i32 len, int nonblock, int raw) {
  if (m->dv->rx_head == m->dv->rx_tail) { m->dv->rx_starved = 1; return m->dv->rx_eof ? 0 : (nonblock ? -1 : -2); }
  if (!raw) {
    int nl = 0;
    for (u32 i = m->dv->rx_head; i < m->dv->rx_tail; i++) if (m->dv->rx[i] == 10) { nl = 1; break; }
    if (!nl && !m->dv->rx_eof) return nonblock ? -1 : -2;
  }
  i32 n = 0;
  while (n < len && m->dv->rx_head < m->dv->rx_tail) {
    i64 at = dma(m, addr + (u32)n, 1, 1, 1);   /* before the byte leaves the queue */
    if (at < 0) break;
    int b = kbd_shift(m);
    arena_write8(m, (u32)at, b);
    n++;
    if (!raw && b == 10) break;
  }
  return n;
}

/* ---- swap ---- */
static i32 swap_op(machine *m, i32 v) {
  int op = v & 15; u32 slot = (u32)v >> 4, a = m->dv->swap_addr;
  if (op == 4) { imap_clear(m->dv->swap_pages, xfree); return (i32)m->dv->swap_slots; }
  if (op == 5) { heap_set_high(m->heap, slot & 1); return 0; }
  if (op == 6) return (i32)heap_largest(m->heap);
  if (slot >= m->dv->swap_slots) return -1;
  if (op == 3) { xfree(imap_del(m->dv->swap_pages, slot)); return 0; }
  if (a < 0x1000 || (i64)a + SWAP_PAGE > m->size) return -1;
  if (op == 1) {
    u8 *p = imap_get(m->dv->swap_pages, slot);
    if (!p) { p = xmalloc(SWAP_PAGE); imap_set(m->dv->swap_pages, slot, p); }
    memcpy(p, m->u8 + a, SWAP_PAGE);
    return 0;
  }
  if (op == 2) {
    u8 *p = imap_del(m->dv->swap_pages, slot);
    if (!p) { memset(m->u8 + a, 0, SWAP_PAGE); return 0; }
    memcpy(m->u8 + a, p, SWAP_PAGE);
    xfree(p);
    return 0;
  }
  return -1;
}

/* ---- the drives ---- */
static drive *resolve_drive(machine *m, const char *name, const char **rest) {
  int n = -1;
  if (name[0] && name[1] == ':') {
    int d = (u8)name[0];
    if (d >= '0' && d <= '9') n = d - '0';
    else if (d >= 'A' && d <= 'Z') n = d - 'A';
    else if (d >= 'a' && d <= 'z') n = d - 'a';
  }
  if (n >= 0) { *rest = name + 2; return n < m->dv->ndrives ? m->dv->drives[n] : 0; }
  *rest = name;
  return m->dv->drive < m->dv->ndrives ? m->dv->drives[m->dv->drive] : 0;
}

static blob *disk_find(machine *m, const char *name) {
  const char *rest;
  drive *d = resolve_drive(m, name, &rest);
  if (!d) return 0;
  const char *clean = (rest[0] == '.' && rest[1] == '/') ? rest + 2 : rest;
  blob *b = 0;
  static char path[2048];
  if (clean[0] == '/') {
    const char *s = clean; while (*s == '/') s++;
    b = smap_get(d->files, s);
    if (!b) b = smap_get(d->files, clean);
  } else {
    for (int i = 0; i < d->ndirs && !b; i++) {
      if (d->dirs[i][0]) {
        size_t dl = strlen(d->dirs[i]), cl = strlen(clean);
        if (dl + cl + 2 > sizeof path) continue;
        memcpy(path, d->dirs[i], dl); path[dl] = '/'; memcpy(path + dl + 1, clean, cl + 1);
        b = smap_get(d->files, path);
      } else b = smap_get(d->files, clean);
    }
    if (!b) b = smap_get(d->files, clean);
    if (!b) b = smap_get(d->files, rest);
  }
  if (!b && strchr(rest, '/') && !(rest != name && rest[0] == '/')) b = smap_get(d->files, strrchr(rest, '/') + 1);
  return b;
}

static fdent *fd_new(machine *m, int kind, i32 *fd) {
  fdent *f = xmalloc(sizeof *f);
  f->kind = kind;
  *fd = m->dv->next_fd++;
  imap_set(m->dv->fds, (u32)*fd, f);
  return f;
}
static void fd_free(fdent *f) {
  if (f->b) blob_unref(f->b);
  if (f->wname) xfree(f->wname);
  if (f->wbuf) xfree(f->wbuf);
  xfree(f);
}

static void proc_uptime(i64 ms, char *s) {
  /* "S.CC T.CC\n": seconds, then tenths of seconds as Linux's idle field */
  char *p = s;
  i64 a[2] = { ms / 1000, ms / 100 }, c[2] = { ms % 1000 / 10, ms % 100 };
  for (int k = 0; k < 2; k++) {
    char t[24]; int n = 0; i64 v = a[k];
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    *p++ = '.'; *p++ = (char)('0' + c[k] / 10); *p++ = (char)('0' + c[k] % 10);
    *p++ = k ? '\n' : ' ';
  }
  *p = 0;
}

/* "NAME@OFF+LEN": the three parts, or 0 */
static int span_of(const char *name, char *base, u32 *off, u32 *len) {
  size_t n = strlen(name);
  size_t i = n;
  while (i > 0 && name[i - 1] >= '0' && name[i - 1] <= '9') i--;
  if (i == n || i == 0 || name[i - 1] != '+') return 0;
  size_t plus = i - 1, j = plus;
  while (j > 0 && name[j - 1] >= '0' && name[j - 1] <= '9') j--;
  if (j == plus || j == 0 || name[j - 1] != '@') return 0;
  size_t at = j - 1;
  memcpy(base, name, at); base[at] = 0;
  double o = 0, l = 0;
  for (size_t k = j; k < plus; k++) o = o * 10 + (name[k] - '0');
  for (size_t k = plus + 1; k < n; k++) l = l * 10 + (name[k] - '0');
  *off = o > 4e9 ? 0xffffffffu : (u32)o;
  *len = l > 4e9 ? 0xffffffffu : (u32)l;
  return 1;
}

i32 dev_disk_open(machine *m, u32 name_addr) {
  static char name[257], base[257];
  dma_cstring(m, name_addr, name, 256);
  i32 fd;
  if (!strcmp(name, "/dev/stdin") || !strcmp(name, "/dev/tty")) {
    fdent *f = fd_new(m, FD_KBD, &fd);
    f->raw = !strcmp(name, "/dev/tty");
    f->nonblock = !!(m->dv->disk_flags & O_NONBLOCK);
    if (f->raw) m->dv->raw_kbd++;
    return fd;
  }
  if (!strcmp(name, "/proc/uptime")) {
    char s[64];
    i64 ms = machine_sim_ms(m);
    proc_uptime(ms < 0 ? 0 : ms, s);
    u32 n = (u32)strlen(s);
    blob *b = blob_new(n);
    memcpy(b->bytes, s, n);
    fdent *f = fd_new(m, FD_FILE, &fd);
    f->b = b; f->off = 0; f->len = n;
    return fd;
  }
  blob *b = disk_find(m, name);
  u32 off = 0, len = b ? b->len : 0;
  u32 so, sl;
  if (!b && span_of(name, base, &so, &sl) && (b = disk_find(m, base))) {
    off = so < b->len ? so : b->len;
    u32 end = (i64)off + sl < b->len ? off + sl : b->len;
    len = end - off;
  }
  if (!b) return -1;
  blob_ref(b);
  fdent *f = fd_new(m, FD_FILE, &fd);
  f->b = b; f->off = off; f->len = len;
  return fd;
}

i32 dev_disk_read(machine *m, i32 len) {
  m->dv->disk_activity++;
  if (m->dv->disk_fd == FD_KEYBOARD)
    return m->dv->tap_in_on ? tap_read(m, (u32)m->dv->disk_addr, len) : kbd_read(m, (u32)m->dv->disk_addr, len, 0, 0);
  fdent *f = imap_get(m->dv->fds, (u32)m->dv->disk_fd);
  if (!f) return -1;
  if (f->kind == FD_KBD) return kbd_read(m, (u32)m->dv->disk_addr, len, f->nonblock, f->raw);
  if (f->kind != FD_FILE) return -1;
  i64 n = (i64)f->len - f->pos;
  if (len < n) n = len;
  u32 got = n > 0 ? dma_write(m, (u32)m->dv->disk_addr, f->b->bytes + f->off + f->pos, (u32)n) : 0;
  f->pos += got;
  return (i32)got;
}

static char *w_path(drive *d, const char *rest) {
  const char *clean = (rest[0] == '.' && rest[1] == '/') ? rest + 2 : rest;
  if (clean[0] == '/') { while (*clean == '/') clean++; return xstrdup(clean); }
  const char *dir = d->ndirs ? d->dirs[0] : 0;
  if (dir && dir[0]) {
    size_t dl = strlen(dir), cl = strlen(clean);
    char *s = xmalloc(dl + cl + 2);
    memcpy(s, dir, dl); s[dl] = '/'; memcpy(s + dl + 1, clean, cl + 1);
    return s;
  }
  return xstrdup(clean);
}

static i32 disk_create(machine *m, u32 name_addr) {
  static char name[257];
  dma_cstring(m, name_addr, name, 256);
  if (!strncmp(name, "download:", 9)) return -1;     /* no one to hand it to */
  const char *rest;
  drive *d = resolve_drive(m, name, &rest);
  if (!d || !d->writable || !d->sink) return -1;
  i32 fd;
  fdent *f = fd_new(m, FD_WRITE, &fd);
  f->wd = d;
  f->wname = w_path(d, rest);
  return fd;
}

static i32 disk_write(machine *m, i32 len) {
  m->dv->disk_activity++;
  fdent *f = imap_get(m->dv->fds, (u32)m->dv->disk_fd);
  if (!f || f->kind != FD_WRITE) return -1;
  u32 n = len > 0 ? (u32)len : 0;
  if (f->wlen + n > f->wcap) {
    while (f->wlen + n > f->wcap) f->wcap = f->wcap ? f->wcap * 2 : 4096;
    f->wbuf = xrealloc(f->wbuf, f->wcap);
  }
  u32 got = dma_read(m, (u32)m->dv->disk_addr, n, f->wbuf + f->wlen);
  f->wlen += got;
  return (i32)got;
}

static i32 disk_wclose(machine *m, i32 fd) {
  fdent *f = imap_get(m->dv->fds, (u32)fd);
  if (!f || f->kind != FD_WRITE) return -1;
  blob *b = blob_new(f->wlen);
  memcpy(b->bytes, f->wbuf, f->wlen);
  drive_put(f->wd, f->wname, b);
  imap_del(m->dv->fds, (u32)fd);
  if (f->wd->sink) f->wd->sink(f->wd, f->wname, b);
  fd_free(f);
  return 0;
}

static i32 disk_unlink(machine *m, u32 name_addr) {
  static char name[257];
  dma_cstring(m, name_addr, name, 256);
  const char *rest;
  drive *d = resolve_drive(m, name, &rest);
  if (!d || !d->writable || !d->sink) return -1;
  char *clean = w_path(d, rest);
  blob *b = smap_del(d->files, clean);
  if (!b) { xfree(clean); return -1; }
  blob_unref(b);
  if (d->remove) d->remove(d, clean);
  xfree(clean);
  return 0;
}

static i32 uart_write(machine *m, i32 len) {
  if (len <= 0) return 0;
  u32 a = (u32)m->dv->uart_tx_addr, n = (u32)len;
  if (dma_plain(m, a)) {
    if (a >= m->size) return 0;
    if (n > m->size - a) n = m->size - a;
    for (u32 i = 0; i < n; i++) { m->dv->uart_activity++; con_byte(m, m->u8[a + i]); }
    return (i32)n;
  }
  static u8 *buf; static u32 cap;
  if (n > cap) { cap = n; buf = xrealloc(buf, cap); }
  n = dma_read(m, a, n, buf);
  for (u32 i = 0; i < n; i++) { m->dv->uart_activity++; con_byte(m, buf[i]); }
  return (i32)n;
}

i64 machine_sim_ms(machine *m) {
  if (m->dv->wall) return (i32)(plat_now_ms() - m->dv->wall_base);
  /* on threads, another CPU's: as CPU 0 last published it (par.c) */
  if (m->par_ap && !m->serving) return __atomic_load_n(&m->smp->time_ms, __ATOMIC_SEQ_CST);
  /* with several CPUs, the machine's time (the round robin's), not the
   * running CPU's own count (devices.js simMs) */
  return (i32)(machine_clock(m) / m->dv->cycles_per_ms + m->dv->sleep_ms);
}

int dev_opcode_by_name(const char *name) {
  char want[5] = "    ";
  for (int i = 0; i < 4 && name[i]; i++) want[i] = (char)(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]);
  for (int i = 0; i * 5 + 4 <= (int)strlen(OPNAMES); i++) {
    int ok = 1;
    for (int k = 0; k < 4; k++) {
      char c = OPNAMES[i * 5 + k];
      if (c >= 'a' && c <= 'z') c -= 32;
      if (c != want[k]) { ok = 0; break; }
    }
    if (ok) return i;
  }
  return -1;
}

i32 machine_info(machine *m);

i32 dev_read32(machine *m, u32 addr) {
  switch (addr) {
    case UART_RX: if (m->dv->rx_head < m->dv->rx_tail) return kbd_shift(m); m->dv->rx_starved = 1; return -1;
    case UART_RXAVL: if (m->dv->rx_head == m->dv->rx_tail) m->dv->rx_starved = 1; return (i32)(m->dv->rx_tail - m->dv->rx_head);
    case UART_TXADDR: return m->dv->uart_tx_addr;
    case UART_TXLEN: return m->dv->uart_tx_result;
    case TIME_MS: return (i32)machine_sim_ms(m);
    case CYCLE_LO: return (i32)m->cycle;
    case CYCLE_HI: return (i32)(m->cycle >> 32);
    case OPNAME: return m->dv->opname_result;
    case HEAP_BASE: return m->dv->heap_base_reg;
    case IMG_CODE: return (i32)m->dv->img_code;
    case IMG_DATA: return (i32)m->dv->img_data;
    case IMG_BSS: return (i32)m->dv->img_bss;
    case IMG_END: return (i32)m->dv->img_end;
    case HEAP_END: return m->dv->heap_end_reg;
    case STACK_BASE: return (i32)m->dv->stack_base;
    case STACK_TOP: return (i32)m->dv->stack_top_reg;
    case STACK_GUARD: return m->dv->stack_guard;
    case SWAP_CMD: return m->dv->swap_result;
    case HEAP_USED: return heap_used(m->heap);
    case HEAP_GEN: return heap_gen(m->heap);
    case HEAP_RELEASE: return heap_released(m->heap);
    case INFO_REG:
      return C4I_C4M | C4I_HRT | C4I_SIG | C4I_FLT | C4I_PROT | C4I_PIT | C4I_DRV |
             (m->no_fpu ? 0 : C4I_FPU) | (m->trap_handler ? C4I_TRAPH : 0);
    case TRAPH_REG: return m->trap_handler;
    case INTERVAL_REG: return m->cycle_interval;
    case TRESTORE_REG: return m->trap_restores_interval;
    case MODE_REG: return m->mode;
    case TT_REG: return m->tt;
    case TP_REG: return m->tp;
    case HND_REG: return m->hnd;
    case JMODE_REG: return m->jmode;
    case JINTERVAL_REG: return m->jinterval;
    case DISK_NAME: case DISK_LEN: case DISK_CLOSE: return m->dv->disk_result;
    case RTC_MS: return (i32)(plat_now_ms() - m->dv->t0);
    case PIT_MS: return m->dv->pit_ms;
    case DISK_DRIVE: return m->dv->drive;
    case DISK_COUNT: return m->dv->ndrives;
    case DISK_RO: {
      drive *d = m->dv->drive < m->dv->ndrives ? m->dv->drives[m->dv->drive] : 0;
      return (d && d->writable && d->sink) ? 0 : 1;
    }
    case DISK_WNAME: case DISK_WLEN: case DISK_WCLOSE: case DISK_UNLINK: return m->dv->disk_result;
    case DISK_DIRS: {
      drive *d = m->dv->drive < m->dv->ndrives ? m->dv->drives[m->dv->drive] : 0;
      return d ? d->ndirs : 0;
    }
    case CON_TAP: return (i32)m->dv->tap_len;
    case CON_ADDR: return m->dv->tap_addr;
    case CON_LEN: return m->dv->tap_result;
    case CON_IN: return m->dv->tap_in_on ? (i32)(m->dv->tap_in_len - m->dv->tap_in_pos) : -1;
    case MBOX_BASE: case MBOX_LEN: return 0;
    case MBOX_BELL: { i32 n = m->dv->mbox_irq; m->dv->mbox_irq = 0; return n; }
    default: return 0;
  }
}

static void set_dirs(drive *d, const char *s) {
  for (int i = 0; i < d->ndirs; i++) xfree(d->dirs[i]);
  xfree(d->dirs);
  int n = 1;
  for (const char *p = s; *p; p++) if (*p == ';') n++;
  d->dirs = xmalloc((size_t)n * sizeof(char *));
  d->ndirs = 0;
  const char *p = s;
  for (;;) {
    const char *q = strchr(p, ';');
    size_t l = q ? (size_t)(q - p) : strlen(p);
    char *t = xmalloc(l + 1); memcpy(t, p, l); t[l] = 0;
    d->dirs[d->ndirs++] = t;
    if (!q) break;
    p = q + 1;
  }
}

void dev_write32(machine *m, u32 addr, i32 val) {
  switch (addr) {
    case UART_TX: m->dv->uart_activity++; con_byte(m, val & 0xff); return;
    case UART_TXADDR: m->dv->uart_tx_addr = val; return;
    case UART_TXLEN: m->dv->uart_tx_result = uart_write(m, val); return;
    case USLP_US: {
      i64 ms = (i64)(val / 1000);
      m->dv->nap_ms = ms > 1 ? ms : 1;
      m->dv->sleep_ms += m->dv->nap_ms;
      m->dv->nap_until = machine_sim_ms(m) + m->dv->nap_ms;
      return;
    }
    case OPNAME: { char s[17]; dma_cstring(m, (u32)val, s, 16); m->dv->opname_result = dev_opcode_by_name(s); return; }
    case DISK_FLAGS: m->dv->disk_flags = val; return;
    case DISK_NAME: m->dv->disk_result = dev_disk_open(m, (u32)val); return;
    case DISK_FD: m->dv->disk_fd = val; return;
    case DISK_ADDR: m->dv->disk_addr = val; return;
    case DISK_LEN: m->dv->disk_result = dev_disk_read(m, val); return;
    case DISK_CLOSE: {
      fdent *f = imap_del(m->dv->fds, (u32)val);
      if (f) { if (f->kind == FD_KBD && f->raw) m->dv->raw_kbd--; fd_free(f); m->dv->disk_result = 0; }
      else m->dv->disk_result = -1;
      return;
    }
    case PIT_MS: {
      i32 ms = val > 0 ? val : 0;
      if (!ms) { m->dv->pit_ms = 0; return; }
      if (ms != m->dv->pit_armed || !m->dv->pit_next) m->dv->pit_next = plat_now_ms() + ms;
      m->dv->pit_ms = m->dv->pit_armed = ms;
      return;
    }
    case DISK_DRIVE: m->dv->drive = val >= 0 ? val : 0; return;
    case DISK_WNAME: m->dv->disk_result = disk_create(m, (u32)val); return;
    case DISK_WADDR: m->dv->disk_addr = val; return;
    case DISK_WLEN: m->dv->disk_result = disk_write(m, val); return;
    case DISK_WCLOSE: m->dv->disk_result = disk_wclose(m, val); return;
    case DISK_UNLINK: m->dv->disk_result = disk_unlink(m, (u32)val); return;
    case DISK_DIRS: {
      drive *d = m->dv->drive < m->dv->ndrives ? m->dv->drives[m->dv->drive] : 0;
      if (d) { static char s[1025]; dma_cstring(m, (u32)val, s, 1024); set_dirs(d, s); }
      return;
    }
    case CON_TAP: m->dv->tap_on = val ? 1 : 0; return;
    case CON_ADDR: m->dv->tap_addr = val; return;
    case CON_LEN: m->dv->tap_result = tap_drain(m, val); return;
    case CON_IN: {
      if (val < 0) { m->dv->tap_in_on = 0; return; }
      xfree(m->dv->tap_in);
      m->dv->tap_in = xmalloc((u32)val + 1);
      m->dv->tap_in_len = dma_read(m, (u32)m->dv->tap_addr, (u32)val, m->dv->tap_in);
      m->dv->tap_in_pos = 0;
      m->dv->tap_in_on = 1;
      return;
    }
    case DISK_EJECT: {
      if (val >= 0 && val < m->dv->ndrives) {
        drive *d = m->dv->drives[val];
        /* the files go: a fresh map (the old blobs are let go) */
        d->files = smap_new();
      }
      return;
    }
    case DISK_RESCAN: return;
    case RESET:
      m->dv->pit_ms = m->dv->pit_armed = 0; m->dv->pit_next = 0;
      m->dv->reset_requested = 1;
      m->dv->halted = 1; m->dv->status = val;
      return;
    case POWER: m->dv->halted = 1; m->dv->status = val; return;
    case HEAP_BASE: m->dv->heap_base_reg = val; return;
    case HEAP_END: m->dv->heap_end_reg = val; return;
    case STACK_GUARD: m->dv->stack_guard = (val && m->dv->stack_top_reg > m->dv->stack_base) ? 1 : 0; return;
    case SWAP_ADDR: m->dv->swap_addr = (u32)val; return;
    case SWAP_CMD: swap_lock(m); m->dv->swap_result = swap_op(m, val); swap_unlock(m); return;
    case HEAP_GEN: heap_set_gen(m->heap, val); return;
    case HEAP_RELEASE: heap_release(m->heap, val); return;
    case MBOX_BELL: return;
    case TRAPH_REG: m->trap_handler = val; return;
    case INTERVAL_REG: m->cycle_interval = val; return;
    case TRESTORE_REG: m->trap_restores_interval = val; return;
    case MODE_REG: m->mode = val; return;
    case TT_REG: m->tt = val; return;
    case TP_REG: m->tp = val; return;
    case HND_REG: m->hnd = val; return;
    default: return;
  }
}

/* for the display (gui.c) */
i64 dev_dma(machine *m, u32 addr, i32 len, int write, int pin) { return dma(m, addr, len, write, pin); }
u32 dev_dma_read(machine *m, u32 addr, u32 len, u8 *out) { return dma_read(m, addr, len, out); }
