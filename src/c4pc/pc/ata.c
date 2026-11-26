/* ata.c -- the PC's IDE disks as block devices (dev.c BLK_*; docs/c4ix-
 * ext2.md 1), for C4IX's own filesystem: every ATA disk on the two
 * channels (1F0h and 170h, master and slave) but the store's (store.c),
 * read and written by PIO, LBA28, and LBA48 where the disk is bigger
 * than LBA28 reaches (128 GB). A SATA disk is not spoken to (AHCI).
 *
 * Under a hypervisor a port read is a trip out to it, and an emulated
 * disk does a request's worth of work per DRQ block: a sector, unless
 * the disk is in multiple mode. So it is (SET MULTIPLE MODE, up to 16
 * sectors a block, READ/WRITE MULTIPLE), and the status is polled with
 * a pause between reads, which leaves the emulator's own thread the room
 * to finish the request: polled flat out, QEMU (under KVM, itself in a
 * VM) took 13 ms a sector to write, 38 KB a second. */
#include "../c4pc.h"
#include "../clib.h"

static inline void outb(u32 p, u8 v) { __asm__ volatile("outb %0,%w1" : : "a"(v), "Nd"(p)); }
static inline u8 inb(u32 p) { u8 v; __asm__ volatile("inb %w1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void insw(u32 p, void *b, u32 n) { __asm__ volatile("rep insw" : "+D"(b), "+c"(n) : "d"(p) : "memory"); }
static inline void outsw(u32 p, const void *b, u32 n) { __asm__ volatile("rep outsw" : "+S"(b), "+c"(n) : "d"(p) : "memory"); }

typedef struct ata { blkdev b; u32 io, ctl; int slave, lba48, mult, dma; } ata;

/* ---- the IDE controller's bus master (cdrom.c's ide_dma_*) ----
 * DMA: a command moves up to 128 sectors (64 KB) in one request, where
 * PIO is a request each DRQ block: in QEMU (under nested KVM) writes went
 * from 160 KB a second to many MB. The controller is PCI's class 1.1;
 * BAR 4 is its bus master's registers, 8 a channel. */
extern u32 ide_bm[2];
extern u8 *ide_dma_buf;
extern u32 *ide_prd;
void ide_dma_arm(int ch, u32 n, int tomem);
int ide_dma_run(int ch, int tomem, i64 ms);
static inline void outl(u32 p, u32 v) { __asm__ volatile("outl %0,%w1" : : "a"(v), "Nd"(p)); }
static inline u32 inl(u32 p) { u32 v; __asm__ volatile("inl %w1,%0" : "=a"(v) : "Nd"(p)); return v; }
static u32 pci_rd(u32 bus, u32 dev, u32 fn, u32 off) {
  outl(0xCF8, 0x80000000u | bus << 16 | dev << 11 | fn << 8 | (off & 0xfc));
  return inl(0xCFC);
}
static void pci_wr(u32 bus, u32 dev, u32 fn, u32 off, u32 v) {
  outl(0xCF8, 0x80000000u | bus << 16 | dev << 11 | fn << 8 | (off & 0xfc));
  outl(0xCFC, v);
}
/* the controller of the legacy ports (1F0h, 170h), if it has a bus
 * master: what to say on the console, 0 if none (PIO, then) */
const char *ide_pci_init(void) {
  for (u32 bus = 0; bus < 8; bus++)
    for (u32 dev = 0; dev < 32; dev++)
      for (u32 fn = 0; fn < 8; fn++) {
        u32 id = pci_rd(bus, dev, fn, 0);
        if ((id & 0xffff) == 0xffff) { if (fn == 0) break; continue; }
        u32 cls = pci_rd(bus, dev, fn, 8);
        if ((cls >> 16) == 0x0101 && (cls & 0x8000)) {     /* IDE, a bus master */
          u32 bar = pci_rd(bus, dev, fn, 0x20);
          /* compatibility mode on both channels (the legacy ports), and
           * the bus master in I/O space */
          if ((cls & 0x0500) || !(bar & 1) || !(bar & ~3u)) return 0;
          pci_wr(bus, dev, fn, 4, pci_rd(bus, dev, fn, 4) | 5);    /* I/O, bus master */
          u8 *raw = xmalloc(3 * 65536);
          ide_dma_buf = (u8 *)(((uintptr_t)raw + 65535) & ~(uintptr_t)65535);
          ide_prd = (u32 *)(ide_dma_buf + 65536);               /* 8 bytes, aligned, past the buffer */
          if ((uintptr_t)ide_dma_buf + 2 * 65536 > 0xFFFFFFFFu) return 0;
          ide_bm[0] = bar & ~3u; ide_bm[1] = (bar & ~3u) + 8;
          return "the IDE controller's DMA";
        }
        if (fn == 0 && !(pci_rd(bus, dev, 0, 0x0C) & 0x800000)) break;   /* one function */
      }
  return 0;
}

/* a microsecond without touching a port: the emulator's to use */
static void io_pause(void) { for (int i = 0; i < 64; i++) __asm__ volatile("pause"); }
/* The clock is looked at every 64th poll, not each: it is the TSC, and
 * VirtualBox emulates rdtsc, a trip out to it each, as the port read is */
static int wait(ata *d, int drq) {
  i64 t0 = plat_now_ms();
  for (u32 k = 1;; k++) {
    u8 s = inb(d->io + 7);
    if (!(s & 0x80)) {
      if (s & 0x21) return -1;                           /* an error, a fault */
      if (!drq || (s & 0x08)) return 0;
    }
    if (!(k & 63) && plat_now_ms() - t0 > 10000) return -1;
    io_pause();
  }
}
static void cmd(ata *d, u32 lba, u32 n, int write) {
  if (d->lba48) {
    outb(d->io + 6, (u8)(0x40 | d->slave << 4));
    outb(d->io + 2, (u8)(n >> 8)); outb(d->io + 3, (u8)(lba >> 24)); outb(d->io + 4, 0); outb(d->io + 5, 0);
    outb(d->io + 2, (u8)n); outb(d->io + 3, (u8)lba); outb(d->io + 4, (u8)(lba >> 8)); outb(d->io + 5, (u8)(lba >> 16));
    outb(d->io + 7, d->mult > 1 ? (write ? 0x39 : 0x29) : (write ? 0x34 : 0x24));   /* WRITE / READ (MULTIPLE | SECTORS) EXT */
  } else {
    outb(d->io + 6, (u8)(0xE0 | d->slave << 4 | ((lba >> 24) & 15)));
    outb(d->io + 2, (u8)n);
    outb(d->io + 3, (u8)lba); outb(d->io + 4, (u8)(lba >> 8)); outb(d->io + 5, (u8)(lba >> 16));
    outb(d->io + 7, d->mult > 1 ? (write ? 0xC5 : 0xC4) : (write ? 0x30 : 0x20));
  }
}
/* K sectors (to 128) at LBA by DMA: 0, or -1 */
static int rw_dma(ata *d, int write, u32 lba, u32 k, u8 *buf) {
  int ch = d->io == 0x170 ? 1 : 0;
  if (wait(d, 0)) return -1;
  if (write) memcpy(ide_dma_buf, buf, k * 512);
  ide_dma_arm(ch, k * 512, !write);
  if (d->lba48) {
    outb(d->io + 6, (u8)(0x40 | d->slave << 4));
    outb(d->io + 2, (u8)(k >> 8)); outb(d->io + 3, (u8)(lba >> 24)); outb(d->io + 4, 0); outb(d->io + 5, 0);
    outb(d->io + 2, (u8)k); outb(d->io + 3, (u8)lba); outb(d->io + 4, (u8)(lba >> 8)); outb(d->io + 5, (u8)(lba >> 16));
    outb(d->io + 7, write ? 0x35 : 0x25);                /* WRITE / READ DMA EXT */
  } else {
    outb(d->io + 6, (u8)(0xE0 | d->slave << 4 | ((lba >> 24) & 15)));
    outb(d->io + 2, (u8)k);
    outb(d->io + 3, (u8)lba); outb(d->io + 4, (u8)(lba >> 8)); outb(d->io + 5, (u8)(lba >> 16));
    outb(d->io + 7, write ? 0xCA : 0xC8);                /* WRITE / READ DMA */
  }
  if (ide_dma_run(ch, !write, 10000)) return -1;
  if (wait(d, 0)) return -1;
  if (!write) memcpy(buf, ide_dma_buf, k * 512);
  return 0;
}
static int rw_(blkdev *b, int write, u32 lba, u32 n, u8 *buf);
/* every transfer counted: the disk's, and all of them (HOST's io, diskioN) */
unsigned long long ata_io_rd, ata_io_wr;
static int rw(blkdev *b, int write, u32 lba, u32 n, u8 *buf) {
  int r = rw_(b, write, lba, n, buf);
  if (!r) {
    unsigned long long by = (unsigned long long)n * 512;
    if (write) { b->io_wr += by; ata_io_wr += by; } else { b->io_rd += by; ata_io_rd += by; }
  }
  return r;
}
static int rw_(blkdev *b, int write, u32 lba, u32 n, u8 *buf) {
  ata *d = (ata *)b;
  /* by DMA where it can; once that fails, by PIO from then on */
  while (n && d->dma) {
    u32 k = n > 128 ? 128 : n;
    if (rw_dma(d, write, lba, k, buf)) { d->dma = 0; break; }
    lba += k; n -= k; buf += k * 512;
  }
  while (n) {
    u32 k = n > 128 ? 128 : n;
    if (wait(d, 0)) return -1;
    cmd(d, lba, k, write);
    for (u32 i = 0; i < k; ) {
      u32 blk = k - i < (u32)d->mult ? k - i : (u32)d->mult;    /* a DRQ block */
      if (wait(d, 1)) return -1;
      if (write) outsw(d->io, buf, 256 * blk); else insw(d->io, buf, 256 * blk);
      buf += 512 * blk; i += blk;
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
  /* DMA, where the controller has a bus master and the disk says it can (word 49) */
  d->dma = ide_bm[d->io == 0x170 ? 1 : 0] && (id[49] & (1u << 8));
  /* multiple mode: as many sectors a DRQ block as it takes, to 16 */
  d->mult = 1;
  u32 mm = id[47] & 0xFF;
  if (mm >= 2) {
    if (mm > 16) mm = 16;
    while (mm & (mm - 1)) mm &= mm - 1;                /* a power of two */
    outb(d->io + 6, (u8)(0xA0 | d->slave << 4));
    outb(d->io + 2, (u8)mm);
    outb(d->io + 7, 0xC6);                               /* SET MULTIPLE MODE */
    if (!wait(d, 0)) d->mult = (int)mm;
  }
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

/* the ATA disk at IO's channel, master or SLAVE, as a block device: 0 if
 * there is none (the store's, handed over: store.c, store_release) */
blkdev *ata_one(u32 io, int slave) {
  ata *d = xmalloc(sizeof *d);
  d->io = io; d->ctl = io == 0x1F0 ? 0x3F6 : 0x376; d->slave = slave;
  if (!identify(d)) { xfree(d); return 0; }
  d->b.rw = rw; d->b.flush = flush; d->b.ctx = d;
  return &d->b;
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
