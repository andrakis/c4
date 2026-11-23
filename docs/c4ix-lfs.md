# 64-bit file positions in C4IX (C4IX_LFS)

C4IX's `int` is the machine's word: 32 bits on libjs and c4pc, 64 on
native c4m. A file position was an `int`, so on a 32-bit build a file
stopped at 2 GB, and a disk's file past that read wrong (its size's high
word ignored). Now a position and a disk's file's size are `c4off`
(c4ix.h):

| build | C4IX_LFS | c4off | cost |
|---|---|---|---|
| native c4m, 64-bit word (Makefile `c4ix.c4r`) | defined | `long long`, which is the word | none: the same instructions |
| libjs, c4pc (build-images.sh, `.c4pc/kernel/c4ix32.c4r`) | not by default | `int`, the word | none |
| either 32-bit one, built with `-D C4IX_LFS` | defined | `long long`, two words | a helper call for each operation on a position |

On 32 bits the default stays the word: a long long there is two words,
and its arithmetic is c4lc's helpers (docs/c4lc-design.md 14.7), on every
read's and write's position. The files that need more are a disk's
(ext2), on a PC; the browser's are memory's and the host's, under 4 GB.

## What changes

- A description's position (`struct file`'s `pos`) is a c4off.
- A disk's file keeps its whole size, `e2size`, beside `size` (what a
  word holds of it, and what RAM holds once it is read in). ext2's size
  is the inode's i_size and, for a regular file, its high word at byte
  108 (the large_file feature, which C4IX's mkfs sets and mke2fs does by
  default). Without C4IX_LFS a file of 2 GB or more is 2 GB less a byte
  long, and is read to there.
- A file bigger than a word's worth is never read into RAM whole (it
  could not be): it is read a range at a time where it is, as a big file
  on the host is (vfs_pread, ext2_read with a c4off position).
- `lseek` is as before, and refuses a position a word cannot hold (on a
  32-bit build, past 2 GB): **SYS_LSEEK64** (256) is the one for those:
  the descriptor, two words (the offset's low 32 bits, then its high),
  and whence, the new position written back into the two; 0 or -1.
  libc4ix's `ulseek64(fd, int *pos, whence)` calls it.
- stat has the size whole too: **ST_SIZE_LO** and **ST_SIZE_HI** (words 8
  and 9; ST_WORDS is 10), 32 bits each. ST_SIZE stays a word: on a 32-bit
  build 2 GB less a byte at most.
- A RAM file stays a word's worth: a write past 2 GB on a 32-bit build is
  refused.

Built on c4lc's `long long` (and c4fc's, at 64 bits: test-c4fc compiles
the kernel with C4IX_LFS, as `make c4ix.c4r` does, and holds the two
byte for byte).

## Not yet

- libc (src/c4ix/libc): `off_t` is still an int, `lseek` the word's. A
  POSIX program (Busybox) sees files to 2 GB on a 32-bit build.
- Writing a file past 2 GB: what a disk's file gets written goes through
  RAM until it is closed (docs/c4ix-ext2.md 3).
- Host drives' files (libjs's, c4pc's) are 32-bit sized.
- /dev/hdN as files (docs/c4ix-ext2.md 4) would want it.

## Measured

2026-11-23, c4pc-host with `--disk`: an ext2 image (mke2fs -d) holding a
sparse 5 GB file, with "START" at its beginning, a marker at 3 GB and
another in its last ten bytes, read by a test program (lfstest:
SYS_LSEEK64 to a position, a read, a relative seek back):

- the default 32-bit kernel: its size 2147483647, the start read, a seek
  to 3 GB refused, and reads stop at 2 GB less a byte;
- the 32-bit kernel built with `-D C4IX_LFS`: its size 0x140000000 in
  ST_SIZE_LO/HI, the 3 GB marker and the last ten bytes read, relative
  seeks across 2 GB and 4 GB right;
- e2fsck finds the disk clean after both, and the ext2 test of
  docs/c4ix-ext2.md (Linux's disks and C4IX's, written and read back)
  passes with the default kernel. The native kernel with C4IX_LFS boots
  on c4m and shuts down.

## Other places the word shows

Surveyed 2026-11-23 for where C4IX does 32 bits' (or a byte's) work that
a 64-bit word could do faster, or assumes 32 bits:

- **The PC runs the 64-bit build too** (docs/c4pc-64.md, 2026-11-23):
  c4pc's engine is built at either word size (C4PC_W64: c4pc-host64w,
  c4pcw64.elf), and its MMU walks tables of 512 eight-byte entries on the
  64-bit word, which src/c4ix/mmu.c builds (MMU_PTES, MMU_DSHIFT; its
  mmu_vbits was allocated in bytes and indexed by word, 8 KB short on 64
  bits, and is now sized by the word).
- **CRC-32 and inflate** (lib/archive.c): most of a boot's instructions
  where nothing accelerates them, as on native c4m. A byte a step, the
  bit buffer kept to 23 bits; a 64-bit word could take slicing-by-8 and a
  buffer refilled seven bytes at a time. The overlapping match copy is a
  byte at a time on either word.
- **ext2's bitmaps** (ext2.c balloc): scanned a byte at a time; a word
  at a time would skip 4 or 8 full bytes at once. Its rd16/rd32 build each
  field from bytes (76 places), which an aligned load could replace.
- **libc's memmove, strlen, strcmp** (libc/string.c): byte loops (memcpy,
  memset and memcmp are the machine's opcodes already).
- **The MMU's and sl4b's bitmaps** use 32 bits of each word (`i >> 5`):
  correct on 64 bits but half used, and two whole-word shortcuts never
  fire there. Only with an MMU, which native c4m has not.
- libc/fp.c's strtod keeps big integers in base-2^32 limbs of `unsigned
  int`, unmasked. That is right at both sizes: c4lc's `unsigned int` holds
  0..2^32-1 on a 64-bit word too (docs/c4lc-design.md). Tested
  2026-11-23: test-c4fp's vectors, and long decimals to 120 digits,
  halfway cases and the subnormal edges, all glibc's at 64 and 32 bits
  (fpconvgen.c has them now). With a 64-bit limb it could do half the
  steps on a 64-bit word.
