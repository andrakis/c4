// blk.c -- the machine's disks, as sectors (docs/c4ix-ext2.md 1): the
// window of registers at 0x540 that libjs/blk.js and c4pc's dev.c keep
// (c4pc: the PC's IDE disks; both: --disk FILE.img). Kernel mode's alone:
// a user task's IOPM has no bit for it. ext2.c keeps its filesystem here.
//
// A command moves up to 256 sectors between a disk and memory (the
// kernel's, physical) and is done when the write that starts it returns.
//
// A device is a disk or a partition of one. Devices 0 to 7 are the disks
// (hd0 ... hd7), and 8 + 4 * D + P - 1 disk D's primary partition P, 1
// to 4 in its MBR (hdDpP: hd0p1 is 8, hd1p1 12). A partition's sectors
// are numbered from its own first, and nothing reaches past its last.
// The MBRs are read when a device is first asked about, and a disk's
// again after its sector 0 is written (blk_user: install.c partitions).

#include "c4ix.h"

enum {
    BLK_COUNT = 0x540, BLK_SEL = 0x544, BLK_SECTORS = 0x548, BLK_FLAGS = 0x54c,
    BLK_LBA = 0x550, BLK_NSECT = 0x554, BLK_ADDR = 0x558, BLK_CMD = 0x55c,
    BLK_RUN = 256, BLK_DISKS = 8
};

static int blk_lk[2];          // the registers: one command at a time, whichever CPU
static int blk_scanned;        // the MBRs read, of this many disks
static int blk_ndisks;
static int p_start[BLK_DISKS * 4];
static int p_len[BLK_DISKS * 4];      // 0: no such partition
static int p_type[BLK_DISKS * 4];

static int disk_count() {
    int n;
    // the machine asked again where the boot found no disks: c4pc offers
    // the window once a disk is handed over (its store's, DISK_EJECT)
    if (!host_has(C4IX_I_BLK) && !(__c4_info() & C4IX_I_BLK)) return 0;
    n = *(int *)BLK_COUNT;
    return n > BLK_DISKS ? BLK_DISKS : n;
}
static int disk_reg(int disk, int reg) {
    int v;
    klock_take(blk_lk);
    *(int *)BLK_SEL = disk;
    v = *(int *)reg;
    klock_drop(blk_lk);
    return v;
}
// N sectors at LBA of DISK, into BUF (CMD 1) or from it (CMD 2): 0, or -1
static int disk_io(int disk, int lba, int n, char *buf, int cmd) {
    int k, r;
    r = 0;
    while (n > 0 && !r) {
        k = n;
        if (k > BLK_RUN) k = BLK_RUN;
        klock_take(blk_lk);
        *(int *)BLK_SEL = disk;
        *(int *)BLK_LBA = lba;
        *(int *)BLK_NSECT = k;
        *(int *)BLK_ADDR = (int)buf;
        *(int *)BLK_CMD = cmd;
        r = *(int *)BLK_CMD;
        klock_drop(blk_lk);
        lba = lba + k;
        n = n - k;
        buf = buf + k * 512;
    }
    return r ? 0 - 1 : 0;
}

static int le32(char *p) { return (p[0] & 255) | (p[1] & 255) << 8 | (p[2] & 255) << 16 | (p[3] & 255) << 24; }

// disk D's partitions, from its MBR: the four primaries (an extended
// one's logical partitions are not read, nor a GPT's)
static void blk_scan(int d) {
    char *mbr, *e;
    int p, s, n, size, ty;
    p = 0;
    while (p < 4) { p_len[d * 4 + p] = 0; ++p; }
    size = disk_reg(d, BLK_SECTORS);
    if (!(mbr = malloc(512))) return;
    if (disk_io(d, 0, 1, mbr, 1) == 0 && (mbr[510] & 255) == 0x55 && (mbr[511] & 255) == 0xAA) {
        p = 0;
        while (p < 4) {
            e = mbr + 446 + p * 16;
            ty = e[4] & 255;
            s = le32(e + 8);
            n = le32(e + 12);
            // a type, not an extended one (5, 15) nor a GPT's guard (0xEE),
            // and inside the disk
            if (ty && ty != 5 && ty != 15 && ty != 0xEE && s > 0 && n > 0 && s + n > s && s + n <= size) {
                p_start[d * 4 + p] = s; p_len[d * 4 + p] = n; p_type[d * 4 + p] = ty;
            }
            ++p;
        }
    }
    free(mbr);
}
static void blk_scan_all() {
    int d, n;
    // again where the disks are more than when they were read: one handed
    // over (c4pc's store's, to install onto: DISK_EJECT)
    n = disk_count();
    if (blk_scanned && n == blk_ndisks) return;
    blk_scanned = 1;
    blk_ndisks = n;
    d = 0;
    while (d < n) { blk_scan(d); ++d; }
}

// a device's disk, and its partition's slot (-1: the whole disk); 0 if ID is a device
static int dev_split(int id, int *disk, int *slot) {
    if (id < 0) return 0 - 1;
    if (id < BLK_DISKS) { *disk = id; *slot = 0 - 1; if (id >= disk_count()) return 0 - 1; blk_scan_all(); return 0; }
    if (id >= BLK_DISKS + BLK_DISKS * 4) return 0 - 1;
    *disk = (id - BLK_DISKS) / 4;
    *slot = id - BLK_DISKS;
    if (*disk >= disk_count()) return 0 - 1;
    blk_scan_all();
    return p_len[*slot] > 0 ? 0 : 0 - 1;
}

int blk_count() { return disk_count(); }
int blk_part(int disk, int p) { return BLK_DISKS + disk * 4 + p - 1; }
int blk_valid(int id) { int d, s; return dev_split(id, &d, &s) == 0; }
int blk_disk(int id) { int d, s; return dev_split(id, &d, &s) == 0 ? d : 0 - 1; }
int blk_sectors(int id) {
    int d, s;
    if (dev_split(id, &d, &s) < 0) return 0;
    return s < 0 ? disk_reg(d, BLK_SECTORS) : p_len[s];
}
int blk_start(int id) {
    int d, s;
    if (dev_split(id, &d, &s) < 0 || s < 0) return 0;
    return p_start[s];
}
int blk_flags(int id) {
    int d, s;
    if (dev_split(id, &d, &s) < 0) return 0;
    return disk_reg(d, BLK_FLAGS);
}
int blk_type(int id) {
    int d, s;
    if (dev_split(id, &d, &s) < 0 || s < 0) return 0;
    return p_type[s];
}
// ID's name into OUT (8 bytes): hd0, hd0p1
char *blk_name(int id, char *out) {
    int d, p;
    if (id < BLK_DISKS) { d = id; p = 0; }
    else { d = (id - BLK_DISKS) / 4; p = (id - BLK_DISKS) % 4 + 1; }
    out[0] = 'h'; out[1] = 'd'; out[2] = '0' + d % 10;
    if (p) { out[3] = 'p'; out[4] = '0' + p; out[5] = 0; }
    else out[3] = 0;
    return out;
}
static int blk_io(int id, int lba, int n, char *buf, int cmd) {
    int d, s, len;
    if (dev_split(id, &d, &s) < 0 || n < 0 || lba < 0) return 0 - 1;
    if (s < 0) return disk_io(d, lba, n, buf, cmd);
    len = p_len[s];
    if (lba + n > len || lba + n < lba) return 0 - 1;    // a partition's writes never leave it
    return disk_io(d, p_start[s] + lba, n, buf, cmd);
}
int blk_read(int id, int lba, int n, char *buf) { return blk_io(id, lba, n, buf, 1); }
int blk_write(int id, int lba, int n, char *buf) { return blk_io(id, lba, n, buf, 2); }
int blk_flush(int id) {
    int r, d, s;
    if (dev_split(id, &d, &s) < 0) return 0 - 1;
    klock_take(blk_lk);
    *(int *)BLK_SEL = d;
    *(int *)BLK_CMD = 3;
    r = *(int *)BLK_CMD;
    klock_drop(blk_lk);
    return r;
}

// SYS_BLKIO: COUNT sectors at LBA of device ID to or from a user task's
// BUF (install.c: the MBR, the boot loader, the boot partition's files),
// through the kernel's own buffer, since a user task's address is not
// memory's. A whole disk's sector 0 written reads its partitions again,
// and is flushed to the disk at once; the rest when SYS_SYNC asks
// (vfs_sync: a disk's flush is an emulator's fdatasync, which after every
// 32 KB of install's 160 MB made it crawl). The caller checked BUF. 0, or -1
int blk_user(int id, int lba, int count, char *buf, int write) {
    char *k;
    int r, d;
    if (count < 1 || count > BLK_RUN) return 0 - 1;
    if (!(k = malloc(count * 512))) return 0 - 1;
    if (write) {
        memcpy(k, buf, count * 512);
        r = blk_write(id, lba, count, k);
        if (!r && id < BLK_DISKS && lba == 0) { blk_flush(id); blk_scan_all(); d = id; blk_scan(d); }
    } else {
        r = blk_read(id, lba, count, k);
        if (!r) memcpy(buf, k, count * 512);
    }
    free(k);
    return r;
}

// the boot's account: each disk, and its partitions, a line
void blk_banner() {
    char nm[8];
    int d, p, n, id;
    n = disk_count();
    d = 0;
    while (d < n) {
        klog("blk: %s: %d MB%s\n", blk_name(d, nm), blk_sectors(d) / 2048, (blk_flags(d) & 1) ? ", read-only" : "");
        p = 1;
        while (p <= 4) {
            id = blk_part(d, p);
            if (blk_valid(id)) klog("blk:   %s: %d MB, type 0x%x\n", blk_name(id, nm), blk_sectors(id) / 2048, blk_type(id));
            ++p;
        }
        ++d;
    }
}

// ID's name, in a buffer of its own (a log line's: the next call's too)
char *blk_nm(int id) {
    static char nm[8];
    return blk_name(id, nm);
}
// a device by its name, hdD or hdDpP: its number, or -1
int blk_parse(char *s) {
    int d, p;
    if (s[0] != 'h' || s[1] != 'd' || s[2] < '0' || s[2] > '7') return 0 - 1;
    d = s[2] - '0';
    if (!s[3]) return d;
    if (s[3] != 'p' || s[4] < '1' || s[4] > '4' || s[5]) return 0 - 1;
    p = s[4] - '0';
    return blk_part(d, p);
}
