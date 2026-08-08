#include "virtio9p.h"
#include "virtio.h"
#include "bootfs.h"

enum { MAX_FIDS = 1024 };
enum { FID_NONE = -1, FID_INODE = 1, FID_XATTR = 2 };

enum { ENOENT = 2, ENOTEMPTY = 39, ENOTSUPP = 524 };

enum { P9_SETATTR_MODE = 0x1, P9_SETATTR_UID = 0x2, P9_SETATTR_GID = 0x4, P9_SETATTR_SIZE = 0x8 };
enum { P9_SETATTR_ATIME = 0x10, P9_SETATTR_MTIME = 0x20, P9_SETATTR_CTIME = 0x40 };
enum { P9_SETATTR_ATIME_SET = 0x80, P9_SETATTR_MTIME_SET = 0x100 };

enum { BLOCKSIZE = 8192 };
enum { MSIZE_MAX = 8192 }; // replybuffer is sized MSIZE_MAX*2; Tversion requests above this are clamped
enum { WALK_MAX = 16 };
enum { STRBUF_LEN = 256 }; // generous headroom over BOOTFS_NAME_LEN for path components/version strings

int *fid_inodeid, *fid_type, *fid_uid;

char *replybuffer;
int replybuffersize;
int rb_off; // write cursor while building a reply's payload (payload starts at offset 7)
int msize;

void virtio9p_init() {
    int i;
    fid_inodeid = malloc(MAX_FIDS * 8);
    fid_type = malloc(MAX_FIDS * 8);
    fid_uid = malloc(MAX_FIDS * 8);
    for (i = 0; i < MAX_FIDS; ++i) { fid_inodeid[i] = -1; fid_type[i] = FID_NONE; fid_uid[i] = 0; }
    replybuffer = malloc(MSIZE_MAX * 2);
    replybuffersize = 0;
    msize = MSIZE_MAX;
}

// ---- request unmarshalling (GetByte-style, matches marshall.js's Unmarshall2) ----

int get_w() {
    int v;
    v = virtio_get_byte();
    v = v | (virtio_get_byte() << 8);
    v = v | (virtio_get_byte() << 16);
    v = v | (virtio_get_byte() << 24);
    return v;
}

int get_d() {
    int v;
    v = get_w();
    virtio_get_byte(); virtio_get_byte(); virtio_get_byte(); virtio_get_byte(); // high 4 bytes, always 0 for us
    return v;
}

int get_h() {
    int v;
    v = virtio_get_byte();
    v = v | (virtio_get_byte() << 8);
    return v;
}

// Reads a 9p string (2-byte length prefix + bytes) into buf, truncated
// to bufcap-1 and NUL-terminated. Drains any bytes beyond bufcap from
// the stream regardless, so the request cursor stays correctly
// positioned for whatever field follows -- silent truncation would
// otherwise desync every subsequent get_*() call in this request.
void get_s(char *buf, int bufcap) {
    int len, i, c;
    len = get_h();
    for (i = 0; i < len; ++i) {
        c = virtio_get_byte();
        if (i < bufcap - 1) buf[i] = c;
    }
    if (len < bufcap) buf[len] = 0;
    else buf[bufcap - 1] = 0;
}

// ---- reply marshalling (writes into replybuffer at rb_off, matches marshall.js's Marshall) ----

void put_b(int v) { replybuffer[rb_off] = v & 0xFF; rb_off = rb_off + 1; }
void put_h(int v) { put_b(v); put_b(v >> 8); }
void put_w(int v) { put_b(v); put_b(v >> 8); put_b(v >> 16); put_b(v >> 24); }
void put_d(int v) { put_w(v); put_w(0); }
void put_qid(int idx) { put_b(bootfs_qid_type(idx)); put_w(0); put_d(idx); } // version always 0, matches everything but Rename

void put_s(char *s) {
    int len, i;
    len = 0;
    while (s[len]) ++len;
    put_h(len);
    for (i = 0; i < len; ++i) put_b(s[i]);
}

// Writes the 7-byte header (size, id+1, tag) at offset 0 and hands the
// whole [0, 7+payloadsize) buffer to virtio.c. Called with
// payloadsize = rb_off - 7 once every case below has finished writing
// its payload starting at rb_off=7 -- BuildReply itself never
// allocates the payload's offset, unlike 9p.js's version, because
// c4lc has no closures to capture "the offset so far" the way that
// JS's inline field-by-field marshalling does; tracking a single
// module-global cursor is the direct equivalent.
void build_reply(int id, int tag, int payloadsize) {
    int save;
    save = rb_off;
    rb_off = 0;
    put_w(payloadsize + 7);
    put_b(id + 1);
    put_h(tag);
    rb_off = save;
    replybuffersize = payloadsize + 7;
}

void send_error(int tag, int errorcode) {
    rb_off = 7;
    put_w(errorcode);
    build_reply(6, tag, rb_off - 7);
}

void virtio9p_receive_request(int queueidx, int descindex) {
    int size, id, tag;
    int fid, newfid, nwfid, dfid, olddirfid, newdirfid, dirfd;
    int mode, flags, gid, uid, major, minor, idx, inode, target;
    int offset, count, i, ret, found;
    int mask, atime_sec, atime_nsec, mtime_sec, mtime_nsec, newsize;
    int nwname, nwidx, countpos;
    int statfs_total, statfs_free;
    char namebuf[STRBUF_LEN], namebuf2[STRBUF_LEN];
    char walknames[WALK_MAX * BOOTFS_NAME_LEN];

    size = get_w();
    id = virtio_get_byte();
    tag = get_h();

    switch (id) {
    case 8: // statfs
        statfs_total = bootfs_total_size();
        statfs_free = (1024 * 1024 * 1024) / BLOCKSIZE;
        rb_off = 7;
        put_w(0x01021997);
        put_w(BLOCKSIZE);
        put_d(statfs_free);
        put_d(statfs_free - statfs_total / BLOCKSIZE);
        put_d(statfs_free - statfs_total / BLOCKSIZE);
        put_d(bootfs_inode_count());
        put_d(1024 * 1024);
        put_d(0);
        put_w(256);
        build_reply(id, tag, rb_off - 7);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    case 112: // topen
    case 12:  // tlopen
        fid = get_w();
        mode = get_w();
        idx = fid_inodeid[fid];
        if ((bootfs_mode(idx) & BOOTFS_S_IFMT) == BOOTFS_S_IFDIR) bootfs_fill_directory(idx);
        rb_off = 7;
        put_qid(idx);
        put_w(msize - 24);
        build_reply(id, tag, rb_off - 7);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    case 70: // link (copy semantics, matches 9p.js)
        dfid = get_w();
        fid = get_w();
        get_s(namebuf, STRBUF_LEN);
        inode = fid_inodeid[fid];
        idx = bootfs_create_file(namebuf, fid_inodeid[dfid]);
        bootfs_set_mode(idx, bootfs_mode(inode));
        bootfs_change_size(idx, bootfs_size(inode));
        for (i = 0; i < bootfs_size(inode); ++i) bootfs_write_byte(idx, i, bootfs_read_byte(inode, i));
        rb_off = 7;
        build_reply(id, tag, 0);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    case 16: // symlink
        fid = get_w();
        get_s(namebuf, STRBUF_LEN);
        get_s(namebuf2, STRBUF_LEN);
        gid = get_w();
        idx = bootfs_create_symlink(namebuf, fid_inodeid[fid], namebuf2);
        bootfs_set_uid(idx, fid_uid[fid]);
        bootfs_set_gid(idx, gid);
        rb_off = 7;
        put_qid(idx);
        build_reply(id, tag, rb_off - 7);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    case 18: // mknod (major/minor not tracked by bootfs -- no device nodes in basefs.json)
        fid = get_w();
        get_s(namebuf, STRBUF_LEN);
        mode = get_w();
        major = get_w();
        minor = get_w();
        gid = get_w();
        idx = bootfs_create_file(namebuf, fid_inodeid[fid]);
        bootfs_set_mode(idx, mode);
        bootfs_set_uid(idx, fid_uid[fid]);
        bootfs_set_gid(idx, gid);
        rb_off = 7;
        put_qid(idx);
        build_reply(id, tag, rb_off - 7);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    case 22: // treadlink
        fid = get_w();
        idx = fid_inodeid[fid];
        for (i = 0; i < bootfs_size(idx) && i < STRBUF_LEN - 1; ++i) namebuf[i] = bootfs_read_byte(idx, i);
        namebuf[i] = 0;
        rb_off = 7;
        put_s(namebuf);
        build_reply(id, tag, rb_off - 7);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    case 72: // tmkdir
        fid = get_w();
        get_s(namebuf, STRBUF_LEN);
        mode = get_w();
        gid = get_w();
        idx = bootfs_create_dir(namebuf, fid_inodeid[fid]);
        bootfs_set_mode(idx, mode | BOOTFS_S_IFDIR);
        bootfs_set_uid(idx, fid_uid[fid]);
        bootfs_set_gid(idx, gid);
        rb_off = 7;
        put_qid(idx);
        build_reply(id, tag, rb_off - 7);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    case 14: // tlcreate
        fid = get_w();
        get_s(namebuf, STRBUF_LEN);
        flags = get_w();
        mode = get_w();
        gid = get_w();
        idx = bootfs_create_file(namebuf, fid_inodeid[fid]);
        fid_inodeid[fid] = idx;
        fid_type[fid] = FID_INODE;
        bootfs_set_uid(idx, fid_uid[fid]);
        bootfs_set_gid(idx, gid);
        bootfs_set_mode(idx, mode);
        rb_off = 7;
        put_qid(idx);
        put_w(msize - 24);
        build_reply(id, tag, rb_off - 7);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    case 52: // lock -- always succeeds
        rb_off = 7;
        put_w(0);
        build_reply(id, tag, rb_off - 7);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    case 24: // getattr
        fid = get_w();
        mask = get_d();
        idx = fid_inodeid[fid];
        rb_off = 7;
        put_d(mask);
        put_qid(idx);
        put_w(bootfs_mode(idx));
        put_w(bootfs_uid(idx));
        put_w(bootfs_gid(idx));
        put_d(1); // nlink
        put_d(0); // device id (major/minor): not tracked, no device nodes in this fs
        put_d(bootfs_size(idx));
        put_d(BLOCKSIZE);
        put_d(bootfs_size(idx) / 512 + 1);
        put_d(bootfs_atime(idx)); put_d(0);
        put_d(bootfs_mtime(idx)); put_d(0);
        put_d(bootfs_ctime(idx)); put_d(0);
        put_d(0); put_d(0); // btime: not tracked
        put_d(0); // st_gen
        put_d(0); // data_version
        build_reply(id, tag, rb_off - 7);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    case 26: // setattr
        fid = get_w();
        mask = get_w();
        mode = get_w();
        uid = get_w();
        gid = get_w();
        newsize = get_d();
        atime_sec = get_d(); atime_nsec = get_d();
        mtime_sec = get_d(); mtime_nsec = get_d();
        idx = fid_inodeid[fid];
        if (mask & P9_SETATTR_MODE) bootfs_set_mode(idx, mode);
        if (mask & P9_SETATTR_UID) bootfs_set_uid(idx, uid);
        if (mask & P9_SETATTR_GID) bootfs_set_gid(idx, gid);
        if (mask & P9_SETATTR_ATIME_SET) bootfs_set_atime(idx, atime_sec);
        // 9p.js sets inode.atime (not mtime) here -- a transcription
        // bug in the porting reference (P9_SETATTR_MTIME_SET branch
        // assigns to the atime field). Fixed here rather than ported
        // faithfully: nothing depends on replicating it, and it would
        // only ever make `ls -l` mtimes wrong.
        if (mask & P9_SETATTR_MTIME_SET) bootfs_set_mtime(idx, mtime_sec);
        if (mask & P9_SETATTR_ATIME) bootfs_set_atime(idx, 0); // "current time": not wired to a wall clock, see docs
        if (mask & P9_SETATTR_MTIME) bootfs_set_mtime(idx, 0);
        if (mask & P9_SETATTR_CTIME) bootfs_set_ctime(idx, 0);
        if (mask & P9_SETATTR_SIZE) bootfs_change_size(idx, newsize);
        rb_off = 7;
        build_reply(id, tag, 0);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    case 50: // fsync
        get_w(); get_d();
        rb_off = 7;
        build_reply(id, tag, 0);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    case 40: // treaddir
    case 116: // read
        fid = get_w();
        offset = get_d();
        count = get_w();
        idx = fid_inodeid[fid];
        if (id == 40) bootfs_fill_directory(idx);
        if (fid_type[fid] == FID_XATTR) count = 0; // capabilities xattr not supported, see xattrwalk below
        else if (bootfs_size(idx) < offset + count) count = bootfs_size(idx) - offset;
        if (count < 0) count = 0;
        rb_off = 7;
        put_w(count);
        for (i = 0; i < count; ++i) put_b(bootfs_read_byte(idx, offset + i));
        build_reply(id, tag, rb_off - 7);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    case 118: // write
        fid = get_w();
        offset = get_d();
        count = get_w();
        idx = fid_inodeid[fid];
        for (i = 0; i < count; ++i) bootfs_write_byte(idx, offset + i, virtio_get_byte());
        rb_off = 7;
        put_w(count);
        build_reply(id, tag, rb_off - 7);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    case 74: // renameat
        olddirfid = get_w();
        get_s(namebuf, STRBUF_LEN);
        newdirfid = get_w();
        get_s(namebuf2, STRBUF_LEN);
        ret = bootfs_rename(fid_inodeid[olddirfid], namebuf, fid_inodeid[newdirfid], namebuf2);
        if (!ret) { send_error(tag, ENOENT); virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize); break; }
        rb_off = 7;
        build_reply(id, tag, 0);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    case 76: // tunlinkat
        dirfd = get_w();
        get_s(namebuf, STRBUF_LEN);
        get_w(); // flags: unused, matches 9p.js
        found = bootfs_search(fid_inodeid[dirfd], namebuf);
        if (found == -1) { send_error(tag, ENOENT); virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize); break; }
        ret = bootfs_unlink(found);
        if (!ret) { send_error(tag, ENOTEMPTY); virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize); break; }
        rb_off = 7;
        build_reply(id, tag, 0);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    case 100: // version
        count = get_w(); // requested msize
        get_s(namebuf, STRBUF_LEN); // requested version string, unused (we always answer 9P2000.L)
        msize = count < MSIZE_MAX ? count : MSIZE_MAX;
        rb_off = 7;
        put_w(msize);
        put_s("9P2000.L");
        build_reply(id, tag, rb_off - 7);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    case 104: // attach
        fid = get_w();
        get_w(); // afid: unused, no auth
        get_s(namebuf, STRBUF_LEN); // uname: unused
        get_s(namebuf2, STRBUF_LEN); // aname: unused (root=host's "host" doesn't select among multiple exports)
        uid = get_w();
        fid_inodeid[fid] = bootfs_root();
        fid_type[fid] = FID_INODE;
        fid_uid[fid] = uid;
        rb_off = 7;
        put_qid(bootfs_root());
        build_reply(id, tag, rb_off - 7);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    case 108: // tflush -- nothing in-flight to flush, every request already completed synchronously
        get_h();
        rb_off = 7;
        build_reply(id, tag, 0);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    case 110: // walk
        fid = get_w();
        nwfid = get_w();
        nwname = get_h();
        if (nwname == 0) {
            fid_inodeid[nwfid] = fid_inodeid[fid];
            fid_type[nwfid] = FID_INODE;
            fid_uid[nwfid] = fid_uid[fid];
            rb_off = 7;
            put_h(0);
            build_reply(id, tag, rb_off - 7);
            virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
            break;
        }
        if (nwname > WALK_MAX) nwname = WALK_MAX; // real 9p clients walk at most a handful of components at once
        for (i = 0; i < nwname; ++i) get_s(walknames + i * BOOTFS_NAME_LEN, BOOTFS_NAME_LEN);
        idx = fid_inodeid[fid];
        rb_off = 9; // reserve 2 bytes at offset 7 for the nwqid count, patched in below
        nwidx = 0;
        for (i = 0; i < nwname; ++i) {
            idx = bootfs_search(idx, walknames + i * BOOTFS_NAME_LEN);
            if (idx == -1) break;
            put_qid(idx);
            ++nwidx;
        }
        if (nwidx > 0) { fid_inodeid[nwfid] = idx; fid_type[nwfid] = FID_INODE; fid_uid[nwfid] = fid_uid[fid]; }
        countpos = rb_off;
        rb_off = 7;
        put_h(nwidx);
        rb_off = countpos;
        build_reply(id, tag, rb_off - 7);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    case 120: // clunk
        fid = get_w();
        fid_inodeid[fid] = -1;
        fid_type[fid] = FID_NONE;
        rb_off = 7;
        build_reply(id, tag, 0);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    case 30: // xattrwalk -- capabilities not supported (see "security.capability" note below)
        fid = get_w();
        newfid = get_w();
        get_s(namebuf, STRBUF_LEN);
        fid_inodeid[newfid] = fid_inodeid[fid];
        fid_type[newfid] = FID_NONE;
        // jor1k always answers "security.capability" xattrwalk with a
        // synthetic 12-byte "full capabilities" blob, which lets the
        // guest kernel skip capability checks on setuid binaries at
        // exec time. Reporting 0 here instead (no xattr support) means
        // the kernel falls back to the plain setuid-bit permission
        // model -- exactly what any real filesystem without xattrs
        // does, and every basefs.json binary that needs setuid
        // (busybox itself) already has the mode bit set directly.
        rb_off = 7;
        put_d(0);
        build_reply(id, tag, rb_off - 7);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
        break;

    default:
        printf("virtio9p: unsupported message id %d (tag=%d) -- sending an error reply, not aborting\n", id, tag);
        send_error(tag, ENOTSUPP);
        virtio_send_reply(queueidx, descindex, replybuffer, replybuffersize);
    }
}
