// c4or1k's 9p root filesystem backing store: an in-memory inode tree
// loaded from tools/mkbootfs.js's bootfs.idx/bootfs.blob (a flattened,
// offline-built copy of basefs.json -- see that tool's header comment
// for the exact file formats and why this project doesn't parse JSON
// or fetch files at runtime the way jor1k's fsloader.js does).
//
// Mirrors jorconsole/jor1k/js/worker/filesystem/filesystem.js's FS
// object, but as parallel int arrays (malloc'd, not static -- same
// reason as mem.c's ram[]: c4lc's 256KB data-segment cap) rather than
// an array of struct/object inodes. No lazy loading, no async events:
// everything basefs.json describes is either already resident (loaded
// from bootfs.blob at startup) or created synchronously at runtime by
// a 9p request (TLCREATE/TMKDIR/TWRITE) -- there's no host round trip
// to wait on, so the "AddEvent" deferred-callback machinery jor1k
// needs (its files are fetched over the network) has no reason to
// exist here; every bootfs_* call below returns its result immediately.

// Sized for the extended filesystem (basefs.json + fs.json merged:
// ~7100 inodes, longest name 54 bytes), with headroom for the inodes a
// running guest creates (TLCREATE/TMKDIR). The basefs-only build uses a
// fraction of this; the arrays are malloc'd, so the cap only bounds the
// allocation, it is not a per-boot cost.
enum { BOOTFS_MAX_INODES = 16384 };
enum { BOOTFS_NAME_LEN = 64 };

// S_IFMT bits, matching filesystem.js.
enum { BOOTFS_S_IFDIR = 0x4000, BOOTFS_S_IFREG = 0x8000, BOOTFS_S_IFLNK = 0xA000 };
enum { BOOTFS_S_IFMT = 0xF000 };

void bootfs_init(char *idx_path, char *blob_path);

// Inode field accessors. idx is always a valid, already-resolved
// inode index (bootfs_search/bootfs_create_* are how callers get one).
int bootfs_mode(int idx);
int bootfs_uid(int idx);
int bootfs_gid(int idx);
int bootfs_parentid(int idx);
int bootfs_size(int idx);
int bootfs_ctime(int idx);
int bootfs_mtime(int idx);
int bootfs_atime(int idx);
void bootfs_set_mode(int idx, int mode);
void bootfs_set_uid(int idx, int uid);
void bootfs_set_gid(int idx, int gid);
void bootfs_set_atime(int idx, int t);
void bootfs_set_mtime(int idx, int t);
void bootfs_set_ctime(int idx, int t);
void bootfs_get_name(int idx, char *buf); // writes up to BOOTFS_NAME_LEN bytes, NUL-terminated

// Tree navigation.
int bootfs_search(int parentid, char *name); // -1 if not found
int bootfs_root();

// Content access. offset is always within [0, bootfs_size(idx)] for
// reads -- callers clamp first, matching every 9p handler's own
// clamping. bootfs_write_byte grows the inode (bootfs_change_size)
// itself if offset reaches past the current size, matching
// filesystem.js's Write; no bulk/callback write entry point -- c4lc
// function pointers are usable (plan doc), but virtio9p.c already
// pulls request bytes one at a time from the descriptor chain via
// virtio_get_byte(), so looping bootfs_write_byte() per byte there is
// simpler than threading a callback through here for no real gain.
int bootfs_read_byte(int idx, int offset);
void bootfs_write_byte(int idx, int offset, int val);
void bootfs_change_size(int idx, int newsize);

// Directory listing: builds (or returns the cached) 9p2000.L-format
// "." / ".." / children byte stream matching filesystem.js's
// FillDirectory, lazily, on first read after creation. Returns the
// inode holding it (its bootfs_size/bootfs_read_byte serve the bytes);
// same idx as passed in -- directories store their own listing.
void bootfs_fill_directory(int idx);
void bootfs_mark_dirty(int idx); // call after any create/unlink/rename touching idx's children

// Mutation. All return the new inode's index (or -1 on failure, only
// possible from running out of inode slots).
int bootfs_create_file(char *name, int parentid);
int bootfs_create_dir(char *name, int parentid);
int bootfs_create_symlink(char *name, int parentid, char *target);
int bootfs_unlink(int idx); // 0 if idx is a non-empty directory, 1 on success
int bootfs_rename(int olddirid, char *oldname, int newdirid, char *newname); // 0/1

// qid.path is just the inode index (unique, stable for the process's
// lifetime -- matches jor1k's own qidnumber closely enough that
// nothing downstream cares about the exact values, only that they're
// unique and stable). qid.type is derived from the mode's S_IFMT bits
// the same way filesystem.js computes it (`mode >> 8`), not stored.
int bootfs_qid_type(int idx);

int bootfs_total_size();
int bootfs_inode_count();
