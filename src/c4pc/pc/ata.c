/* ata.c -- the PC's IDE disks as block devices (dev.c BLK_*; docs/c4ix-
 * ext2.md 1), for C4IX's own filesystem: every ATA disk on the two
 * channels (1F0h and 170h, master and slave) but the store's (store.c),
 * read and written by PIO, LBA28, and LBA48 where the disk is bigger
 * than LBA28 reaches (128 GB). A SATA disk is not spoken to (AHCI). */
#include "../c4pc.h"
#include "../clib.h"

static inline void outb(u32 p, u8 v) { __asm__ volatile("outb %0,%w1" : : "a"(v), "Nd"(p)); }
static inline u8 inb(u32 p) { u8 v; __asm__ volatile("inb %w1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void insw(u32 p, void *b, u32 n) { __asm__ volatile("rep insw" : "+D"(b), "+c"(n) : "d"(p) : "memory"); }
static inline void outsw(u32 p, const void *b, u32 n) { __asm__ volatile("rep outsw" : "+S"(b), "+c"(n) : "d"(p) : "memory"); }

typedef struct ata { blkdev b; u32 io, ctl; int slave, lba48; } ata;

static int wait(ata *d, int drq) {
  i64 t0 = plat_now_ms();
  for (;;) {
    u8 s = inb(d->io + 7);
    if (!(s & 0x80)) {
      if (s & 0x21) return -1;                           /* an error, a fault */
      if (!drq || (s & 0x08)) return 0;
    }
    if (plat_now_ms() - t0 > 10000) return -1;
  }
}
static void cmd(ata *d, u32 lba, u32 n, int write) {
  if (d->lba48) {
    outb(d->io + 6, (u8)(0x40 | d->slave << 4));
    outb(d->io + 2, (u8)(n >> 8)); outb(d->io + 3, (u8)(lba >> 24)); outb(d->io + 4, 0); outb(d->io + 5, 0);
    outb(d->io + 2, (u8)n); outb(d->io + 3, (u8)lba); outb(d->io + 4, (u8)(lba >> 8)); outb(d->io + 5, (u8)(lba >> 16));
    outb(d->io + 7, write ? 0x34 : 0x24);                /* WRITE / READ SECTORS EXT */
  } else {
    outb(d->io + 6, (u8)(0xE0 | d->slave << 4 | ((lba >> 24) & 15)));
    outb(d->io + 2, (u8)n);
    outb(d->io + 3, (u8)lba); outb(d->io + 4, (u8)(lba >> 8)); outb(d->io + 5, (u8)(lba >> 16));
    outb(d->io + 7, write ? 0x30 : 0x20);
  }
}
static int rw(blkdev *b, int write, u32 lba, u32 n, u8 *buf) {
  ata *d = (ata *)b;
  while (n) {
    u32 k = n > 128 ? 128 : n;
    if (wait(d, 0)) return -1;
    cmd(d, lba, k, write);
    for (u32 i = 0; i < k; i++) {
      if (wait(d, 1)) return -1;
      if (write) outsw(d->io, buf, 256); else insw(d->io, buf, 256);
      buf += 512;
    }
    if (write && wait(d, 0)) return -1;
    lba += k; n -= k;
  }
  return 0;
}
static int flush(blkdev *b) {
  ata *d = (ata *)b;
  if (wait(d, 0)) return -1;
  outb(d->io + 6, (u8)(0xA0 | d->slave << 4));
  outb(d->io + 7, d->lba48 ? 0xEA : 0xE7);               /* FLUSH CACHE (EXT) */
  return wait(d, 0);
}

/* an ATA disk at this channel and position: 1, its size in d */
static int identify(ata *d) {
  if (inb(d->io + 7) == 0xFF) return 0;
  outb(d->ctl, 0x02);
  outb(d->io + 6, (u8)(0xA0 | d->slave << 4));
  for (int i = 0; i < 15; i++) inb(d->io + 7);
  outb(d->io + 2, 0); outb(d->io + 3, 0); outb(d->io + 4, 0); outb(d->io + 5, 0);
  outb(d->io + 7, 0xEC);
  if (inb(d->io + 7) == 0) return 0;
  i64 t0 = plat_now_ms();
  while (inb(d->io + 7) & 0x80) if (plat_now_ms() - t0 > 2000) return 0;
  if (inb(d->io + 4) || inb(d->io + 5)) return 0;      /* ATAPI or SATA */
  if (wait(d, 1)) return 0;
  u16 id[256];
  insw(d->io, id, 256);
  u32 s28 = (u32)id[60] | (u32)id[61] << 16;
  uint64_t s48 = (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32;
  d->lba48 = (id[83] & (1u << 10)) && s48 > s28;
  uint64_t s = d->lba48 ? s48 : s28;
  d->b.sectors = s > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)s;      /* 2 TB at most */
  char *w = d->b.what; int k = 0;
  const char *pos = d->io == 0x1F0 ? "primary " : "secondary ";
  while (*pos) w[k++] = *pos++;
  pos = d->slave ? "slave, " : "master, ";
  while (*pos) w[k++] = *pos++;
  for (int i = 0; i < 20 && k < 46; i++) {               /* the model, its words byte-swapped */
    char a = (char)(id[27 + i] >> 8), b = (char)id[27 + i];
    if (a != ' ' || (k && w[k - 1] != ' ')) w[k++] = a;
    if (k < 46 && (b != ' ' || w[k - 1] != ' ')) w[k++] = b;
  }
  while (k && w[k - 1] == ' ') k--;
  w[k] = 0;
  return d->b.sectors > 0;
}

/* every ATA disk but the one at (skip_io, skip_slave), as block devices
 * into blks[*n] on, at most max; how many */
int ata_find(blkdev **blks, int *n, int max, u32 skip_io, int skip_slave) {
  static const u32 ports[2][2] = {{0x1F0, 0x3F6}, {0x170, 0x376}};
  int found = 0;
  for (int ch = 0; ch < 2; ch++)
    for (int s = 0; s < 2 && *n < max; s++) {
      if (ports[ch][0] == skip_io && s == skip_slave) continue;
      ata *d = xmalloc(sizeof *d);
      d->io = ports[ch][0]; d->ctl = ports[ch][1]; d->slave = s;
      if (!identify(d)) { xfree(d); continue; }
      d->b.rw = rw; d->b.flush = flush; d->b.ctx = d;
      blks[(*n)++] = &d->b;
      found++;
    }
  return found;
}
