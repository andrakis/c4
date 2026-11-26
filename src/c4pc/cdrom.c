/* cdrom.c -- CD drives as host drives (from 2), so that C4IX can
 * mount a disc (src/c4ix/user/mount.c) and copy from it: what the web
 * page's uploads are for a browser, a second CD in the VM is for a PC.
 *
 * The drives are ATAPI ones on the IDE channels, as store.c's disks are
 * ATA ones (1F0h and 170h, master and slave; a SATA drive is not spoken
 * to), driven by polled PACKET commands (READ(10) by the controller's
 * DMA where it has a bus master, else PIO): TEST UNIT READY, REQUEST
 * SENSE, READ CAPACITY, READ(10), and START STOP UNIT to eject. The disc
 * is ISO 9660, its names Rock Ridge's where it has them (Linux's
 * mkisofs -R), else Joliet's (Windows'), else its own less ";1".
 *
 * Nothing of it is read into memory ahead: C4IX's mount makes a host
 * directory of "N:/" (vfs.c vfs_lazydir), and asks for N:DIR/.c4ix-dir
 * when it is first looked in, which is made here from the directory's
 * records, "SIZE NAME" or "d NAME" a line; a file is read from the disc
 * a range at a time as it is read (dev.c's drive hooks). N:.c4pc-drive
 * says what the drive is, for mount and df. Each mount looks at the
 * disc again (DISK_RESCAN); an eject (umount, DISK_EJECT) opens the
 * tray, where the machine has one.
 *
 * c4pc-host's --cdrom FILE is the same with an ISO file for the disc
 * (cdrom_image), for the tests. */
#include "c4pc.h"
#include "clib.h"

static inline void outb(u32 p, u8 v) { __asm__ volatile("outb %0,%w1" : : "a"(v), "Nd"(p)); }
static inline u8 inb(u32 p) { u8 v; __asm__ volatile("inb %w1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void insw(u32 p, void *b, u32 n) { __asm__ volatile("rep insw" : "+D"(b), "+c"(n) : "d"(p) : "memory"); }
static inline void outsw(u32 p, const void *b, u32 n) { __asm__ volatile("rep outsw" : "+S"(b), "+c"(n) : "d"(p) : "memory"); }

#define SECT 2048
#define MAXRUN 16                       /* sectors a READ(10) asks for at once */

/* The IDE controller's bus master (PCI IDE, as the PIIX's: VMware's,
 * QEMU's pc, a real PC's), for DMA: its registers for each channel (0 if
 * there is none: PIO), a buffer of 64 KB that crosses no 64 KB line, and
 * a table of one region. Found by pc/ata.c's ide_pci_init; c4pc-host
 * has none. One transfer at a time: the devices are CPU 0's. */
u32 ide_bm[2];
u8 *ide_dma_buf;
u32 *ide_prd;
static inline void outl_(u32 p, u32 v) { __asm__ volatile("outl %0,%w1" : : "a"(v), "Nd"(p)); }
/* the channel whose ports start at IO */
static int ide_ch(u32 io) { return io == 0x170 ? 1 : 0; }
/* ready a transfer of N bytes (to 64 KB) by channel CH's bus master,
 * TOMEM: from the device into memory */
void ide_dma_arm(int ch, u32 n, int tomem) {
  u32 bm = ide_bm[ch];
  ide_prd[0] = (u32)(uintptr_t)ide_dma_buf;
  ide_prd[1] = (n & 0xFFFF) | 0x80000000u;              /* 0: 64 KB; the table's end */
  outb(bm, 0);
  outl_(bm + 4, (u32)(uintptr_t)ide_prd);
  outb(bm + 2, (u8)(inb(bm + 2) | 6));                   /* the interrupt and error bits cleared */
  outb(bm, tomem ? 8 : 0);
}
static void io_pause_(void) { for (int i = 0; i < 64; i++) __asm__ volatile("pause"); }
/* start it, once the device has its command, and wait: 0, or -1 */
int ide_dma_run(int ch, int tomem, i64 ms) {
  u32 bm = ide_bm[ch];
  outb(bm, (u8)((tomem ? 8 : 0) | 1));
  i64 t0 = plat_now_ms();
  int r = 0;
  for (u32 k = 1;; k++) {
    u8 s = inb(bm + 2);
    if (s & 2) { r = -1; break; }                        /* the bus master's error */
    if (!(s & 1) || (s & 4)) break;                      /* no longer active, or the device's done */
    /* the clock every 64th poll: the TSC, a trip out of VirtualBox each */
    if (!(k & 63) && plat_now_ms() - t0 > ms) { r = -1; break; }
    io_pause_();
  }
  outb(bm, tomem ? 8 : 0);
  return r;
}

typedef struct cd {
  u32 io, ctl; int slave;
  /* an image instead of a drive (cdrom_image): its sectors, and how many */
  int (*rd)(void *ctx, u32 lba, u32 n, u8 *buf); void *rctx; u32 rblocks;
  char model[41], where[24];
  /* the disc, as the last look found it */
  int media; u32 blocks;                /* 2048-byte sectors */
  int no_dma;                           /* DMA failed once: PIO */
  char label[33];
  int joliet, rr;
  u32 root_lba, root_len;
  /* the last directory read, for the lookups that follow a listing */
  u32 dir_lba, dir_len; u8 *dir_buf;
  /* the last sector run read, for files read a little at a time */
  u32 run_lba, run_n; u8 *run_buf;
} cd;

/* ---- ATAPI, polled ----
 * A port read under a hypervisor is a trip out to it: the data is read a
 * DRQ block at a time (not a word at a time: 1,024 trips a sector, 90 KB
 * a second in QEMU under nested KVM), and the status polled with a pause
 * between, the emulator's thread's time to finish the read (pc/ata.c). */
static void io_pause(void) { for (int i = 0; i < 64; i++) __asm__ volatile("pause"); }
static int wait_ready(cd *c, i64 ms) {
  i64 t0 = plat_now_ms();
  for (;;) {
    u8 s = inb(c->io + 7);
    if (!(s & 0x80)) return s;
    if (plat_now_ms() - t0 > ms) return -1;
    io_pause();
  }
}
static void sel(cd *c) {
  outb(c->io + 6, (u8)(0xA0 | c->slave << 4));
  for (int i = 0; i < 15; i++) inb(c->io + 7);          /* 400 ns */
}
/* a PACKET command: the bytes it gives, into buf (at most max; the rest
 * read and dropped), or -1: an error (the sense then says why) */
static i32 packet(cd *c, const u8 *cmd, u8 *buf, u32 max, i64 ms) {
  if (wait_ready(c, 2000) < 0) return -1;
  sel(c);
  if (wait_ready(c, 2000) < 0) return -1;
  outb(c->io + 1, 0);                                    /* PIO */
  outb(c->io + 4, (u8)(MAXRUN * SECT)); outb(c->io + 5, (u8)((MAXRUN * SECT) >> 8));
  outb(c->io + 7, 0xA0);
  i64 t0 = plat_now_ms();
  for (;;) {
    u8 s = inb(c->io + 7);
    if (!(s & 0x80) && (s & 0x08)) break;
    if (!(s & 0x80) && (s & 0x01)) return -1;
    if (plat_now_ms() - t0 > 2000) return -1;
    io_pause();
  }
  u16 w[6]; memcpy(w, cmd, 12);
  outsw(c->io, w, 6);
  u32 got = 0;
  for (;;) {
    int s = wait_ready(c, ms);                           /* a disc spinning up takes seconds */
    if (s < 0) return -1;
    if (s & 0x01) return -1;
    if (!(s & 0x08)) break;                              /* done */
    u32 n = (u32)inb(c->io + 4) | (u32)inb(c->io + 5) << 8;
    if (!n) break;
    u32 words = (n + 1) / 2;
    u32 fit = got + words * 2 <= max ? words : (max - got) / 2;
    if (fit) { insw(c->io, buf + got, fit); got += fit * 2; words -= fit; }
    while (words) { u16 x; insw(c->io, &x, 1); words--; }   /* past what was wanted: read and dropped */
  }
  return (i32)got;
}
static int sense(cd *c, int *asc) {
  u8 cmd[12] = {0x03, 0, 0, 0, 18, 0}, b[18];
  memset(b, 0, sizeof b);
  if (packet(c, cmd, b, sizeof b, 2000) < 0) return -1;
  if (asc) *asc = b[12];
  return b[2] & 15;
}
/* is there a disc: 1, 0 none, after the drive has had its say (a disc
 * changed is UNIT ATTENTION once; one spinning up NOT READY a while) */
static int ready(cd *c) {
  u8 tur[12] = {0};
  for (int k = 0; k < 20; k++) {
    if (packet(c, tur, 0, 0, 5000) >= 0) return 1;
    int asc = 0, key = sense(c, &asc);
    if (key == 2 && asc == 0x3A) return 0;               /* MEDIUM NOT PRESENT */
    if (key != 6 && key != 2) return 0;
    i64 t = plat_now_ms(); while (plat_now_ms() - t < 100) ;
  }
  return 0;
}
/* READ(10) by DMA (the bus master's, ide_dma_*): K sectors into BUF, 0
 * or -1. One request, where PIO is one a sector */
static int read_dma(cd *c, const u8 *cmd, u8 *buf, u32 k) {
  int ch = ide_ch(c->io);
  if (wait_ready(c, 2000) < 0) return -1;
  sel(c);
  if (wait_ready(c, 2000) < 0) return -1;
  ide_dma_arm(ch, k * SECT, 1);
  outb(c->io + 1, 1);                                    /* DMA */
  outb(c->io + 4, 0); outb(c->io + 5, 0);
  outb(c->io + 7, 0xA0);
  i64 t0 = plat_now_ms();
  for (;;) {
    u8 s = inb(c->io + 7);
    if (!(s & 0x80) && (s & 0x08)) break;
    if (!(s & 0x80) && (s & 0x01)) return -1;
    if (plat_now_ms() - t0 > 2000) return -1;
    io_pause();
  }
  u16 w[6]; memcpy(w, cmd, 12);
  outsw(c->io, w, 6);
  if (ide_dma_run(ch, 1, 10000)) return -1;
  int s = wait_ready(c, 10000);
  if (s < 0 || (s & 0x01)) return -1;
  memcpy(buf, ide_dma_buf, k * SECT);
  return 0;
}
static int read_sectors(cd *c, u32 lba, u32 n, u8 *buf) {
  if (c->rd) return c->rd(c->rctx, lba, n, buf);
  while (n) {
    u32 k = n > MAXRUN ? MAXRUN : n;
    u8 cmd[12] = {0x28, 0, (u8)(lba >> 24), (u8)(lba >> 16), (u8)(lba >> 8), (u8)lba, 0, (u8)(k >> 8), (u8)k, 0, 0, 0};
    int ok = 0;
    /* by DMA where the controller has a bus master; once that fails, by
     * PIO from then on */
    if (ide_bm[ide_ch(c->io)] && !c->no_dma) {
      if (!read_dma(c, cmd, buf, k)) ok = 1;
      else { c->no_dma = 1; sense(c, 0); }
    }
    for (int tries = 0; tries < 3 && !ok; tries++) {
      if (packet(c, cmd, buf, k * SECT, 10000) == (i32)(k * SECT)) ok = 1;
      else sense(c, 0);
    }
    if (!ok) return -1;
    lba += k; n -= k; buf += k * SECT;
  }
  return 0;
}

/* an ATAPI device at this channel and position: 1, its model kept */
static int probe(cd *c) {
  if (inb(c->io + 7) == 0xFF) return 0;                  /* nothing on the channel */
  outb(c->ctl, 0x02);                                    /* no interrupts */
  sel(c);
  outb(c->io + 2, 0); outb(c->io + 3, 0); outb(c->io + 4, 0); outb(c->io + 5, 0);
  outb(c->io + 7, 0xEC);                                 /* IDENTIFY: a packet device refuses it, */
  if (inb(c->io + 7) == 0) return 0;
  if (wait_ready(c, 1000) < 0) return 0;
  if (inb(c->io + 4) != 0x14 || inb(c->io + 5) != 0xEB) {   /* and says it is one */
    /* an ATA disk (the store's, store.c) answers it: its 512 bytes read
     * and dropped, or its next command finds them still waiting */
    if (inb(c->io + 7) & 0x08) { u16 junk[256]; insw(c->io, junk, 256); }
    return 0;
  }
  outb(c->io + 7, 0xA1);                                 /* IDENTIFY PACKET DEVICE */
  i64 t0 = plat_now_ms();
  for (;;) {
    u8 s = inb(c->io + 7);
    if (!(s & 0x80) && (s & 0x08)) break;
    if (!(s & 0x80) && (s & 0x01)) return 0;
    if (plat_now_ms() - t0 > 1000) return 0;
  }
  u16 id[256];
  insw(c->io, id, 256);
  if (((id[0] >> 8) & 0x1F) != 5) return 0;             /* not a CD/DVD drive */
  for (int i = 0; i < 20; i++) { c->model[2 * i] = (char)(id[27 + i] >> 8); c->model[2 * i + 1] = (char)id[27 + i]; }
  c->model[40] = 0;
  for (int i = 39; i >= 0 && c->model[i] == ' '; i--) c->model[i] = 0;
  return 1;
}

/* ---- ISO 9660 ---- */
static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

/* the disc looked at again: its volume descriptors */
static void look(cd *c) {
  c->media = 0; c->blocks = 0; c->label[0] = 0; c->joliet = c->rr = 0;
  c->dir_len = 0; c->run_n = 0;
  if (c->rd) c->blocks = c->rblocks;
  else {
    if (!ready(c)) return;
    u8 cap[8], rc[12] = {0x25};
    if (packet(c, rc, cap, 8, 5000) == 8) c->blocks = ((u32)cap[0] << 24 | (u32)cap[1] << 16 | (u32)cap[2] << 8 | cap[3]) + 1;
  }
  u8 *s = xmalloc(SECT);
  u32 jlba = 0, jlen = 0;
  for (u32 lba = 16; lba < 64; lba++) {
    if (read_sectors(c, lba, 1, s)) break;
    if (memcmp(s + 1, "CD001", 5)) break;
    if (s[0] == 255) break;
    if (s[0] == 1 && !c->root_lba) {
      c->root_lba = le32(s + 156 + 2); c->root_len = le32(s + 156 + 10);
      memcpy(c->label, s + 40, 32); c->label[32] = 0;
      for (int i = 31; i >= 0 && (c->label[i] == ' ' || !c->label[i]); i--) c->label[i] = 0;
      if (!c->blocks) c->blocks = le32(s + 80);
    }
    if (s[0] == 2 && s[88] == '%' && s[89] == '/' && (s[90] == '@' || s[90] == 'C' || s[90] == 'E')) {
      jlba = le32(s + 156 + 2); jlen = le32(s + 156 + 10);
    }
  }
  if (c->root_lba) {
    c->media = 1;
    /* Rock Ridge: the root's "." record's system use starts with SP */
    if (!read_sectors(c, c->root_lba, 1, s)) {
      u32 nl = s[32], su = 33 + nl + !(nl & 1);
      if (su + 7 <= s[0] && s[su] == 'S' && s[su + 1] == 'P' && s[su + 4] == 0xBE && s[su + 5] == 0xEF) c->rr = 1;
    }
    if (!c->rr && jlba) { c->joliet = 1; c->root_lba = jlba; c->root_len = jlen; }
  }
  xfree(s);
}
static void rescan(drive *d) { cd *c = d->ctx; c->root_lba = 0; look(c); }

/* one directory record's name, as C4IX gets it (UTF-8), into out */
static void rec_name(cd *c, const u8 *r, char *out, u32 max) {
  u32 nl = r[32], o = 0;
  const u8 *n = r + 33;
  if (c->rr) {
    /* NM entries in the system use area, a piece each */
    u32 su = 33 + nl + !(nl & 1), end = r[0];
    int found = 0;
    while (su + 4 <= end) {
      u32 el = r[su + 2];
      if (el < 4 || su + el > end) break;
      if (r[su] == 'N' && r[su + 1] == 'M' && el > 5 && !(r[su + 4] & 6)) {
        for (u32 k = 5; k < el && o + 1 < max; k++) out[o++] = (char)r[su + k];
        found = 1;
      }
      su += el;
    }
    if (found) { out[o] = 0; return; }
  }
  if (c->joliet) {
    for (u32 k = 0; k + 1 < nl; k += 2) {
      u32 ch = (u32)n[k] << 8 | n[k + 1];
      if (ch == ';') break;
      if (ch < 0x80) { if (o + 1 < max) out[o++] = (char)ch; }
      else if (ch < 0x800) { if (o + 2 < max) { out[o++] = (char)(0xC0 | ch >> 6); out[o++] = (char)(0x80 | (ch & 63)); } }
      else if (o + 3 < max) { out[o++] = (char)(0xE0 | ch >> 12); out[o++] = (char)(0x80 | ((ch >> 6) & 63)); out[o++] = (char)(0x80 | (ch & 63)); }
    }
  } else {
    for (u32 k = 0; k < nl && n[k] != ';' && o + 1 < max; k++) out[o++] = (char)n[k];
    if (o && out[o - 1] == '.') o--;                     /* "README." */
  }
  out[o] = 0;
}

/* a directory's records, read (the last one kept) */
static const u8 *dir_read(cd *c, u32 lba, u32 len) {
  if (c->dir_len && c->dir_lba == lba && c->dir_len == len) return c->dir_buf;
  if (len > (16u << 20)) return 0;
  u32 n = (len + SECT - 1) / SECT;
  xfree(c->dir_buf);
  c->dir_buf = xmalloc((size_t)n * SECT);
  c->dir_len = 0;
  if (read_sectors(c, lba, n, c->dir_buf)) return 0;
  c->dir_lba = lba; c->dir_len = len;
  return c->dir_buf;
}
/* each record of a directory but . and ..: f(record, name, ctx); 1 stops */
static int dir_each(cd *c, u32 lba, u32 len, int (*f)(cd *, const u8 *, const char *, void *), void *ctx) {
  const u8 *b = dir_read(c, lba, len);
  if (!b) return -1;
  char name[256];
  u32 p = 0;
  while (p < len) {
    u32 rl = b[p];
    if (!rl) { p = (p / SECT + 1) * SECT; continue; }    /* the rest of the sector is padding */
    if (p + rl > len || rl < 34) break;
    const u8 *r = b + p;
    if (!(r[32] == 1 && (r[33] == 0 || r[33] == 1))) {
      rec_name(c, r, name, sizeof name);
      if (name[0] && f(c, r, name, ctx)) return 1;
    }
    p += rl;
  }
  return 0;
}
struct find_q { const char *want; u32 wl; u32 lba, len; int isdir, found; };
static int find_fn(cd *c, const u8 *r, const char *name, void *ctx) {
  (void)c;
  struct find_q *q = ctx;
  if (strlen(name) != q->wl || memcmp(name, q->want, q->wl)) return 0;
  q->lba = le32(r + 2); q->len = le32(r + 10); q->isdir = (r[25] & 2) != 0; q->found = 1;
  return 1;
}
/* a path from the root: its extent and size, 1 a directory; -1 none */
static int walk(cd *c, const char *path, u32 *lba, u32 *len) {
  u32 l = c->root_lba, n = c->root_len;
  int isdir = 1;
  const char *p = path;
  while (*p == '/') p++;
  while (*p) {
    const char *e = p; while (*e && *e != '/') e++;
    if (!isdir) return -1;
    struct find_q q = { p, (u32)(e - p), 0, 0, 0, 0 };
    if (dir_each(c, l, n, find_fn, &q) < 0 || !q.found) return -1;
    l = q.lba; n = q.len; isdir = q.isdir;
    p = e; while (*p == '/') p++;
  }
  *lba = l; *len = n;
  return isdir;
}

/* ---- the drive's hooks (dev.c) ---- */
struct list_q { obuf *o; };
static void ob_num(obuf *o, u32 v) { char t[12]; int k = 0; do t[k++] = (char)('0' + v % 10); while (v /= 10); while (k) ob_put(o, t[--k]); }
static int list_fn(cd *c, const u8 *r, const char *name, void *ctx) {
  (void)c;
  obuf *o = ((struct list_q *)ctx)->o;
  if (r[25] & 2) { ob_put(o, 'd'); }
  else ob_num(o, le32(r + 10));
  ob_put(o, ' '); ob_puts(o, name); ob_put(o, '\n');
  return 0;
}
static blob *blob_of(obuf *o) { blob *b = blob_new(o->n); memcpy(b->bytes, o->b, o->n); xfree(o->b); return b; }
/* the made files: DIR/.c4ix-dir, the listing; .c4pc-drive, what this is */
static blob *fetch(drive *d, const char *name) {
  cd *c = d->ctx;
  size_t n = strlen(name);
  obuf o = {0, 0, 0};
  if (!strcmp(name, ".c4pc-drive")) {
    ob_puts(&o, "kind cdrom\ndevice "); ob_puts(&o, c->where);
    ob_puts(&o, "\nmodel "); ob_puts(&o, c->model[0] ? c->model : "?");
    ob_puts(&o, "\nmedia "); ob_puts(&o, c->media ? "yes" : "no");
    if (c->media) {
      ob_puts(&o, "\nlabel "); ob_puts(&o, c->label[0] ? c->label : "-");
      ob_puts(&o, "\nfs iso9660"); ob_puts(&o, c->rr ? " rock-ridge" : c->joliet ? " joliet" : "");
      ob_puts(&o, "\nsize "); ob_num(&o, c->blocks * 2); ob_puts(&o, "\nused "); ob_num(&o, c->blocks * 2);
      ob_puts(&o, "\nfree 0");
    }
    ob_put(&o, '\n');
    return blob_of(&o);
  }
  if (n >= 9 && !strcmp(name + n - 9, ".c4ix-dir")) {
    if (!c->media) return 0;
    char dir[1024];
    if (n - 9 >= sizeof dir) return 0;
    memcpy(dir, name, n - 9); dir[n - 9] = 0;
    u32 l, len;
    if (walk(c, dir, &l, &len) != 1) return 0;
    struct list_q q = { &o };
    if (dir_each(c, l, len, list_fn, &q) < 0) { xfree(o.b); return 0; }
    if (!o.b) ob_puts(&o, "");
    return blob_of(&o);
  }
  return 0;
}
/* a file on the disc: its size, and where it is (its first sector) */
static int find(drive *d, const char *name, u32 *size, u32 *where) {
  cd *c = d->ctx;
  if (!c->media) return -1;
  u32 l, len;
  if (walk(c, name, &l, &len) != 0) return -1;
  *size = len; *where = l;
  return 0;
}
/* n bytes at pos of the file whose first sector is where */
static u32 pread_(drive *d, u32 where, u32 pos, u8 *buf, u32 n) {
  cd *c = d->ctx;
  u32 done = 0;
  if (!c->run_buf) c->run_buf = xmalloc(MAXRUN * SECT);
  while (done < n) {
    u32 at = pos + done, lba = where + at / SECT, off = at % SECT;
    if (!(c->run_n && lba >= c->run_lba && lba < c->run_lba + c->run_n)) {
      u32 k = MAXRUN;
      if (c->blocks && lba + k > c->blocks) k = c->blocks > lba ? c->blocks - lba : 1;
      if (read_sectors(c, lba, k, c->run_buf)) { c->run_n = 0; return done; }
      c->run_lba = lba; c->run_n = k;
    }
    u32 in = (c->run_lba + c->run_n - lba) * SECT - off;
    u32 take = n - done < in ? n - done : in;
    memcpy(buf + done, c->run_buf + (lba - c->run_lba) * SECT + off, take);
    done += take;
  }
  return done;
}
static void eject(drive *d) {
  cd *c = d->ctx;
  u8 allow[12] = {0x1E}, stop[12] = {0x1B, 0, 0, 0, 0x02};
  if (!c->rd) { packet(c, allow, 0, 0, 2000); packet(c, stop, 0, 0, 10000); }
  c->media = 0; c->root_lba = 0; c->dir_len = 0; c->run_n = 0;
}

/* every CD drive on the IDE channels, as a drive each into drives[*nd]
 * on (at most max); how many. log: a line each, for the console, which
 * numbers them from base */
/* a drive of an image: rd reads its sectors (ctx its own), blocks of
 * them; what, the console's name for it */
drive *cdrom_image(int (*rd)(void *, u32, u32, u8 *), void *ctx, u32 blocks, const char *what) {
  cd *c = xmalloc(sizeof *c);
  c->rd = rd; c->rctx = ctx; c->rblocks = blocks;
  size_t n = strlen(what); if (n > 23) n = 23;
  memcpy(c->where, what, n); c->where[n] = 0;
  memcpy(c->model, "an image", 9);
  look(c);
  drive *d = drive_new(0);
  d->ctx = c; d->fetch = fetch; d->find = find; d->pread = pread_; d->eject = eject; d->rescan = rescan;
  return d;
}

int cdrom_find(drive **drives, int *nd, int max, int base, void (*log)(const char *)) {
  static const u32 ports[2][2] = {{0x1F0, 0x3F6}, {0x170, 0x376}};
  int found = 0;
  for (int ch = 0; ch < 2; ch++)
    for (int s = 0; s < 2 && *nd < max; s++) {
      cd *c = xmalloc(sizeof *c);
      c->io = ports[ch][0]; c->ctl = ports[ch][1]; c->slave = s;
      if (!probe(c)) { xfree(c); continue; }
      { const char *w1 = ch ? "secondary " : "primary ", *w2 = s ? "slave" : "master";
        size_t a1 = strlen(w1), a2 = strlen(w2);
        memcpy(c->where, w1, a1); memcpy(c->where + a1, w2, a2 + 1); }
      look(c);
      drive *d = drive_new(0);
      d->ctx = c; d->fetch = fetch; d->find = find; d->pread = pread_; d->eject = eject; d->rescan = rescan;
      if (log) {
        char line[160]; obuf o = {0, 0, 0};
        ob_puts(&o, "c4pc: drive "); ob_num(&o, (u32)(base + *nd)); ob_puts(&o, ": a CD drive on the "); ob_puts(&o, c->where);
        ob_puts(&o, " ("); ob_puts(&o, c->model); ob_puts(&o, "), ");
        if (c->media) { ob_puts(&o, "a disc '"); ob_puts(&o, c->label); ob_puts(&o, "', "); ob_num(&o, c->blocks / 512); ob_puts(&o, " MB"); }
        else ob_puts(&o, "no disc");
        ob_put(&o, '\n');
        u32 k = o.n < sizeof line - 1 ? o.n : sizeof line - 1;
        memcpy(line, o.b, k); line[k] = 0; xfree(o.b);
        log(line);
      }
      drives[(*nd)++] = d;
      found++;
    }
  return found;
}
