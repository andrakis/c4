// C4IX userland: crc32 -- the CRC-32 and size of each file.
//
//   crc32 FILE ...      one "CRC SIZE NAME" line each, the CRC as 8 hex digits
//
// The same CRC gzip uses (IEEE), so a host can check what a build
// inside the machine made: python3 -c 'import zlib,sys;
// print("%08x" % zlib.crc32(open(sys.argv[1],"rb").read()))' FILE.
// Links libarc for arc_crc32 and arc_readfile.

#include "c4ix_user.h"
#include "archive.h"

static void hex8(int v) {
    char out[9];
    int i, d;
    i = 7;
    while (i >= 0) {
        d = v & 15;
        out[i] = d < 10 ? '0' + d : 'a' + d - 10;
        v = (v >> 4) & 0x0FFFFFFF;
        --i;
    }
    out[8] = 0;
    uprintf("%s", out);
}

int main(int argc, char **argv) {
    char *buf;
    int i, len, bad, st[4];
    if (argc < 2) { ufprintf(STDERR, "usage: crc32 FILE ...\n"); return 2; }
    bad = 0;
    i = 1;
    while (i < argc) {
        // An absolute path must be in the VFS: the board's disk answers
        // a path it lacks with the file of the same base name, which
        // would checksum the disk's copy instead of the one made here.
        if (argv[i][0] == '/' && ustat(argv[i], st) < 0) {
            ufprintf(STDERR, "crc32: no such file: %s\n", argv[i]);
            bad = 1; ++i; continue;
        }
        buf = arc_readfile(argv[i], &len);
        if (!buf) { ufprintf(STDERR, "crc32: cannot read %s\n", argv[i]); bad = 1; }
        else {
            hex8(arc_crc32(0, buf, len));
            uprintf(" %d %s\n", len, argv[i]);
            free(buf);
        }
        ++i;
    }
    return bad;
}
