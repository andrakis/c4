#include "bootfs.h"

// bootfs.idx record layout, must match tools/mkbootfs.js exactly:
// name[BOOTFS_NAME_LEN] then 8 x int32 LE (mode,uid,gid,parentid,
// firstid,nextid,size,bloboff). The int32 fields sit immediately after
// the name slot, so their offsets are all relative to BOOTFS_NAME_LEN
// (raised to 64 for the extended fs -- see bootfs.h).
// c4lc requires LITERAL enum initializers (no BOOTFS_NAME_LEN+N
// expressions), so these are spelled out for BOOTFS_NAME_LEN=64: the
// int32 fields sit right after the 64-byte name slot. Keep in lockstep
// with BOOTFS_NAME_LEN and tools/mkbootfs.js's RECORD_LEN if either
// changes.
enum { REC_LEN = 96 };   // 64 (name) + 8 x int32
enum { REC_MODE = 64, REC_UID = 68, REC_GID = 72, REC_PARENTID = 76,
       REC_FIRSTID = 80, REC_NEXTID = 84, REC_SIZE = 88, REC_BLOBOFF = 92 };

int *m_mode, *m_uid, *m_gid, *m_parentid, *m_firstid, *m_nextid;
int *m_size, *m_bloboff, *m_atime, *m_mtime, *m_ctime, *m_dirty;
char *m_name; // BOOTFS_MAX_INODES * BOOTFS_NAME_LEN, fixed-width slots
char **m_heap; // NULL until first write/creation materializes a private buffer
int *m_heapsize;

char *blob;
int blob_len;
int inode_count;

// Sign-extends the composed 32-bit pattern -- matches cpu.c's sext()
// idiom exactly (see that file's header comment for why: c4lc's
// 64-bit `int` never truncates at bit 31, so parentid/firstid/nextid
// records written as -1 by mkbootfs.js -- i.e. 0xFFFFFFFF's bytes --
// would otherwise read back as the *positive* 4294967295, not -1.
// bootfs_search's `while (id != -1)` loop would then treat that as a
// real, wildly out-of-bounds inode index -- the actual root cause of
// a SIGSEGV found the hard way partway through a real Linux boot (see
// docs/c4or1k-design.md's M5 section): walking into an empty
// directory (nothing ever sets its firstid away from mkbootfs.js's
// default -1) dereferenced a wild pointer computed from that huge
// misread "index".
int rd_le32(char *p, int off) {
    int v, signbit;
    v = (p[off] & 0xFF) | ((p[off + 1] & 0xFF) << 8) | ((p[off + 2] & 0xFF) << 16) | ((p[off + 3] & 0xFF) << 24);
    v = v & 0xFFFFFFFF;
    signbit = 0x80000000;
    return (v ^ signbit) - signbit;
}

int str_eq(char *a, char *b) {
    while (*a && *b) { if (*a != *b) return 0; ++a; ++b; }
    return *a == *b;
}

int str_len(char *s) {
    int n; n = 0;
    while (s[n]) ++n;
    return n;
}

// Reads a whole file into a malloc'd buffer sized exactly to its
// length; *len_out receives that length. Same shape as boot.c's
// load_kernel, but returning the buffer instead of copying into ram[].
enum { RWF_CHUNK = 65536 };
char *read_whole_file(char *path, int *len_out) {
    int fd, n, cap, total;
    char *buf;

    fd = open(path, 0);
    if (fd < 0) { printf("bootfs: could not open %s\n", path); exit(1); }
    cap = RWF_CHUNK;
    buf = malloc(cap);
    total = 0;
    while (1) {
        if (total + RWF_CHUNK > cap) {
            char *nbuf;
            cap = cap * 2;
            nbuf = malloc(cap);
            memcpy(nbuf, buf, total);
            free(buf);
            buf = nbuf;
        }
        n = read(fd, buf + total, RWF_CHUNK);
        if (n <= 0) break;
        total = total + n;
    }
    close(fd);
    *len_out = total;
    return buf;
}

void bootfs_init(char *idx_path, char *blob_path) {
    char *idx;
    int idx_len, i, off;

    idx = read_whole_file(idx_path, &idx_len);
    inode_count = rd_le32(idx, 0);
    if (inode_count > BOOTFS_MAX_INODES) {
        printf("bootfs: %d inodes exceeds BOOTFS_MAX_INODES=%d\n", inode_count, BOOTFS_MAX_INODES);
        exit(1);
    }

    m_mode = malloc(BOOTFS_MAX_INODES * 8); m_uid = malloc(BOOTFS_MAX_INODES * 8);
    m_gid = malloc(BOOTFS_MAX_INODES * 8); m_parentid = malloc(BOOTFS_MAX_INODES * 8);
    m_firstid = malloc(BOOTFS_MAX_INODES * 8); m_nextid = malloc(BOOTFS_MAX_INODES * 8);
    m_size = malloc(BOOTFS_MAX_INODES * 8); m_bloboff = malloc(BOOTFS_MAX_INODES * 8);
    m_atime = malloc(BOOTFS_MAX_INODES * 8); m_mtime = malloc(BOOTFS_MAX_INODES * 8);
    m_ctime = malloc(BOOTFS_MAX_INODES * 8); m_dirty = malloc(BOOTFS_MAX_INODES * 8);
    m_name = malloc(BOOTFS_MAX_INODES * BOOTFS_NAME_LEN);
    m_heap = malloc(BOOTFS_MAX_INODES * 8);
    m_heapsize = malloc(BOOTFS_MAX_INODES * 8);

    for (i = 0; i < BOOTFS_MAX_INODES; ++i) {
        m_heap[i] = 0;
        m_heapsize[i] = 0;
        m_dirty[i] = 1; // directories fill their listing lazily on first open
        m_atime[i] = 0; m_mtime[i] = 0; m_ctime[i] = 0;
    }

    for (i = 0; i < inode_count; ++i) {
        off = 4 + i * REC_LEN;
        memcpy(m_name + i * BOOTFS_NAME_LEN, idx + off, BOOTFS_NAME_LEN);
        m_mode[i] = rd_le32(idx, off + REC_MODE);
        m_uid[i] = rd_le32(idx, off + REC_UID);
        m_gid[i] = rd_le32(idx, off + REC_GID);
        m_parentid[i] = rd_le32(idx, off + REC_PARENTID);
        m_firstid[i] = rd_le32(idx, off + REC_FIRSTID);
        m_nextid[i] = rd_le32(idx, off + REC_NEXTID);
        m_size[i] = rd_le32(idx, off + REC_SIZE);
        m_bloboff[i] = rd_le32(idx, off + REC_BLOBOFF);
    }
    free(idx);

    blob = read_whole_file(blob_path, &blob_len);
    printf("bootfs: loaded %d inodes, %d blob bytes\n", inode_count, blob_len);
}

int bootfs_mode(int idx) { return m_mode[idx]; }
int bootfs_uid(int idx) { return m_uid[idx]; }
int bootfs_gid(int idx) { return m_gid[idx]; }
int bootfs_parentid(int idx) { return m_parentid[idx]; }
int bootfs_size(int idx) { return m_size[idx]; }
int bootfs_ctime(int idx) { return m_ctime[idx]; }
int bootfs_mtime(int idx) { return m_mtime[idx]; }
int bootfs_atime(int idx) { return m_atime[idx]; }
void bootfs_set_mode(int idx, int mode) { m_mode[idx] = mode; }
void bootfs_set_uid(int idx, int uid) { m_uid[idx] = uid; }
void bootfs_set_gid(int idx, int gid) { m_gid[idx] = gid; }
void bootfs_set_atime(int idx, int t) { m_atime[idx] = t; }
void bootfs_set_mtime(int idx, int t) { m_mtime[idx] = t; }
void bootfs_set_ctime(int idx, int t) { m_ctime[idx] = t; }

void bootfs_get_name(int idx, char *buf) {
    memcpy(buf, m_name + idx * BOOTFS_NAME_LEN, BOOTFS_NAME_LEN);
}

int bootfs_root() { return 0; }

int bootfs_search(int parentid, char *name) {
    int id;
    id = m_firstid[parentid];
    while (id != -1) {
        if (str_eq(m_name + id * BOOTFS_NAME_LEN, name)) return id;
        id = m_nextid[id];
    }
    return -1;
}

int bootfs_qid_type(int idx) { return (m_mode[idx] & BOOTFS_S_IFMT) >> 8; }

int bootfs_read_byte(int idx, int offset) {
    if (m_heap[idx]) return m_heap[idx][offset] & 0xFF;
    return blob[m_bloboff[idx] + offset] & 0xFF;
}

// Materializes idx's content into a private, resizable heap buffer,
// copying over whatever it already had (from the blob or a smaller
// heap buffer) -- matches filesystem.js's ChangeSize exactly.
void bootfs_change_size(int idx, int newsize) {
    char *nbuf;
    int copy, i;

    if (newsize == m_size[idx] && m_heap[idx]) return;
    nbuf = malloc(newsize > 0 ? newsize : 1);
    copy = m_size[idx] < newsize ? m_size[idx] : newsize;
    for (i = 0; i < copy; ++i) nbuf[i] = bootfs_read_byte(idx, i);
    if (m_heap[idx]) free(m_heap[idx]);
    m_heap[idx] = nbuf;
    m_heapsize[idx] = newsize;
    m_size[idx] = newsize;
}

void bootfs_write_byte(int idx, int offset, int val) {
    if (offset >= m_heapsize[idx]) {
        bootfs_change_size(idx, ((offset + 1) * 3) / 2);
    } else if (offset >= m_size[idx]) {
        m_size[idx] = offset + 1;
    }
    m_heap[idx][offset] = val & 0xFF;
}

void bootfs_mark_dirty(int idx) { m_dirty[idx] = 1; }

// Writes one Q/d/b/s direntry into dirinode's heap buffer at `off`;
// returns its length. Split out of bootfs_fill_directory (and defined
// before it -- c4lc is single-pass, no forward declarations within a
// file) only because c4lc has no local-function-scope helpers --
// everything is top-level.
int bootfs_put_direntry(int dirinode, int off, int entinode, int nextoff, char *name) {
    int start, namelen, i;
    start = off;
    // Q: type(b) version(w) path(d)
    m_heap[dirinode][off] = bootfs_qid_type(entinode); off = off + 1;
    m_heap[dirinode][off] = 0; m_heap[dirinode][off+1] = 0;
    m_heap[dirinode][off+2] = 0; m_heap[dirinode][off+3] = 0; off = off + 4; // version=0
    m_heap[dirinode][off] = entinode & 0xFF; m_heap[dirinode][off+1] = (entinode >> 8) & 0xFF;
    m_heap[dirinode][off+2] = (entinode >> 16) & 0xFF; m_heap[dirinode][off+3] = (entinode >> 24) & 0xFF;
    m_heap[dirinode][off+4] = 0; m_heap[dirinode][off+5] = 0; m_heap[dirinode][off+6] = 0; m_heap[dirinode][off+7] = 0;
    off = off + 8;
    // d: next offset
    m_heap[dirinode][off] = nextoff & 0xFF; m_heap[dirinode][off+1] = (nextoff >> 8) & 0xFF;
    m_heap[dirinode][off+2] = (nextoff >> 16) & 0xFF; m_heap[dirinode][off+3] = (nextoff >> 24) & 0xFF;
    m_heap[dirinode][off+4] = 0; m_heap[dirinode][off+5] = 0; m_heap[dirinode][off+6] = 0; m_heap[dirinode][off+7] = 0;
    off = off + 8;
    // b: type (mode >> 12)
    m_heap[dirinode][off] = (bootfs_mode(entinode) >> 12) & 0xFF; off = off + 1;
    // s: name
    namelen = str_len(name);
    m_heap[dirinode][off] = namelen & 0xFF; m_heap[dirinode][off+1] = (namelen >> 8) & 0xFF;
    off = off + 2;
    for (i = 0; i < namelen; ++i) { m_heap[dirinode][off] = name[i]; off = off + 1; }
    return off - start;
}

// Q(qid=13) + d(offset,8) + b(mode>>12,1) + s(name: 2+len) per entry,
// "." then ".." then every child -- matches filesystem.js's
// FillDirectory field-for-field (including reusing the *next* entry's
// start offset as "the offset to read this entry back at", which is
// meaningless for a byte array traversal but is what real 9p clients
// expect: each direntry's "offset" field is where a subsequent
// TREADDIR should resume, i.e. simply "one past this entry").
void bootfs_fill_directory(int idx) {
    int parentid, size, id, off, entrylen;
    char namebuf[BOOTFS_NAME_LEN];

    if (!m_dirty[idx]) return;
    parentid = m_parentid[idx];
    if (parentid == -1) parentid = idx;

    size = 0;
    size = size + 13 + 8 + 1 + 2 + 1; // "."
    size = size + 13 + 8 + 1 + 2 + 2; // ".."
    id = m_firstid[idx];
    while (id != -1) {
        bootfs_get_name(id, namebuf);
        size = size + 13 + 8 + 1 + 2 + str_len(namebuf);
        id = m_nextid[id];
    }

    bootfs_change_size(idx, size);
    off = 0;

    entrylen = 13 + 8 + 1 + 2 + 1;
    off = off + bootfs_put_direntry(idx, off, idx, off + entrylen, ".");
    entrylen = 13 + 8 + 1 + 2 + 2;
    off = off + bootfs_put_direntry(idx, off, parentid, off + entrylen, "..");

    id = m_firstid[idx];
    while (id != -1) {
        bootfs_get_name(id, namebuf);
        entrylen = 13 + 8 + 1 + 2 + str_len(namebuf);
        off = off + bootfs_put_direntry(idx, off, id, off + entrylen, namebuf);
        id = m_nextid[id];
    }

    m_dirty[idx] = 0;
}

int bootfs_create_inode(char *name, int mode, int parentid) {
    int idx, namelen, i;
    if (inode_count >= BOOTFS_MAX_INODES) return -1;
    idx = inode_count;
    inode_count = inode_count + 1;

    namelen = str_len(name);
    if (namelen >= BOOTFS_NAME_LEN) namelen = BOOTFS_NAME_LEN - 1;
    for (i = 0; i < namelen; ++i) m_name[idx * BOOTFS_NAME_LEN + i] = name[i];
    m_name[idx * BOOTFS_NAME_LEN + namelen] = 0;

    m_mode[idx] = mode;
    m_uid[idx] = 0; m_gid[idx] = 0;
    m_parentid[idx] = parentid;
    m_firstid[idx] = -1;
    m_size[idx] = 0;
    m_bloboff[idx] = 0;
    m_heap[idx] = 0;
    m_heapsize[idx] = 0;
    m_dirty[idx] = 1;

    m_nextid[idx] = m_firstid[parentid];
    m_firstid[parentid] = idx;
    m_dirty[parentid] = 1;
    return idx;
}

int bootfs_create_file(char *name, int parentid) {
    return bootfs_create_inode(name, BOOTFS_S_IFREG, parentid);
}

int bootfs_create_dir(char *name, int parentid) {
    return bootfs_create_inode(name, BOOTFS_S_IFDIR, parentid);
}

int bootfs_create_symlink(char *name, int parentid, char *target) {
    int idx, tlen, i;
    idx = bootfs_create_inode(name, BOOTFS_S_IFLNK, parentid);
    if (idx < 0) return -1;
    tlen = str_len(target);
    bootfs_change_size(idx, tlen);
    for (i = 0; i < tlen; ++i) bootfs_write_byte(idx, i, target[i]);
    return idx;
}

int bootfs_find_previous_id(int idx) {
    int parentid, id;
    parentid = m_parentid[idx];
    id = m_firstid[parentid];
    while (id != -1) {
        if (m_nextid[id] == idx) return id;
        id = m_nextid[id];
    }
    return -1;
}

int bootfs_unlink(int idx) {
    int parentid, prev;
    if (idx == 0) return 0;
    if ((m_mode[idx] & BOOTFS_S_IFMT) == BOOTFS_S_IFDIR) {
        if (m_firstid[idx] != -1) return 0;
    }
    parentid = m_parentid[idx];
    if (m_firstid[parentid] == idx) {
        m_firstid[parentid] = m_nextid[idx];
    } else {
        prev = bootfs_find_previous_id(idx);
        m_nextid[prev] = m_nextid[idx];
    }
    m_dirty[parentid] = 1;
    m_nextid[idx] = -1;
    m_firstid[idx] = -1;
    m_parentid[idx] = -1;
    return 1;
}

int bootfs_rename(int olddirid, char *oldname, int newdirid, char *newname) {
    int oldid, newid, namelen, i;
    oldid = bootfs_search(olddirid, oldname);
    if (oldid == -1) return 0;
    newid = bootfs_search(newdirid, newname);
    if (newid != -1) bootfs_unlink(newid);

    if (m_firstid[olddirid] == oldid) {
        m_firstid[olddirid] = m_nextid[oldid];
    } else {
        int prev; prev = bootfs_find_previous_id(oldid);
        m_nextid[prev] = m_nextid[oldid];
    }

    namelen = str_len(newname);
    if (namelen >= BOOTFS_NAME_LEN) namelen = BOOTFS_NAME_LEN - 1;
    for (i = 0; i < namelen; ++i) m_name[oldid * BOOTFS_NAME_LEN + i] = newname[i];
    m_name[oldid * BOOTFS_NAME_LEN + namelen] = 0;

    m_parentid[oldid] = newdirid;
    m_nextid[oldid] = m_firstid[newdirid];
    m_firstid[newdirid] = oldid;

    m_dirty[olddirid] = 1;
    m_dirty[newdirid] = 1;
    return 1;
}

int bootfs_total_size() {
    int i, total;
    total = 0;
    for (i = 0; i < inode_count; ++i) total = total + m_size[i];
    return total;
}

int bootfs_inode_count() { return inode_count; }
