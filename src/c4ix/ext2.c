// ext2.c -- the second extended filesystem, on a block device (blk.c):
// C4IX's own files on a disk, written as they change (docs/c4ix-ext2.md
// 2). Revision 1 as `mke2fs -t ext2` makes it: blocks of 1, 2 or 4 KB,
// block groups with their bitmaps and inode tables, direct, single,
// double and triple indirect blocks, linear directories (an htree's
// index is dropped where a directory changes, as Linux does on a kernel
// without it: its blocks are still a linear directory), fast and slow
// symlinks read as files. No journal (ext3), no extents (ext4): a disk
// with an incompatible feature is refused; with a read-only-compatible
// one this does not know, it mounts read-only.
//
// Every change goes to the disk as it is made: a block or inode marked
// used in its bitmap before anything points at it, a directory entry
// written after its inode. The group descriptors are written as their
// counts change; the superblock at sync and unmount, and marked not
// clean while mounted, so that a disk a machine stopped with gets
// e2fsck's look. Blocks of metadata (bitmaps, inode tables, directories,
// indirect blocks) go through a small cache, written through; a file's
// own blocks are read and written straight between the disk and the
// caller's buffer, a run of adjacent ones at a time.
//
// The VFS (vfs.c) keeps the tree: a mounted directory's vnodes carry
// their filesystem and inode number, and call here to list, read, write
// back, make and remove. Everything here runs under the VFS's lock.

#include "c4ix.h"

enum {
    E2_MAGIC = 0xEF53, E2_ROOT = 2, E2_NDIR = 12, E2_MAXFS = 4, E2_CACHE = 48,
    E2_INCOMPAT_FILETYPE = 2, E2_INCOMPAT_KNOWN = 2,
    E2_RO_SPARSE = 1, E2_RO_LARGE = 2, E2_RO_BTREE = 4, E2_RO_KNOWN = 7,
    E2_COMPAT_JOURNAL = 4,
    E2_INDEX_FL = 0x1000,
    S_IFMT = 0xF000, S_IFDIR = 0x4000, S_IFREG = 0x8000, S_IFLNK = 0xA000
};

struct e2fs {
    int dev, ro;
    int bsize, spb, ppb;       // block bytes, sectors a block, block numbers a block
    int blocks, inodes, first_data, bpg, ipg, ngroups, isize, first_ino;
    int gdt_blocks;            // blocks of group descriptors after the superblock
    int filetype;              // directory entries carry a file type
    int sparse;                // superblock backups only in groups 0, 1 and powers of 3, 5, 7
    int base_time;             // the clock: the superblock's last write, plus uptime
    char *sb;                  // the superblock, 1024 bytes
    char *gdt;                 // the group descriptors, 32 bytes each
    int sb_dirty;
    int used;
};
static struct e2fs e2_mounts[E2_MAXFS];

// the cache of metadata blocks: (fs, block) -> bytes, the oldest reused
static struct e2fs *c_fs[E2_CACHE];
static int c_blk[E2_CACHE];
static int c_age[E2_CACHE];
static char *c_data[E2_CACHE];
static int c_size[E2_CACHE];   // the bytes c_data holds: a block of the biggest size it was used for
static int c_clock;

static int rd16(char *p) { return (p[0] & 255) | (p[1] & 255) << 8; }
static int rd32(char *p) { return (p[0] & 255) | (p[1] & 255) << 8 | (p[2] & 255) << 16 | (p[3] & 255) << 24; }
static void wr16(char *p, int v) { p[0] = v; p[1] = v >> 8; }
static void wr32(char *p, int v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static int e2_now(struct e2fs *fs) { return fs->base_time + __time() / 1000; }
static int e2_cmp(char *a, char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return (*a & 255) - (*b & 255);
}

// ---- blocks ----

static int bread_to(struct e2fs *fs, int b, int n, char *buf) {
    return blk_read(fs->dev, b * fs->spb, n * fs->spb, buf);
}
static int bwrite_from(struct e2fs *fs, int b, int n, char *buf) {
    if (fs->ro) return 0 - 1;
    return blk_write(fs->dev, b * fs->spb, n * fs->spb, buf);
}
// block B's bytes, cached; 0 if it cannot be read
static char *bget(struct e2fs *fs, int b) {
    int i, old;
    ++c_clock;
    old = 0;
    i = 0;
    while (i < E2_CACHE) {
        if (c_fs[i] == fs && c_blk[i] == b) { c_age[i] = c_clock; return c_data[i]; }
        if (c_age[i] < c_age[old]) old = i;
        ++i;
    }
    // a buffer too small for this filesystem's blocks (one an unmounted
    // disk of smaller blocks left) is made again
    if (c_data[old] && c_size[old] < fs->bsize) { free(c_data[old]); c_data[old] = 0; }
    if (!c_data[old]) { if (!(c_data[old] = vfs_malloc(fs->bsize))) return 0; c_size[old] = fs->bsize; }
    c_fs[old] = 0;
    if (bread_to(fs, b, 1, c_data[old]) < 0) return 0;
    c_fs[old] = fs; c_blk[old] = b; c_age[old] = c_clock;
    return c_data[old];
}
// a cached block written back
static int bput(struct e2fs *fs, int b) {
    int i;
    i = 0;
    while (i < E2_CACHE) {
        if (c_fs[i] == fs && c_blk[i] == b) return bwrite_from(fs, b, 1, c_data[i]);
        ++i;
    }
    return 0 - 1;
}
// a block that is about to be overwritten whole: cached, zeroed, not read
static char *bzero_get(struct e2fs *fs, int b) {
    int i, old;
    ++c_clock;
    old = 0;
    i = 0;
    while (i < E2_CACHE) {
        if (c_fs[i] == fs && c_blk[i] == b) { old = i; break; }
        if (c_age[i] < c_age[old]) old = i;
        ++i;
    }
    // a buffer too small for this filesystem's blocks (one an unmounted
    // disk of smaller blocks left) is made again
    if (c_data[old] && c_size[old] < fs->bsize) { free(c_data[old]); c_data[old] = 0; }
    if (!c_data[old]) { if (!(c_data[old] = vfs_malloc(fs->bsize))) return 0; c_size[old] = fs->bsize; }
    memset(c_data[old], 0, fs->bsize);
    c_fs[old] = fs; c_blk[old] = b; c_age[old] = c_clock;
    return c_data[old];
}
static void cache_drop(struct e2fs *fs) {
    int i;
    i = 0;
    while (i < E2_CACHE) { if (c_fs[i] == fs) { c_fs[i] = 0; c_age[i] = 0; } ++i; }
}

// ---- the superblock and the group descriptors ----

static char *gd(struct e2fs *fs, int g) { return fs->gdt + g * 32; }
// group G's descriptor written: the block of the table it is in
static void gd_put(struct e2fs *fs, int g) {
    int b, per;
    per = fs->bsize / 32;
    b = g / per;
    bwrite_from(fs, fs->first_data + 1 + b, 1, fs->gdt + b * fs->bsize);
}
static int sb_write(struct e2fs *fs) {
    char *buf;
    int r;
    if (fs->ro) return 0;
    if (!(buf = vfs_malloc(1024))) return 0 - 1;
    memcpy(buf, fs->sb, 1024);
    wr32(buf + 48, e2_now(fs));                 // s_wtime
    r = blk_write(fs->dev, 2, 2, buf);
    free(buf);
    fs->sb_dirty = 0;
    return r;
}
static void counts(struct e2fs *fs, int g, int dblocks, int dinodes, int ddirs) {
    char *d;
    d = gd(fs, g);
    wr16(d + 12, rd16(d + 12) + dblocks);
    wr16(d + 14, rd16(d + 14) + dinodes);
    wr16(d + 16, rd16(d + 16) + ddirs);
    gd_put(fs, g);
    wr32(fs->sb + 12, rd32(fs->sb + 12) + dblocks);
    wr32(fs->sb + 16, rd32(fs->sb + 16) + dinodes);
    fs->sb_dirty = 1;
}

// ---- inodes ----

// inode INO into BUF (fs->isize bytes, at most 256): 0, or -1
static int iget(struct e2fs *fs, int ino, char *buf) {
    int g, i, off, b;
    char *p;
    if (ino < 1 || ino > fs->inodes) return 0 - 1;
    g = (ino - 1) / fs->ipg;
    i = (ino - 1) % fs->ipg;
    off = i * fs->isize;
    b = rd32(gd(fs, g) + 8) + off / fs->bsize;
    if (!(p = bget(fs, b))) return 0 - 1;
    memcpy(buf, p + off % fs->bsize, fs->isize < 256 ? fs->isize : 256);
    return 0;
}
static int iput(struct e2fs *fs, int ino, char *buf) {
    int g, i, off, b;
    char *p;
    if (ino < 1 || ino > fs->inodes) return 0 - 1;
    g = (ino - 1) / fs->ipg;
    i = (ino - 1) % fs->ipg;
    off = i * fs->isize;
    b = rd32(gd(fs, g) + 8) + off / fs->bsize;
    if (!(p = bget(fs, b))) return 0 - 1;
    memcpy(p + off % fs->bsize, buf, fs->isize < 256 ? fs->isize : 256);
    return bput(fs, b);
}
static int i_mode(char *ib) { return rd16(ib); }
static int i_size(char *ib) { return rd32(ib + 4); }
// a regular file's whole size: with C4IX_LFS its high word too (large_file);
// without, one of 2 GB or more is 2 GB less a byte
static c4off i_fsize(char *ib) {
#ifdef C4IX_LFS
    if ((i_mode(ib) & S_IFMT) == S_IFREG) return ((c4off)rd32(ib + 108) << 32) | ((c4off)rd32(ib + 4) & 0xFFFFFFFFll);
    return (c4off)rd32(ib + 4) & 0xFFFFFFFFll;
#else
    if (rd32(ib + 4) < 0 || ((i_mode(ib) & S_IFMT) == S_IFREG && rd32(ib + 108))) return 0x7FFFFFFF;
    return rd32(ib + 4);
#endif
}
static int i_links(char *ib) { return rd16(ib + 26); }

// ---- allocation ----

// a free block, near group GOAL; marked used. 0 when there is none
static int balloc(struct e2fs *fs, int goal) {
    int g, k, n, bit, byte, b0, nb;
    char *bm;
    k = 0;
    while (k < fs->ngroups) {
        g = (goal + k) % fs->ngroups;
        ++k;
        if (rd16(gd(fs, g) + 12) == 0) continue;
        b0 = rd32(gd(fs, g));
        if (!(bm = bget(fs, b0))) continue;
        nb = fs->blocks - fs->first_data - g * fs->bpg;
        if (nb > fs->bpg) nb = fs->bpg;
        n = 0;
        while (n < nb) {
            byte = bm[n >> 3] & 255;
            if (byte == 255) { n = (n | 7) + 1; continue; }
            bit = 1 << (n & 7);
            if (!(byte & bit)) {
                bm[n >> 3] = byte | bit;
                bput(fs, b0);
                counts(fs, g, 0 - 1, 0, 0);
                return fs->first_data + g * fs->bpg + n;
            }
            ++n;
        }
    }
    return 0;
}
static void bfree(struct e2fs *fs, int b) {
    int g, n, b0;
    char *bm;
    if (b < fs->first_data || b >= fs->blocks) return;
    g = (b - fs->first_data) / fs->bpg;
    n = (b - fs->first_data) % fs->bpg;
    b0 = rd32(gd(fs, g));
    if (!(bm = bget(fs, b0))) return;
    if (!(bm[n >> 3] & (1 << (n & 7)))) return;
    bm[n >> 3] = bm[n >> 3] & ~(1 << (n & 7));
    bput(fs, b0);
    counts(fs, g, 1, 0, 0);
}
// a free inode, in group GOAL or after it; marked used. 0 when there is none
static int ialloc(struct e2fs *fs, int goal, int isdir) {
    int g, k, n, b0, byte, bit;
    char *bm;
    k = 0;
    while (k < fs->ngroups) {
        g = (goal + k) % fs->ngroups;
        ++k;
        if (rd16(gd(fs, g) + 14) == 0) continue;
        b0 = rd32(gd(fs, g) + 4);
        if (!(bm = bget(fs, b0))) continue;
        n = 0;
        while (n < fs->ipg) {
            byte = bm[n >> 3] & 255;
            bit = 1 << (n & 7);
            if (!(byte & bit) && g * fs->ipg + n + 1 >= fs->first_ino) {
                bm[n >> 3] = byte | bit;
                bput(fs, b0);
                counts(fs, g, 0, 0 - 1, isdir);
                return g * fs->ipg + n + 1;
            }
            ++n;
        }
    }
    return 0;
}
static void ifree(struct e2fs *fs, int ino, int isdir) {
    int g, n, b0;
    char *bm;
    g = (ino - 1) / fs->ipg;
    n = (ino - 1) % fs->ipg;
    b0 = rd32(gd(fs, g) + 4);
    if (!(bm = bget(fs, b0))) return;
    bm[n >> 3] = bm[n >> 3] & ~(1 << (n & 7));
    bput(fs, b0);
    counts(fs, g, 0, 1, isdir ? 0 - 1 : 0);
}

// ---- a file's blocks ----

// the block an indirect block's slot I names, made where ALLOC and there
// is none (a new indirect block zeroed); 0 for none. IB: the inode, whose
// block count a made block adds to
static int slot(struct e2fs *fs, int blk, int i, int alloc, int goal, char *ib, int zero) {
    char *p;
    int b;
    if (!(p = bget(fs, blk))) return 0;
    b = rd32(p + i * 4);
    if (b || !alloc) return b;
    if (!(b = balloc(fs, goal))) return 0;
    if (zero) { if (!bzero_get(fs, b) || bput(fs, b) < 0) return 0; }
    if (!(p = bget(fs, blk))) return 0;
    wr32(p + i * 4, b);
    bput(fs, blk);
    wr32(ib + 28, rd32(ib + 28) + fs->spb);
    return b;
}
// file block LBN's place on the disk: made where ALLOC and there is none
// (the inode IB changed then: the caller writes it). 0 for a hole
static int bmap(struct e2fs *fs, int ino, char *ib, int lbn, int alloc) {
    int b, ppb, goal, top, idx;
    ppb = fs->ppb;
    goal = (ino - 1) / fs->ipg;
    if (lbn < E2_NDIR) {
        b = rd32(ib + 40 + lbn * 4);
        if (b || !alloc) return b;
        if (!(b = balloc(fs, goal))) return 0;
        wr32(ib + 40 + lbn * 4, b);
        wr32(ib + 28, rd32(ib + 28) + fs->spb);
        return b;
    }
    lbn = lbn - E2_NDIR;
    if (lbn < ppb) { top = 12; idx = 1; }
    else if (lbn - ppb < ppb * ppb) { lbn = lbn - ppb; top = 13; idx = 2; }
    else { lbn = lbn - ppb - ppb * ppb; top = 14; idx = 3; }
    b = rd32(ib + 40 + top * 4);
    if (!b) {
        if (!alloc) return 0;
        if (!(b = balloc(fs, goal))) return 0;
        if (!bzero_get(fs, b) || bput(fs, b) < 0) return 0;
        wr32(ib + 40 + top * 4, b);
        wr32(ib + 28, rd32(ib + 28) + fs->spb);
    }
    if (idx == 3) { b = slot(fs, b, lbn / (ppb * ppb), alloc, goal, ib, 1); if (!b) return 0; lbn = lbn % (ppb * ppb); }
    if (idx >= 2) { b = slot(fs, b, lbn / ppb, alloc, goal, ib, 1); if (!b) return 0; lbn = lbn % ppb; }
    return slot(fs, b, lbn, alloc, goal, ib, 0);
}
// every block below BLK (an indirect block of LEVEL, 1 single) whose file
// block is FROM or past, freed; BASE: the first file block it maps. 1 when
// BLK is now empty, and freed too
static int free_tree(struct e2fs *fs, int blk, int level, int base, int from, char *ib) {
    char *p;
    int i, b, span, empty, ppb;
    ppb = fs->ppb;
    span = 1;
    i = 1;
    while (i < level) { span = span * ppb; ++i; }
    empty = 1;
    i = 0;
    while (i < ppb) {
        if (!(p = bget(fs, blk))) return 0;
        b = rd32(p + i * 4);
        if (b) {
            if (base + (i + 1) * span <= from) empty = 0;     // wholly before: kept
            else if (level == 1 || free_tree(fs, b, level - 1, base + i * span, from, ib)) {
                if (level == 1 && base + i * span < from) empty = 0;
                else {
                    if (level == 1) { bfree(fs, b); wr32(ib + 28, rd32(ib + 28) - fs->spb); }
                    if (!(p = bget(fs, blk))) return 0;
                    wr32(p + i * 4, 0);
                    bput(fs, blk);          // now: a later bget may take the cache's copy
                }
            } else empty = 0;
        }
        ++i;
    }
    if (empty) { bfree(fs, blk); wr32(ib + 28, rd32(ib + 28) - fs->spb); }
    return empty;
}
// the inode's blocks from file block FROM on, freed
static void trunc_blocks(struct e2fs *fs, char *ib, int from) {
    int i, ppb, b;
    ppb = fs->ppb;
    i = from;
    while (i < E2_NDIR) {
        b = rd32(ib + 40 + i * 4);
        if (b) { bfree(fs, b); wr32(ib + 28, rd32(ib + 28) - fs->spb); wr32(ib + 40 + i * 4, 0); }
        ++i;
    }
    if ((b = rd32(ib + 88)) && free_tree(fs, b, 1, E2_NDIR, from, ib)) wr32(ib + 88, 0);
    if ((b = rd32(ib + 92)) && free_tree(fs, b, 2, E2_NDIR + ppb, from, ib)) wr32(ib + 92, 0);
    if ((b = rd32(ib + 96)) && free_tree(fs, b, 3, E2_NDIR + ppb + ppb * ppb, from, ib)) wr32(ib + 96, 0);
}

// ---- reading and writing a file's bytes ----

static int ext2_read_via(struct e2fs *fs, int ino, char *ib, char *buf, c4off pos, int len, char *blk);
// LEN bytes of inode INO from POS into BUF; the count, or -1. BUF may be a
// user task's (a read, under the MMU: its address is not memory's), so the
// disk is read into the kernel's own buffer, a run of up to 16 blocks at a
// time, and copied from there
int ext2_read(struct e2fs *fs, int ino, char *buf, c4off pos, int len) {
    char ib[256];
    char *blk;
    int k;
    if (iget(fs, ino, ib) < 0) return 0 - 1;
    if (!(blk = vfs_malloc(16 * fs->bsize))) return 0 - 1;
    k = ext2_read_via(fs, ino, ib, buf, pos, len, blk);
    free(blk);
    return k;
}
static int ext2_read_via(struct e2fs *fs, int ino, char *ib, char *buf, c4off pos, int len, char *blk) {
    c4off size;
    int done, lbn, off, n, b, run, b0;
    size = i_fsize(ib);
    if ((i_mode(ib) & S_IFMT) == S_IFLNK && size < 60 && !rd32(ib + 28)) {
        // a fast symlink: its target is in the block pointers
        if (pos >= size) return 0;
        if (len > size - pos) len = (int)(size - pos);
        memcpy(buf, ib + 40 + (int)pos, len);
        return len;
    }
    if (pos >= size) return 0;
    if (len > size - pos) len = (int)(size - pos);
    done = 0;
    while (done < len) {
        // the block and the offset in it (from a 64-bit position, with C4IX_LFS)
        lbn = (int)((pos + done) / fs->bsize);
        off = (int)((pos + done) % fs->bsize);
        b = bmap(fs, ino, ib, lbn, 0);
        n = fs->bsize - off;
        if (n > len - done) n = len - done;
        if (!b) { memset(buf + done, 0, n); done = done + n; continue; }
        if (off == 0 && n == fs->bsize) {
            // whole blocks: a run of adjacent ones, read at once
            run = 1;
            b0 = b;
            while (run < 16 && done + (run + 1) * fs->bsize <= len && bmap(fs, ino, ib, lbn + run, 0) == b0 + run) ++run;
            if (bread_to(fs, b0, run, blk) < 0) return 0 - 1;
            memcpy(buf + done, blk, run * fs->bsize);
            done = done + run * fs->bsize;
            continue;
        }
        if (bread_to(fs, b, 1, blk) < 0) return 0 - 1;
        memcpy(buf + done, blk + off, n);
        done = done + n;
    }
    return done;
}

// LEN bytes of BUF at POS of inode INO, on the disk, and nowhere else (a
// file too big for the RAM: vfs.c's write-through): its blocks made where
// there were none -- a part of one the rest zeros -- and its size grown to
// the end of them if that is past it. BUF may be a user task's (under the
// MMU, not memory's), so the disk is written from the kernel's own buffer,
// a run of up to 16 adjacent blocks at a time. The count, or -1.
int ext2_pwrite(struct e2fs *fs, int ino, char *buf, c4off pos, int len) {
    char ib[256];
    char *blk;
    c4off end;
    int done, lbn, off, n, b, run, b0, had;
    if (fs->ro || len < 0) return 0 - 1;
    if (iget(fs, ino, ib) < 0) return 0 - 1;
    if (!(blk = vfs_malloc(16 * fs->bsize))) return 0 - 1;
    done = 0;
    while (done < len) {
        lbn = (int)((pos + done) / fs->bsize);
        off = (int)((pos + done) % fs->bsize);
        n = fs->bsize - off;
        if (n > len - done) n = len - done;
        if (off == 0 && n == fs->bsize) {
            // whole blocks: a run of them, where the disk has them side by side
            if (!(b0 = bmap(fs, ino, ib, lbn, 1))) break;
            run = 1;
            while (run < 16 && done + (run + 1) * fs->bsize <= len && bmap(fs, ino, ib, lbn + run, 1) == b0 + run) ++run;
            memcpy(blk, buf + done, run * fs->bsize);
            if (bwrite_from(fs, b0, run, blk) < 0) break;
            done = done + run * fs->bsize;
            continue;
        }
        // part of a block: what it had, or zeros where it is new
        had = bmap(fs, ino, ib, lbn, 0);
        if (!(b = had ? had : bmap(fs, ino, ib, lbn, 1))) break;
        if (had) { if (bread_to(fs, b, 1, blk) < 0) break; }
        else memset(blk, 0, fs->bsize);
        memcpy(blk + off, buf + done, n);
        if (bwrite_from(fs, b, 1, blk) < 0) break;
        done = done + n;
    }
    free(blk);
    end = pos + done;
    if (end > i_fsize(ib)) {
#ifdef C4IX_LFS
        wr32(ib + 4, (int)(end & 0xFFFFFFFFll));
        wr32(ib + 108, (int)(end >> 32));
#else
        wr32(ib + 4, end);
        wr32(ib + 108, 0);
#endif
    }
    wr32(ib + 16, e2_now(fs));                  // mtime
    wr32(ib + 12, e2_now(fs));                  // ctime
    if (iput(fs, ino, ib) < 0) return 0 - 1;
    if (!done && len) return 0 - 1;             // full, or the disk refused
    return done;
}

// inode INO's bytes made BUF's LEN: its blocks written (made, where it
// grew), its blocks past the end freed, its size and mtime set. 0, or -1
int ext2_write(struct e2fs *fs, int ino, char *buf, int len) {
    char ib[256];
    char *tail;
    int nb, lbn, b, run, b0, rest;
    if (fs->ro) return 0 - 1;
    if (iget(fs, ino, ib) < 0) return 0 - 1;
    nb = (len + fs->bsize - 1) / fs->bsize;
    lbn = 0;
    while (lbn < nb) {
        if (!(b = bmap(fs, ino, ib, lbn, 1))) { iput(fs, ino, ib); return 0 - 1; }
        rest = len - lbn * fs->bsize;
        if (rest >= fs->bsize) {
            run = 1;
            b0 = b;
            while (run < 64 && lbn + run < nb && (len - (lbn + run) * fs->bsize) >= fs->bsize) {
                if (bmap(fs, ino, ib, lbn + run, 1) != b0 + run) break;
                ++run;
            }
            if (bwrite_from(fs, b0, run, buf + lbn * fs->bsize) < 0) { iput(fs, ino, ib); return 0 - 1; }
            lbn = lbn + run;
            continue;
        }
        // the last, part of a block: the rest of it zeros
        if (!(tail = vfs_malloc(fs->bsize))) { iput(fs, ino, ib); return 0 - 1; }
        memset(tail, 0, fs->bsize);
        memcpy(tail, buf + lbn * fs->bsize, rest);
        b = bwrite_from(fs, b, 1, tail);
        free(tail);
        if (b < 0) { iput(fs, ino, ib); return 0 - 1; }
        ++lbn;
    }
    trunc_blocks(fs, ib, nb);
    wr32(ib + 4, len);
    wr32(ib + 108, 0);                          // the size's high word (large_file)
    wr32(ib + 16, e2_now(fs));                  // mtime
    wr32(ib + 12, e2_now(fs));                  // ctime
    return iput(fs, ino, ib);
}

// ---- directories ----

// the next entry of directory DIR from *POS (bytes into it): its name,
// inode and whether it is a directory; 1, or 0 at the end. . and .. too
int ext2_dir_next(struct e2fs *fs, int dir, int *pos, char *name, int *ino, int *isdir) {
    char ib[256], cb[256];
    char *p;
    int size, lbn, off, b, rl, nl, e, t;
    if (iget(fs, dir, ib) < 0) return 0;
    size = i_size(ib);
    while (*pos < size) {
        lbn = *pos / fs->bsize;
        off = *pos % fs->bsize;
        if (!(b = bmap(fs, dir, ib, lbn, 0)) || !(p = bget(fs, b))) { *pos = (lbn + 1) * fs->bsize; continue; }
        e = rd32(p + off);
        rl = rd16(p + off + 4);
        nl = p[off + 6] & 255;
        t = p[off + 7] & 255;
        if (rl < 8 || off + rl > fs->bsize) { *pos = (lbn + 1) * fs->bsize; continue; }
        *pos = *pos + rl;
        if (!e || nl == 0) continue;
        if (nl > VN_NAME_MAX - 1) nl = VN_NAME_MAX - 1;
        memcpy(name, p + off + 8, nl);
        name[nl] = 0;
        *ino = e;
        if (fs->filetype && t) *isdir = t == 2;
        else { if (iget(fs, e, cb) < 0) *isdir = 0; else *isdir = (i_mode(cb) & S_IFMT) == S_IFDIR; }
        return 1;
    }
    return 0;
}

static int name_len(char *s) { int n; n = 0; while (s[n]) ++n; return n; }
static int same(char *p, int nl, char *name) {
    int i;
    if (name_len(name) != nl) return 0;
    i = 0;
    while (i < nl) { if (p[i] != name[i]) return 0; ++i; }
    return 1;
}
// NAME in directory DIR: its inode, or 0
int ext2_lookup(struct e2fs *fs, int dir, char *name) {
    char n[VN_NAME_MAX];
    int pos, ino, isdir;
    pos = 0;
    while (ext2_dir_next(fs, dir, &pos, n, &ino, &isdir)) if (!e2_cmp(n, name)) return ino;
    return 0;
}

// an htree's index dropped: the directory is linear from now (its blocks
// already are; e2fsck -D can index it again)
static void unindex(char *ib) {
    int f;
    f = rd32(ib + 32);
    if (f & E2_INDEX_FL) wr32(ib + 32, f & ~E2_INDEX_FL);
}
// an entry NAME -> INO (of type FT) added to directory DIR: in a block with
// room, else in a new one at its end. 0, or -1
static int dir_add(struct e2fs *fs, int dir, char *name, int ino, int ft) {
    char ib[256];
    char *p;
    int size, lbn, off, b, rl, nl, need, have, e, nb;
    nl = name_len(name);
    if (nl < 1 || nl > 255) return 0 - 1;
    need = (8 + nl + 3) & ~3;
    if (iget(fs, dir, ib) < 0) return 0 - 1;
    unindex(ib);
    size = i_size(ib);
    lbn = 0;
    while (lbn * fs->bsize < size) {
        if ((b = bmap(fs, dir, ib, lbn, 0)) && (p = bget(fs, b))) {
            off = 0;
            while (off < fs->bsize) {
                e = rd32(p + off);
                rl = rd16(p + off + 4);
                if (rl < 8 || off + rl > fs->bsize) break;
                have = e ? (8 + (p[off + 6] & 255) + 3) & ~3 : 0;
                if (rl - have >= need) {
                    if (have) { wr16(p + off + 4, have); off = off + have; rl = rl - have; }
                    wr32(p + off, ino);
                    wr16(p + off + 4, rl);
                    p[off + 6] = nl;
                    p[off + 7] = fs->filetype ? ft : 0;
                    memcpy(p + off + 8, name, nl);
                    bput(fs, b);
                    wr32(ib + 16, e2_now(fs)); wr32(ib + 12, e2_now(fs));
                    return iput(fs, dir, ib);
                }
                off = off + rl;
            }
        }
        ++lbn;
    }
    // a new block
    if (!(b = bmap(fs, dir, ib, lbn, 1))) return 0 - 1;
    if (!(p = bzero_get(fs, b))) return 0 - 1;
    wr32(p, ino);
    wr16(p + 4, fs->bsize);
    p[6] = nl;
    p[7] = fs->filetype ? ft : 0;
    memcpy(p + 8, name, nl);
    if (bput(fs, b) < 0) return 0 - 1;
    nb = (lbn + 1) * fs->bsize;
    wr32(ib + 4, nb);
    wr32(ib + 16, e2_now(fs)); wr32(ib + 12, e2_now(fs));
    return iput(fs, dir, ib);
}
// NAME's entry in directory DIR removed: its inode, or 0
static int dir_remove(struct e2fs *fs, int dir, char *name) {
    char ib[256];
    char *p;
    int size, lbn, off, prev, b, rl, e, nl;
    if (iget(fs, dir, ib) < 0) return 0;
    size = i_size(ib);
    lbn = 0;
    while (lbn * fs->bsize < size) {
        if ((b = bmap(fs, dir, ib, lbn, 0)) && (p = bget(fs, b))) {
            off = 0; prev = 0 - 1;
            while (off < fs->bsize) {
                e = rd32(p + off);
                rl = rd16(p + off + 4);
                nl = p[off + 6] & 255;
                if (rl < 8 || off + rl > fs->bsize) break;
                if (e && same(p + off + 8, nl, name)) {
                    if (prev >= 0) wr16(p + prev + 4, rd16(p + prev + 4) + rl);
                    else wr32(p + off, 0);
                    bput(fs, b);
                    unindex(ib);
                    wr32(ib + 16, e2_now(fs)); wr32(ib + 12, e2_now(fs));
                    iput(fs, dir, ib);
                    return e;
                }
                prev = off;
                off = off + rl;
            }
        }
        ++lbn;
    }
    return 0;
}
// does directory DIR hold anything but . and ..
static int dir_empty(struct e2fs *fs, int dir) {
    char n[VN_NAME_MAX];
    int pos, ino, isdir;
    pos = 0;
    while (ext2_dir_next(fs, dir, &pos, n, &ino, &isdir))
        if (e2_cmp(n, ".") && e2_cmp(n, "..")) return 0;
    return 1;
}
// a directory's ".." made DIR
static void set_dotdot(struct e2fs *fs, int ino, int dir) {
    char ib[256];
    char *p;
    int b, off, rl;
    if (iget(fs, ino, ib) < 0 || !(b = bmap(fs, ino, ib, 0, 0)) || !(p = bget(fs, b))) return;
    off = 0;
    while (off < fs->bsize) {
        rl = rd16(p + off + 4);
        if (rl < 8) return;
        if ((p[off + 6] & 255) == 2 && p[off + 8] == '.' && p[off + 9] == '.') { wr32(p + off, dir); bput(fs, b); return; }
        off = off + rl;
    }
}
static int links_add(struct e2fs *fs, int ino, int d) {
    char ib[256];
    if (iget(fs, ino, ib) < 0) return 0 - 1;
    wr16(ib + 26, i_links(ib) + d);
    wr32(ib + 12, e2_now(fs));
    return iput(fs, ino, ib);
}

// ---- making and removing ----

static void inode_init(struct e2fs *fs, char *ib, int mode, int links) {
    int t;
    memset(ib, 0, 256);
    t = e2_now(fs);
    wr16(ib, mode);
    wr32(ib + 8, t); wr32(ib + 12, t); wr32(ib + 16, t);
    wr16(ib + 26, links);
    if (fs->isize > 128) wr16(ib + 128, 32);    // i_extra_isize: the fields ext4 adds, there and zero
}
// a file NAME made in DIR, with MODE's permission bits: its inode, or 0
int ext2_create(struct e2fs *fs, int dir, char *name, int mode) {
    char ib[256];
    int ino;
    if (fs->ro || ext2_lookup(fs, dir, name)) return 0;
    if (!(ino = ialloc(fs, (dir - 1) / fs->ipg, 0))) return 0;
    inode_init(fs, ib, S_IFREG | (mode & 4095), 1);
    if (iput(fs, ino, ib) < 0 || dir_add(fs, dir, name, ino, 1) < 0) { ifree(fs, ino, 0); return 0; }
    return ino;
}
// a directory NAME made in DIR: its inode, or 0
int ext2_mkdir(struct e2fs *fs, int dir, char *name, int mode) {
    char ib[256];
    char *p;
    int ino, b, g;
    if (fs->ro || ext2_lookup(fs, dir, name)) return 0;
    // a group of its own, the one with the most inodes free: directories spread
    g = 0; b = 0;
    while (b < fs->ngroups) { if (rd16(gd(fs, b) + 14) > rd16(gd(fs, g) + 14)) g = b; ++b; }
    if (!(ino = ialloc(fs, g, 1))) return 0;
    inode_init(fs, ib, S_IFDIR | (mode & 4095), 2);
    if (!(b = bmap(fs, ino, ib, 0, 1)) || !(p = bzero_get(fs, b))) { ifree(fs, ino, 1); return 0; }
    wr32(p, ino); wr16(p + 4, 12); p[6] = 1; p[7] = fs->filetype ? 2 : 0; p[8] = '.';
    wr32(p + 12, dir); wr16(p + 16, fs->bsize - 12); p[18] = 2; p[19] = fs->filetype ? 2 : 0; p[20] = '.'; p[21] = '.';
    bput(fs, b);
    wr32(ib + 4, fs->bsize);
    if (iput(fs, ino, ib) < 0 || dir_add(fs, dir, name, ino, 2) < 0) return 0;
    links_add(fs, dir, 1);
    return ino;
}
// an inode whose last link went: its blocks and itself freed
static void inode_drop(struct e2fs *fs, int ino) {
    char ib[256];
    int isdir;
    if (iget(fs, ino, ib) < 0) return;
    isdir = (i_mode(ib) & S_IFMT) == S_IFDIR;
    if (!((i_mode(ib) & S_IFMT) == S_IFLNK && i_size(ib) < 60 && !rd32(ib + 28))) trunc_blocks(fs, ib, 0);
    wr16(ib + 26, 0);
    wr32(ib + 20, e2_now(fs));                  // dtime
    iput(fs, ino, ib);
    ifree(fs, ino, isdir);
}
// NAME removed from DIR: a file's link, or an empty directory. 0, or -1
int ext2_unlink(struct e2fs *fs, int dir, char *name) {
    char ib[256];
    int ino, isdir;
    if (fs->ro || !e2_cmp(name, ".") || !e2_cmp(name, "..")) return 0 - 1;
    if (!(ino = ext2_lookup(fs, dir, name)) || iget(fs, ino, ib) < 0) return 0 - 1;
    isdir = (i_mode(ib) & S_IFMT) == S_IFDIR;
    if (isdir && !dir_empty(fs, ino)) return 0 - 1;
    if (!dir_remove(fs, dir, name)) return 0 - 1;
    if (isdir) { inode_drop(fs, ino); links_add(fs, dir, 0 - 1); return 0; }
    if (i_links(ib) <= 1) inode_drop(fs, ino);
    else links_add(fs, ino, 0 - 1);
    return 0;
}
// a second name NAME in DIR for inode INO (not a directory). 0, or -1
int ext2_link(struct e2fs *fs, int dir, char *name, int ino) {
    if (fs->ro || ext2_lookup(fs, dir, name)) return 0 - 1;
    if (dir_add(fs, dir, name, ino, 1) < 0) return 0 - 1;
    return links_add(fs, ino, 1);
}
// OLDNAME in OLDDIR named NEWNAME in NEWDIR; one there already (a file,
// or an empty directory) goes. 0, or -1
int ext2_rename(struct e2fs *fs, int olddir, char *oldname, int newdir, char *newname) {
    char ib[256];
    int ino, isdir, t;
    if (fs->ro || !(ino = ext2_lookup(fs, olddir, oldname)) || iget(fs, ino, ib) < 0) return 0 - 1;
    isdir = (i_mode(ib) & S_IFMT) == S_IFDIR;
    if ((t = ext2_lookup(fs, newdir, newname))) {
        if (t == ino) return 0;
        if (ext2_unlink(fs, newdir, newname) < 0) return 0 - 1;
    }
    if (dir_add(fs, newdir, newname, ino, isdir ? 2 : 1) < 0) return 0 - 1;
    dir_remove(fs, olddir, oldname);
    if (isdir && olddir != newdir) {
        set_dotdot(fs, ino, newdir);
        links_add(fs, olddir, 0 - 1);
        links_add(fs, newdir, 1);
    }
    return 0;
}
// inode INO's permission bits made MODE's
int ext2_chmod(struct e2fs *fs, int ino, int mode) {
    char ib[256];
    if (fs->ro || iget(fs, ino, ib) < 0) return 0 - 1;
    wr16(ib, (i_mode(ib) & S_IFMT) | (mode & 4095));
    wr32(ib + 12, e2_now(fs));
    return iput(fs, ino, ib);
}
// what stat says of inode INO: out[0] mode, [1] size, [2] links, [3] uid,
// [4] gid, [5] mtime. 0, or -1
// a file's whole size (i_fsize): -1 if its inode cannot be read
c4off ext2_size(struct e2fs *fs, int ino) {
    char ib[256];
    if (iget(fs, ino, ib) < 0) return 0 - 1;
    return i_fsize(ib);
}
int ext2_stat(struct e2fs *fs, int ino, int *out) {
    char ib[256];
    if (iget(fs, ino, ib) < 0) return 0 - 1;
    out[0] = i_mode(ib); out[1] = C4OFF_WORD(i_fsize(ib)); out[2] = i_links(ib);
    out[3] = rd16(ib + 2); out[4] = rd16(ib + 24); out[5] = rd32(ib + 16);
    return 0;
}
// the filesystem's counts: out[0] block size, [1] blocks, [2] free, [3]
// inodes, [4] free, [5] read-only
void ext2_statfs(struct e2fs *fs, int *out) {
    out[0] = fs->bsize; out[1] = fs->blocks; out[2] = rd32(fs->sb + 12);
    out[3] = fs->inodes; out[4] = rd32(fs->sb + 16); out[5] = fs->ro;
}
char *ext2_label(struct e2fs *fs) { return fs->sb + 120; }
int ext2_dev(struct e2fs *fs) { return fs->dev; }
int ext2_ro(struct e2fs *fs) { return fs->ro; }

// ---- mounting ----

// DEV's ext2 filesystem, mounted (read-only where RO, or where it has a
// feature that wants it): the filesystem, or 0 with why in the log
// do devices A and B share sectors: the same one, or a disk and a partition of it
static int e2_overlap(int a, int b) {
    if (a == b) return 1;
    if (a < 8 && blk_disk(b) == a) return 1;
    if (b < 8 && blk_disk(a) == b) return 1;
    return 0;
}

struct e2fs *ext2_mount(int dev, int ro) {
    struct e2fs *fs;
    char *sb;
    int i, inc, roc, n;
    i = 0;
    while (i < E2_MAXFS) {
        if (e2_mounts[i].used && e2_overlap(e2_mounts[i].dev, dev)) { klog("ext2: %s: mounted already, or a disk or partition it is on\n", blk_nm(dev)); return 0; }
        ++i;
    }
    i = 0;
    while (i < E2_MAXFS && e2_mounts[i].used) ++i;
    if (i == E2_MAXFS) { klog("ext2: %s: no room for another mount\n", blk_nm(dev)); return 0; }
    fs = &e2_mounts[i];
    memset((char *)fs, 0, sizeof(struct e2fs));
    if (!(sb = vfs_malloc(1024))) return 0;
    if (blk_read(dev, 2, 2, sb) < 0 || rd16(sb + 56) != E2_MAGIC) {
        klog("ext2: %s: no ext2 filesystem there\n", blk_nm(dev));
        free(sb);
        return 0;
    }
    fs->dev = dev;
    fs->sb = sb;
    fs->ro = ro || (blk_flags(dev) & 1);
    inc = rd32(sb + 96);
    roc = rd32(sb + 100);
    if (rd32(sb + 76) >= 1 && (inc & ~E2_INCOMPAT_KNOWN)) {
        klog("ext2: %s: features this cannot read (incompat 0x%x: ext4's, or a journal to recover)\n", blk_nm(dev), inc);
        free(sb);
        return 0;
    }
    if (rd32(sb + 76) >= 1 && (roc & ~E2_RO_KNOWN) && !fs->ro) {
        klog("ext2: %s: read-only: features this cannot write (ro_compat 0x%x)\n", blk_nm(dev), roc);
        fs->ro = 1;
    }
    if ((rd32(sb + 92) & E2_COMPAT_JOURNAL) && !fs->ro) {
        klog("ext2: %s: read-only: it has a journal (ext3), which this does not keep\n", blk_nm(dev));
        fs->ro = 1;
    }
    fs->bsize = 1024 << rd32(sb + 24);
    if (fs->bsize < 1024 || fs->bsize > 4096) { klog("ext2: %s: blocks of %d bytes\n", blk_nm(dev), fs->bsize); free(sb); return 0; }
    fs->spb = fs->bsize / 512;
    fs->ppb = fs->bsize / 4;
    fs->inodes = rd32(sb);
    fs->blocks = rd32(sb + 4);
    fs->first_data = rd32(sb + 20);
    fs->bpg = rd32(sb + 32);
    fs->ipg = rd32(sb + 40);
    fs->isize = 128; fs->first_ino = 11;
    if (rd32(sb + 76) >= 1) { fs->isize = rd16(sb + 88); fs->first_ino = rd32(sb + 84); }
    fs->filetype = (inc & E2_INCOMPAT_FILETYPE) != 0;
    fs->sparse = (roc & E2_RO_SPARSE) != 0;
    if (fs->bpg <= 0 || fs->ipg <= 0 || fs->isize < 128 || fs->isize > fs->bsize) { klog("ext2: %s: a superblock that does not add up\n", blk_nm(dev)); free(sb); return 0; }
    fs->ngroups = (fs->blocks - fs->first_data + fs->bpg - 1) / fs->bpg;
    n = (fs->ngroups * 32 + fs->bsize - 1) / fs->bsize;
    fs->gdt_blocks = n;
    if (!(fs->gdt = vfs_malloc(n * fs->bsize)) || bread_to(fs, fs->first_data + 1, n, fs->gdt) < 0) {
        klog("ext2: %s: its group descriptors cannot be read\n", blk_nm(dev));
        free(sb);
        return 0;
    }
    fs->base_time = rd32(sb + 48);
    if (rd32(sb + 44) > fs->base_time) fs->base_time = rd32(sb + 44);
    fs->base_time = fs->base_time - __time() / 1000;
    if (!(rd16(sb + 58) & 1)) klog("ext2: %s: not unmounted cleanly last time: e2fsck it when you can\n", blk_nm(dev));
    fs->used = 1;
    if (!fs->ro) {
        // mounted: not clean until it is unmounted, and counted
        wr16(sb + 58, rd16(sb + 58) & ~1);
        wr16(sb + 52, rd16(sb + 52) + 1);
        wr32(sb + 44, e2_now(fs));
        sb_write(fs);
        blk_flush(dev);
    }
    return fs;
}
// the superblock's counts written, where they changed: after each
// operation, so a machine stopped between two leaves them right
int ext2_commit(struct e2fs *fs) {
    if (fs->ro || !fs->sb_dirty) return 0;
    return sb_write(fs);
}
int ext2_sync(struct e2fs *fs) {
    if (fs->ro) return 0;
    if (fs->sb_dirty) sb_write(fs);
    return blk_flush(fs->dev);
}
int ext2_umount(struct e2fs *fs) {
    if (!fs->ro) {
        wr16(fs->sb + 58, rd16(fs->sb + 58) | 1);   // clean
        sb_write(fs);
        blk_flush(fs->dev);
    }
    cache_drop(fs);
    free(fs->sb); free(fs->gdt);
    fs->used = 0;
    return 0;
}

// ---- making one: mkfs ----

// does group G hold a copy of the superblock (sparse_super: 0, 1, and the
// powers of 3, 5 and 7)
static int has_super(int g) {
    int p;
    if (g < 2) return 1;
    p = 3; while (p < g) p = p * 3; if (p == g) return 1;
    p = 5; while (p < g) p = p * 5; if (p == g) return 1;
    p = 7; while (p < g) p = p * 7; if (p == g) return 1;
    return 0;
}
static void bit_set(char *bm, int n) { bm[n >> 3] = bm[n >> 3] | (1 << (n & 7)); }

// DEV made an empty ext2 filesystem of BSIZE-byte blocks (1024, 2048 or
// 4096; 0: 1024 under 512 MB, else 4096), named LABEL: a root directory
// and lost+found. 0, or -1 with why in the log
int ext2_mkfs(int dev, int bsize, char *label) {
    char *sb, *gdt, *bm, *zero, *ib, *p;
    int sectors, blocks, spb, first, bpg, ngroups, ipg, ipb, itb, gdtb, g, base, nb, used, k, i, t;
    int free_b, free_i, rootb, lfb, inodes, ok, rsv;
    sectors = blk_sectors(dev);
    if (sectors <= 0 || (blk_flags(dev) & 1)) { klog("ext2: %s: not a disk that can be written\n", blk_nm(dev)); return 0 - 1; }
    i = 0;
    while (i < E2_MAXFS) {
        if (e2_mounts[i].used && e2_overlap(e2_mounts[i].dev, dev)) { klog("ext2: %s: mounted, or a partition of it: unmount it first\n", blk_nm(dev)); return 0 - 1; }
        ++i;
    }
    if (!bsize) bsize = sectors < 1048576 ? 1024 : 4096;
    if (bsize != 1024 && bsize != 2048 && bsize != 4096) return 0 - 1;
    spb = bsize / 512;
    blocks = sectors / spb;
    first = bsize == 1024 ? 1 : 0;
    bpg = bsize * 8;
    ngroups = (blocks - first + bpg - 1) / bpg;
    ipb = bsize / 128;
    // an inode for every 8 KB, at least 16 a group, at most a bitmap's worth
    inodes = (blocks / 8) * (bsize / 1024);
    if (bsize == 1024) inodes = blocks / 8;
    ipg = (inodes + ngroups - 1) / ngroups;
    ipg = (ipg + ipb - 1) / ipb * ipb;
    if (ipg < 16) ipg = 16;
    if (ipg > bsize * 8) ipg = bsize * 8;
    itb = ipg / ipb;
    gdtb = (ngroups * 32 + bsize - 1) / bsize;
    // a last group too small for its own metadata is left off
    nb = blocks - first - (ngroups - 1) * bpg;
    if (ngroups > 1 && nb < (has_super(ngroups - 1) ? 1 + gdtb : 0) + 2 + itb + 16) {
        --ngroups;
        blocks = first + ngroups * bpg;
    }
    if (blocks < 64) { klog("ext2: %s: too small\n", blk_nm(dev)); return 0 - 1; }
    if (!(sb = vfs_malloc(1024)) || !(gdt = vfs_malloc(gdtb * bsize)) || !(bm = vfs_malloc(bsize)) ||
        !(zero = vfs_malloc(bsize * 16)) || !(ib = vfs_malloc(256))) return 0 - 1;
    memset(sb, 0, 1024); memset(gdt, 0, gdtb * bsize); memset(zero, 0, bsize * 16);
    t = __time() / 1000 + 1700000000;           // no calendar here: a plausible one, the same each boot
    free_b = 0; free_i = 0;
    rootb = 0; lfb = 0;
    ok = 1;
    g = 0;
    while (g < ngroups && ok) {
        base = first + g * bpg;
        nb = blocks - base;
        if (nb > bpg) nb = bpg;
        used = 0;
        if (has_super(g)) used = 1 + gdtb;
        p = gdt + g * 32;
        wr32(p, base + used);                   // block bitmap
        wr32(p + 4, base + used + 1);           // inode bitmap
        wr32(p + 8, base + used + 2);           // inode table
        used = used + 2 + itb;
        if (g == 0) { rootb = base + used; lfb = base + used + 1; used = used + 2; }
        // the block bitmap: the metadata, and past the group's end
        memset(bm, 0, bsize);
        i = 0; while (i < used) { bit_set(bm, i); ++i; }
        i = nb; while (i < bpg) { bit_set(bm, i); ++i; }
        if (blk_write(dev, (base + (has_super(g) ? 1 + gdtb : 0)) * spb, spb, bm) < 0) ok = 0;
        // the inode bitmap: the reserved ones (1-10) and lost+found in group 0
        memset(bm, 0, bsize);
        if (g == 0) { i = 0; while (i < 11) { bit_set(bm, i); ++i; } }
        i = ipg; while (i < bsize * 8) { bit_set(bm, i); ++i; }
        if (blk_write(dev, (base + (has_super(g) ? 1 + gdtb : 0) + 1) * spb, spb, bm) < 0) ok = 0;
        // the inode table, zeroed
        k = 0;
        while (k < itb && ok) {
            i = itb - k; if (i > 16) i = 16;
            if (blk_write(dev, (rd32(p + 8) + k) * spb, i * spb, zero) < 0) ok = 0;
            k = k + i;
        }
        wr16(p + 12, nb - used);
        wr16(p + 14, g == 0 ? ipg - 11 : ipg);
        wr16(p + 16, g == 0 ? 2 : 0);
        free_b = free_b + nb - used;
        free_i = free_i + (g == 0 ? ipg - 11 : ipg);
        ++g;
    }
    // the superblock
    rsv = blocks / 20;
    wr32(sb, ipg * ngroups); wr32(sb + 4, blocks); wr32(sb + 8, rsv);
    wr32(sb + 12, free_b); wr32(sb + 16, free_i); wr32(sb + 20, first);
    k = 0; i = bsize; while (i > 1024) { i = i / 2; ++k; }
    wr32(sb + 24, k); wr32(sb + 28, k);
    wr32(sb + 32, bpg); wr32(sb + 36, bpg); wr32(sb + 40, ipg);
    wr32(sb + 44, 0); wr32(sb + 48, t);
    wr16(sb + 52, 0); wr16(sb + 54, 0xFFFF);
    wr16(sb + 56, E2_MAGIC); wr16(sb + 58, 1); wr16(sb + 60, 1);
    wr32(sb + 64, t); wr32(sb + 76, 1);
    wr32(sb + 84, 11); wr16(sb + 88, 128);
    wr32(sb + 96, E2_INCOMPAT_FILETYPE); wr32(sb + 100, E2_RO_SPARSE | E2_RO_LARGE);
    i = 0; while (i < 16) { sb[104 + i] = (t >> (i & 3) * 8) ^ (i * 37 + dev * 101); ++i; }   // the uuid
    sb[104 + 6] = (sb[104 + 6] & 15) | 64; sb[104 + 8] = (sb[104 + 8] & 63) | 128;
    if (label) { i = 0; while (i < 16 && label[i]) { sb[120 + i] = label[i]; ++i; } }
    // the root and lost+found: inode 2 and 11, a block each
    memset(ib, 0, 256);
    memset(bm, 0, bsize);
    wr32(bm, 2); wr16(bm + 4, 12); bm[6] = 1; bm[7] = 2; bm[8] = '.';
    wr32(bm + 12, 2); wr16(bm + 16, 12); bm[18] = 2; bm[19] = 2; bm[20] = '.'; bm[21] = '.';
    wr32(bm + 24, 11); wr16(bm + 28, bsize - 24); bm[30] = 10; bm[31] = 2; memcpy(bm + 32, "lost+found", 10);
    if (blk_write(dev, rootb * spb, spb, bm) < 0) ok = 0;
    memset(bm, 0, bsize);
    wr32(bm, 11); wr16(bm + 4, 12); bm[6] = 1; bm[7] = 2; bm[8] = '.';
    wr32(bm + 12, 2); wr16(bm + 16, bsize - 12); bm[18] = 2; bm[19] = 2; bm[20] = '.'; bm[21] = '.';
    if (blk_write(dev, lfb * spb, spb, bm) < 0) ok = 0;
    // the inode table's first block, with them in it
    memset(zero, 0, bsize);
    p = zero + (2 - 1) * 128;
    wr16(p, S_IFDIR | 493); wr32(p + 4, bsize); wr32(p + 8, t); wr32(p + 12, t); wr32(p + 16, t);
    wr16(p + 26, 3); wr32(p + 28, spb); wr32(p + 40, rootb);
    if (11 <= ipb) {
        p = zero + (11 - 1) * 128;
        wr16(p, S_IFDIR | 448); wr32(p + 4, bsize); wr32(p + 8, t); wr32(p + 12, t); wr32(p + 16, t);
        wr16(p + 26, 2); wr32(p + 28, spb); wr32(p + 40, lfb);
        if (blk_write(dev, rd32(gdt + 8) * spb, spb, zero) < 0) ok = 0;
    } else {
        // 1 KB blocks hold 8 inodes: 11 is in the table's second block
        if (blk_write(dev, rd32(gdt + 8) * spb, spb, zero) < 0) ok = 0;
        memset(zero, 0, bsize);
        p = zero + ((11 - 1) % ipb) * 128;
        wr16(p, S_IFDIR | 448); wr32(p + 4, bsize); wr32(p + 8, t); wr32(p + 12, t); wr32(p + 16, t);
        wr16(p + 26, 2); wr32(p + 28, spb); wr32(p + 40, lfb);
        if (blk_write(dev, (rd32(gdt + 8) + (11 - 1) / ipb) * spb, spb, zero) < 0) ok = 0;
    }
    // the superblock and descriptors, in group 0 and each group that keeps a copy
    g = 0;
    while (g < ngroups && ok) {
        if (has_super(g)) {
            base = first + g * bpg;
            wr16(sb + 90, g);                   // s_block_group_nr
            if (g == 0) { if (blk_write(dev, 2, 2, sb) < 0) ok = 0; }
            else {
                memset(bm, 0, bsize);
                memcpy(bm, sb, 1024);
                if (blk_write(dev, base * spb, spb, bm) < 0) ok = 0;
            }
            if (blk_write(dev, (base + 1) * spb, gdtb * spb, gdt) < 0) ok = 0;
        }
        ++g;
    }
    blk_flush(dev);
    free(sb); free(gdt); free(bm); free(zero); free(ib);
    if (!ok) { klog("ext2: %s: a write failed making it\n", blk_nm(dev)); return 0 - 1; }
    klog("ext2: %s: made: %d blocks of %d bytes, %d inodes, %d groups\n", blk_nm(dev), blocks, bsize, ipg * ngroups, ngroups);
    return 0;
}
