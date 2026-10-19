//
// libarc: archives for C4IX -- gzip (deflate) and tar.
//
// Link a program against libarc.c4l as well as libc4ix.c4l. Used by
// vfsload, which unpacks the root archive at boot, and by tar and gunzip.
//
// Every archive this reads is a whole file in memory: the inflater
// writes into one buffer the size the gzip trailer gives, and the tar
// reader walks that buffer. Simple, and C4IX's files are RAM anyway.
//

#ifndef __C4IX_ARCHIVE_H
#define __C4IX_ARCHIVE_H 1

// what arc_extract does with each entry
enum { ARC_LIST = 0, ARC_EXTRACT = 1 };
// verbosity: 0 quiet, 1 one line per file, 2 also directories
enum { ARC_QUIET = 0, ARC_VERBOSE = 1 };

// The error text of the last failure, or 0.
extern char *arc_error;

// CRC-32 (IEEE, as gzip uses it) of LEN bytes, continuing from CRC
// (start at 0). The result is the low 32 bits at either word size.
int arc_crc32(int crc, char *buf, int len);

// Inflate a gzip member. On success *OUT is a fresh buffer of *OUTLEN
// bytes, CRC-checked, and 0 is returned; on failure, -1 and arc_error.
int arc_gunzip(char *in, int inlen, char **out, int *outlen);

// Raw deflate data into OUT, which holds at most OUTCAP bytes.
// Returns the bytes written, or -1. *USED gets the input consumed.
int arc_inflate(char *in, int inlen, char *out, int outcap, int *used);

// Walk a tar image (ustar, or this repo's C4TAR1) held in memory.
// MODE ARC_LIST prints names; ARC_EXTRACT writes each file under DEST,
// making directories as it goes. Returns the file count, or -1.
int arc_untar(char *buf, int len, char *dest, int mode, int verbose);

// Read a whole file into memory: *PLEN bytes, or 0 on failure.
char *arc_readfile(char *path, int *plen);

// Open PATH and do what it needs: a gzip member is inflated first,
// then the tar inside is listed or extracted. Returns files, or -1.
int arc_extract(char *path, char *dest, int mode, int verbose);

// mkdir -p: every directory along PATH. 0 on success.
int arc_mkdirs(char *path);

#endif // __C4IX_ARCHIVE_H
