// c4or1k boot loader: loads a raw (already-decompressed) OR1000
// Linux kernel image into guest RAM at address 0 and patches its
// embedded device-tree blob's memory-size property, matching
// jor1k/js/worker/system.js's OnKernelLoaded/PatchKernel.
//
// No bzip2/ELF handling: jorconsole-sysroot's own postinstall step
// already unpacks vmlinux.bin.bz2 to a raw flat binary (confirmed via
// `file` = "data", not ELF) -- see docs/c4or1k-design.md's M4 notes.
// No Little2Big either: that's jor1k's own byte-swap-for-native-speed
// trick (ram.js), irrelevant here since mem.c stores genuine
// big-endian bytes already in the file's natural order.

// Loads path into ram[] starting at address 0. Returns the byte
// length loaded, or -1 on error.
int load_kernel(char *path);

// Scans the first `length` bytes of ram[] for the DTB's "memory\0"
// property (system.js's exact byte-offset/value guard, replicated
// verbatim) and overwrites its size cell to memorysize_mb megabytes.
void patch_kernel(int length, int memorysize_mb);
