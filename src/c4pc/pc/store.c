/* store.c -- a drive that outlasts the machine (M6): host drive 1, which
 * C4IX's persist keeps its saves on (src/c4ix/user/persist.c), kept on an
 * ATA disk, as the page keeps it in IndexedDB and cli.js in a -w folder.
 *
 * The disk is spoken to by PIO (the ATA ports at 1F0h and 170h, master
 * and slave, LBA28): QEMU's -hda or -drive if=ide, VMware's and
 * VirtualBox's IDE disks, a PC's in IDE mode. It is ours when its first
 * sector says "C4PC STORE 1"; with --store a disk whose first 64 sectors
 * are all zero (a new, blank virtual disk) is made ours. No other disk is
 * ever written.
 *
 * Its layout, in 512-byte sectors:
 *   0        the header: the magic, then the directory's and the disk's size
 *   1-64     the directory: 256 entries of 128 bytes, each a file's name
 *            (empty: free), its first sector, its sectors, its length and
 *            its CRC-32
 *   65-      the files, each in one run of sectors
 * A file written goes to a run no file uses (not even its old self), and
 * then its directory sector is written: cut short, the disk has the old
 * file or the new one. A file removed is its entry cleared. At boot every
 * file the directory names is read into the drive, its CRC checked. */
#include "../c4pc.h"
#include "../clib.h"

static inline void outb(u32 p, u8 v) { __asm__ volatile("outb %0,%w1" : : "a"(v), "Nd"(p)); }
static inline u8 inb(u32 p) { u8 v; __asm__ volatile("inb %w1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void insw(u32 p, void *b, u32 n) { __asm__ volatile("rep insw" : "+D"(b), "+c"(n) : "d"(p) : "memory"); }
static inline void outsw(u32 p, const void *b, u32 n) { __asm__ volatile("rep outsw" : "+S"(b), "+c"(n) : "d"(p) : "memory"); }

#define MAGIC "C4PC STORE 1"
#define DIRS 64                         /* directory sectors */
#define ENTRIES (DIRS * 4)              /* 128-byte entries */
#define DATA0 (1 + DIRS)
#define NAMEMAX 100
typedef struct { char name[NAMEMAX]; u32 start, nsect, len, crc, gen, pad[2]; } entry;
static entry dir[ENTRIES];
static u32 io, total;                   /* the channel's ports, the disk's sectors */
static int slave;
static u8 sec[512];
static void (*say)(const char *);
static int full_said;
u32 store_writes, store_bytes;

/* ---- ATA, by PIO ---- */
static int ata_wait(int drq) {
  for (u32 n = 0; n < 5000000; n++) {
    u8 s = inb(io + 7);
    if (s & 0x80) continue;                          /* busy */
    if (s & 0x21) return -1;                         /* an error, a fault */
    if (!drq || (s & 0x08)) return 0;
  }
  return -1;
}
static void ata_lba(u32 lba, u32 count) {
  outb(io + 6, (u8)(0xE0 | slave << 4 | ((lba >> 24) & 15)));
  outb(io + 2, (u8)count);
  outb(io + 3, (u8)lba); outb(io + 4, (u8)(lba >> 8)); outb(io + 5, (u8)(lba >> 16));
}
static int ata_read(u32 lba, u32 n, u8 *buf) {
  while (n > 0) {
    u32 k = n > 255 ? 255 : n;
    if (ata_wait(0)) return -1;
    ata_lba(lba, k);
    outb(io + 7, 0x20);
    for (u32 i = 0; i < k; i++) { if (ata_wait(1)) return -1; insw(io, buf, 256); buf += 512; }
    lba += k; n -= k;
  }
  return 0;
}
static int ata_write(u32 lba, u32 n, const u8 *buf) {
  while (n > 0) {
    u32 k = n > 255 ? 255 : n;
    if (ata_wait(0)) return -1;
    ata_lba(lba, k);
    outb(io + 7, 0x30);
    for (u32 i = 0; i < k; i++) { if (ata_wait(1)) return -1; outsw(io, buf, 256); buf += 512; }
    if (ata_wait(0)) return -1;
    lba += k; n -= k;
  }
  outb(io + 7, 0xE7);                                /* the disk's cache, flushed */
  return ata_wait(0);
}
/* an ATA disk at this channel and position: its sectors, or 0 */
static u32 identify(u32 port, u32 ctl, int sl) {
  io = port; slave = sl;
  if (inb(io + 7) == 0xFF) return 0;                 /* nothing on the channel */
  outb(ctl, 0x02);                                   /* no interrupts */
  outb(io + 6, (u8)(0xA0 | sl << 4));
  for (int i = 0; i < 15; i++) inb(io + 7);          /* 400 ns */
  outb(io + 2, 0); outb(io + 3, 0); outb(io + 4, 0); outb(io + 5, 0);
  outb(io + 7, 0xEC);
  if (inb(io + 7) == 0) return 0;
  for (u32 n = 0; n < 5000000 && (inb(io + 7) & 0x80); n++) ;
  if (inb(io + 4) || inb(io + 5)) return 0;          /* ATAPI or SATA: not an ATA disk */
  if (ata_wait(1)) return 0;
  u16 id[256];
  insw(io, id, 256);
  return (u32)id[60] | (u32)id[61] << 16;            /* LBA28's sectors */
}

/* ---- CRC-32 ---- */
static u32 crc_t[256];
static u32 crc32(const u8 *p, u32 n) {
  if (!crc_t[1]) for (u32 i = 0; i < 256; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_t[i] = c; }
  u32 c = ~0u;
  while (n--) c = crc_t[(c ^ *p++) & 255] ^ (c >> 8);
  return ~c;
}

/* ---- the store ---- */
static int write_dir_sector(u32 i) { return ata_write(1 + i / 4, 1, (const u8 *)&dir[i & ~3u]); }
static i32 find(const char *name) {
  for (u32 i = 0; i < ENTRIES; i++) if (dir[i].name[0] && !strcmp(dir[i].name, name)) return (i32)i;
  return -1;
}
/* a run of n sectors no file uses: the first that fits */
static u32 alloc(u32 n) {
  u32 at = DATA0;
  for (;;) {
    u32 next = total;
    int clash = 0;
    for (u32 i = 0; i < ENTRIES; i++) {
      if (!dir[i].name[0] || !dir[i].nsect) continue;
      u32 s = dir[i].start, e = s + dir[i].nsect;
      if (s < at + n && e > at) { clash = 1; if (e > at) at = e; break; }
      if (s >= at && s < next) next = s;
    }
    if (clash) continue;
    if (at + n <= total) return at;
    return 0;
  }
}
/* what .c4pc-drive says: the store's KB in all, used and free */
static void tally(drive *d) {
  u32 used = DATA0;
  for (u32 i = 0; i < ENTRIES; i++) if (dir[i].name[0]) used += dir[i].nsect;
  d->kind = "store";
  d->size_kb = total / 2; d->used_kb = used / 2; d->free_kb = total > used ? (total - used) / 2 : 0;
}
static void store_sink(drive *d, const char *name, blob *b) {
  if (strlen(name) >= NAMEMAX) return;
  u32 n = (b->len + 511) / 512;
  i32 i = find(name);
  if (i < 0) { for (u32 k = 0; k < ENTRIES; k++) if (!dir[k].name[0]) { i = (i32)k; break; } }
  u32 at = n ? alloc(n) : DATA0;
  if (i < 0 || !at) {
    if (!full_said && say) { say("c4pc: the store is full: "); say(name); say(" is kept in RAM only\n"); }
    full_said = 1;
    return;
  }
  if (n) {
    u8 *buf = xmalloc((size_t)n * 512);
    memcpy(buf, b->bytes, b->len);
    int r = ata_write(at, n, buf);
    xfree(buf);
    if (r) return;
  }
  entry e;
  memset(&e, 0, sizeof e);
  memcpy(e.name, name, strlen(name) + 1);
  e.start = at; e.nsect = n; e.len = b->len; e.crc = crc32(b->bytes, b->len);
  e.gen = dir[i].gen + 1;
  dir[i] = e;
  write_dir_sector((u32)i);
  store_writes++; store_bytes += b->len;
  tally(d);
}
static void store_remove(drive *d, const char *name) {
  i32 i = find(name);
  if (i < 0) return;
  memset(&dir[i], 0, sizeof dir[i]);
  write_dir_sector((u32)i);
  tally(d);
}

/* the store's disk found (or, with create, a blank one made one) and its
 * files laid into a new drive: the drive, or 0. what: for the console */
drive *store_open(int create, void (*log)(const char *), char *what, u32 whatlen) {
  static const u32 ports[2][2] = {{0x1F0, 0x3F6}, {0x170, 0x376}};
  say = log;
  int found = 0;
  for (int c = 0; c < 2 && !found; c++)
    for (int s = 0; s < 2 && !found; s++) {
      u32 nsect = identify(ports[c][0], ports[c][1], s);
      if (nsect < DATA0 + 64) continue;
      if (ata_read(0, 1, sec)) continue;
      if (!memcmp(sec, MAGIC, sizeof MAGIC - 1)) { total = nsect; found = 1; break; }
      if (!create) continue;
      int blank = 1;
      for (u32 k = 0; k < 64 && blank; k++) {
        if (ata_read(k, 1, sec)) { blank = 0; break; }
        for (int j = 0; j < 512; j++) if (sec[j]) { blank = 0; break; }
      }
      if (!blank) continue;
      memset(dir, 0, sizeof dir);
      for (u32 k = 0; k < DIRS; k += 16) if (ata_write(1 + k, 16, (const u8 *)&dir[k * 4])) { blank = 0; break; }
      if (!blank) continue;
      memset(sec, 0, 512);
      memcpy(sec, MAGIC, sizeof MAGIC - 1);
      u32 hdr[2] = {DIRS, nsect};
      memcpy(sec + 16, hdr, 8);
      if (ata_write(0, 1, sec)) continue;
      total = nsect; found = 2;
    }
  if (!found) return 0;
  if (ata_read(1, DIRS, (u8 *)dir)) return 0;
  drive *d = drive_new(1);
  u32 files = 0, bytes = 0, bad = 0;
  for (u32 i = 0; i < ENTRIES; i++) {
    entry *e = &dir[i];
    if (!e->name[0]) continue;
    e->name[NAMEMAX - 1] = 0;
    if (e->start < DATA0 || e->start + e->nsect > total || e->len > e->nsect * 512) { bad++; continue; }
    blob *b = blob_new(e->nsect * 512);
    if (ata_read(e->start, e->nsect, b->bytes) || crc32(b->bytes, e->len) != e->crc) { blob_unref(b); bad++; continue; }
    b->len = e->len;
    drive_put(d, e->name, b);
    files++; bytes += e->len;
  }
  d->sink = store_sink;
  d->remove = store_remove;
  tally(d);
  /* "a new store on the primary slave, 1024 MB" or "the store on ...: 12 files, 3456 KB" */
  char *p = what, *end = what + whatlen - 1;
#define PUT(s) do { const char *q_ = (s); while (*q_ && p < end) *p++ = *q_++; } while (0)
#define NUM(v) do { char t_[12]; int k_ = 0; u32 x_ = (v); do t_[k_++] = (char)('0' + x_ % 10); while (x_ /= 10); while (k_ && p < end) *p++ = t_[--k_]; } while (0)
  PUT(found == 2 ? "a new store on the " : "the store on the ");
  PUT(io == 0x1F0 ? "primary " : "secondary "); PUT(slave ? "slave, " : "master, ");
  NUM(total / 2048); PUT(" MB");
  if (found == 1) { PUT(": "); NUM(files); PUT(" files, "); NUM(bytes >> 10); PUT(" KB"); }
  if (bad) { PUT(", "); NUM(bad); PUT(" unreadable left out"); }
  *p = 0;
#undef PUT
#undef NUM
  return d;
}
