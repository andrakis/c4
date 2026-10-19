//
// libarc: gzip and tar for C4IX. archive.h has the interface.
//
// The inflater is RFC 1951 the way zlib's puff.c reads it: canonical
// Huffman tables built from code lengths, decoded a bit at a time --
// with one addition, a 9-bit lookup table in front of each code, so
// most symbols cost one table read instead of up to fifteen loop turns.
// That matters here: the root archive is megabytes and every
// instruction is interpreted.
//
// Word size. Nothing below may depend on whether int is 32 or 64 bits,
// and this VM has no unsigned type. The bit buffer never holds more
// than 23 bits, so its shifts are safe either way. The CRC keeps its
// value masked to 32 bits after every step, so a 64-bit host computes
// the same number a 32-bit one does; the shift that would drag a sign
// bit in is masked off.
//

#include "c4ix_user.h"
#include "archive.h"

char *arc_error;

enum {
    MAXBITS = 15,           // longest code deflate allows
    MAXLCODES = 286,        // literal/length codes
    MAXDCODES = 30,         // distance codes
    FIXLCODES = 288,        // literal/length codes in the fixed table
    FASTBITS = 9,           // lookup-table width
    FASTSIZE = 512,         // 1 << FASTBITS
    TARBLOCK = 512
};

// ---- CRC-32 ----

int *crc_table;

static void crc_init() {
    int n, k, c;
    crc_table = (int *)ualloc(256 * sizeof(int));
    n = 0;
    while (n < 256) {
        c = n;
        k = 0;
        while (k < 8) {
            // c >> 1 as an unsigned shift: mask off what an arithmetic
            // shift would have copied down from the sign bit
            if (c & 1) c = 0xEDB88320 ^ ((c >> 1) & 0x7FFFFFFF);
            else c = (c >> 1) & 0x7FFFFFFF;
            ++k;
        }
        crc_table[n] = c & 0xFFFFFFFF;
        ++n;
    }
}

int arc_crc32(int crc, char *buf, int len) {
    int *t;
    char *e;
    if (!crc_table) crc_init();
    t = crc_table;
    e = buf + len;
    crc = (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF;
    while (buf < e) crc = t[(crc ^ *buf++) & 255] ^ ((crc >> 8) & 0x00FFFFFF);
    return (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF;
}

// ---- the inflater's state ----

char *in_buf;  int in_len;  int in_pos;
char *out_buf; int out_cap; int out_pos;
int bitbuf; int bitcnt;
int in_short;              // set when the input ran out mid-stream

// The lengths and distances: base values and extra bits, RFC 1951 3.2.5.
int lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
int lext[29]  = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
int dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                  257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                  8193, 12289, 16385, 24577 };
int dext[30]  = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                  7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
// the order code-length code lengths arrive in, 3.2.7
int clorder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

// A Huffman table: COUNT[len] codes of each length, SYMBOL the symbols
// in canonical order, FAST[the next 9 stream bits] = symbol * 16 + its
// length for every code of 9 bits or fewer (0 where none fits).
struct huff {
    int *count;
    int *symbol;
    int *fast;
};

struct huff *lencode;
struct huff *distcode;
struct huff *fixlen;
struct huff *fixdist;
int *lengths;              // scratch: code lengths being read

static struct huff *huff_new(int nsym) {
    struct huff *h;
    h = (struct huff *)ualloc(sizeof(struct huff));
    h->count = (int *)ualloc((MAXBITS + 1) * sizeof(int));
    h->symbol = (int *)ualloc(nsym * sizeof(int));
    h->fast = (int *)ualloc(FASTSIZE * sizeof(int));
    return h;
}

static int arc_fail(char *why) {
    arc_error = why;
    return 0 - 1;
}

// The next NEED bits, low bit first. NEED is at most 16.
static int bits(int need) {
    int v;
    while (bitcnt < need) {
        if (in_pos >= in_len) { in_short = 1; return 0; }
        bitbuf = bitbuf | ((in_buf[in_pos++] & 255) << bitcnt);
        bitcnt = bitcnt + 8;
    }
    v = bitbuf & ((1 << need) - 1);
    bitbuf = bitbuf >> need;
    bitcnt = bitcnt - need;
    return v;
}

// Build H from LEN[0..N-1]. Returns 0 for a complete code, a positive
// number for an incomplete one (allowed for a single distance code),
// and -1 for an over-subscribed one, which no encoder may write.
static int construct(struct huff *h, int *len, int n) {
    int sym, l, left, code, r, j, k;
    int offs[16];
    int next[16];

    l = 0;
    while (l <= MAXBITS) { h->count[l] = 0; ++l; }
    sym = 0;
    while (sym < n) { h->count[len[sym]] = h->count[len[sym]] + 1; ++sym; }
    k = 0;
    while (k < FASTSIZE) { h->fast[k] = 0; ++k; }
    if (h->count[0] == n) return 0;          // no codes: complete, but useless

    left = 1;
    l = 1;
    while (l <= MAXBITS) {
        left = left << 1;
        left = left - h->count[l];
        if (left < 0) return left;
        ++l;
    }

    offs[1] = 0;
    l = 1;
    while (l < MAXBITS) { offs[l + 1] = offs[l] + h->count[l]; ++l; }
    sym = 0;
    while (sym < n) {
        if (len[sym] != 0) { h->symbol[offs[len[sym]]] = sym; offs[len[sym]] = offs[len[sym]] + 1; }
        ++sym;
    }

    // canonical codes, for the lookup table: the stream sends each code
    // high bit first, so the table is indexed by the code reversed
    code = 0;
    next[1] = 0;
    l = 1;
    while (l < MAXBITS) { code = (code + h->count[l]) << 1; next[l + 1] = code; ++l; }
    sym = 0;
    while (sym < n) {
        l = len[sym];
        if (l != 0) {
            code = next[l];
            next[l] = code + 1;
            if (l <= FASTBITS) {
                r = 0; j = 0;
                while (j < l) { r = (r << 1) | ((code >> j) & 1); ++j; }
                while (r < FASTSIZE) { h->fast[r] = sym * 16 + l; r = r + (1 << l); }
            }
        }
        ++sym;
    }
    return left;
}

// One symbol from H.
static int decode(struct huff *h) {
    int e, l, code, first, count, index;

    // top up to FASTBITS without failing: the stream may end sooner
    while (bitcnt < FASTBITS && in_pos < in_len) {
        bitbuf = bitbuf | ((in_buf[in_pos++] & 255) << bitcnt);
        bitcnt = bitcnt + 8;
    }
    e = h->fast[bitbuf & (FASTSIZE - 1)];
    if (e && (e & 15) <= bitcnt) {
        l = e & 15;
        bitbuf = bitbuf >> l;
        bitcnt = bitcnt - l;
        return e >> 4;
    }

    // the slow path: a long code, a bit at a time
    code = 0; first = 0; index = 0;
    l = 1;
    while (l <= MAXBITS) {
        code = code | bits(1);
        if (in_short) return 0 - 1;
        count = h->count[l];
        if (code - count < first) return h->symbol[index + (code - first)];
        index = index + count;
        first = first + count;
        first = first << 1;
        code = code << 1;
        ++l;
    }
    return 0 - 10;                           // ran out of codes
}

// Literals and matches until end-of-block, with LC and DC.
//
// This loop is where the time goes, so the bit buffer lives in locals
// here and the lookup table is read inline; the globals are written
// back only around the slow path and on the way out. A literal costs
// a table read and a store. A match that does not overlap itself is
// one memcpy, which the host does.
static int codes(struct huff *lc, struct huff *dc) {
    int sym, len, dist, e, l, bb, bc, ip, il, op, oc;
    int *lfast, *dfast;
    char *ib, *ob, *from;

    lfast = lc->fast; dfast = dc->fast;
    ib = in_buf; il = in_len; ob = out_buf; oc = out_cap;
    bb = bitbuf; bc = bitcnt; ip = in_pos; op = out_pos;
    while (1) {
        // a literal/length symbol
        if (bc < 16 && ip + 2 <= il) {
            bb = bb | ((ib[ip] & 255) << bc) | ((ib[ip + 1] & 255) << (bc + 8));
            ip = ip + 2; bc = bc + 16;
        }
        e = lfast[bb & (FASTSIZE - 1)];
        if (e && (l = e & 15) <= bc) {
            bb = bb >> l; bc = bc - l;
            sym = e >> 4;
        } else {
            bitbuf = bb; bitcnt = bc; in_pos = ip;
            sym = decode(lc);
            bb = bitbuf; bc = bitcnt; ip = in_pos;
            if (sym < 0) return arc_fail("bad literal/length code");
        }
        if (sym < 256) {
            if (op >= oc) return arc_fail("output overflows its size");
            ob[op++] = sym;
            continue;
        }
        if (sym == 256) break;

        sym = sym - 257;
        if (sym >= 29) return arc_fail("bad length symbol");
        len = lbase[sym];
        if ((l = lext[sym])) {
            bitbuf = bb; bitcnt = bc; in_pos = ip;
            len = len + bits(l);
            bb = bitbuf; bc = bitcnt; ip = in_pos;
        }

        // its distance
        if (bc < 16 && ip + 2 <= il) {
            bb = bb | ((ib[ip] & 255) << bc) | ((ib[ip + 1] & 255) << (bc + 8));
            ip = ip + 2; bc = bc + 16;
        }
        e = dfast[bb & (FASTSIZE - 1)];
        if (e && (l = e & 15) <= bc) {
            bb = bb >> l; bc = bc - l;
            sym = e >> 4;
        } else {
            bitbuf = bb; bitcnt = bc; in_pos = ip;
            sym = decode(dc);
            bb = bitbuf; bc = bitcnt; ip = in_pos;
        }
        if (sym < 0 || sym >= 30) return arc_fail("bad distance symbol");
        dist = dbase[sym];
        if ((l = dext[sym])) {
            bitbuf = bb; bitcnt = bc; in_pos = ip;
            dist = dist + bits(l);
            bb = bitbuf; bc = bitcnt; ip = in_pos;
        }
        if (in_short) return arc_fail("input ends mid-block");
        if (dist > op) return arc_fail("distance reaches before the start");
        if (op + len > oc) return arc_fail("output overflows its size");
        from = ob + op - dist;
        if (dist >= len) {
            memcpy(ob + op, from, len);
            op = op + len;
        } else {
            // it overlaps the bytes it is writing: byte by byte
            while (len > 0) { ob[op++] = *from++; --len; }
        }
    }
    bitbuf = bb; bitcnt = bc; in_pos = ip; out_pos = op;
    return 0;
}

static int stored() {
    int len, nlen;
    // drop to a byte boundary, then hand back any whole bytes the bit
    // buffer had already read ahead
    bitbuf = 0;
    in_pos = in_pos - (bitcnt >> 3);
    bitcnt = 0;
    if (in_pos + 4 > in_len) return arc_fail("input ends in a stored header");
    len = (in_buf[in_pos] & 255) | ((in_buf[in_pos + 1] & 255) << 8);
    nlen = (in_buf[in_pos + 2] & 255) | ((in_buf[in_pos + 3] & 255) << 8);
    in_pos = in_pos + 4;
    if (len != (0xFFFF ^ nlen)) return arc_fail("stored block length check");
    if (in_pos + len > in_len) return arc_fail("input ends in a stored block");
    if (out_pos + len > out_cap) return arc_fail("output overflows its size");
    memcpy(out_buf + out_pos, in_buf + in_pos, len);
    out_pos = out_pos + len;
    in_pos = in_pos + len;
    return 0;
}

static int fixed() {
    int sym;
    if (!fixlen) {
        fixlen = huff_new(FIXLCODES);
        fixdist = huff_new(MAXDCODES);
        sym = 0;
        while (sym < 144) lengths[sym++] = 8;
        while (sym < 256) lengths[sym++] = 9;
        while (sym < 280) lengths[sym++] = 7;
        while (sym < FIXLCODES) lengths[sym++] = 8;
        construct(fixlen, lengths, FIXLCODES);
        sym = 0;
        while (sym < MAXDCODES) lengths[sym++] = 5;
        construct(fixdist, lengths, MAXDCODES);
    }
    return codes(fixlen, fixdist);
}

static int dynamic() {
    int nlen, ndist, ncode, index, err, sym, len, rep;

    nlen = bits(5) + 257;
    ndist = bits(5) + 1;
    ncode = bits(4) + 4;
    if (in_short) return arc_fail("input ends in a block header");
    if (nlen > MAXLCODES || ndist > MAXDCODES) return arc_fail("bad code counts");

    index = 0;
    while (index < ncode) { lengths[clorder[index]] = bits(3); ++index; }
    while (index < 19) { lengths[clorder[index]] = 0; ++index; }
    if (construct(lencode, lengths, 19) != 0) return arc_fail("bad code-length code");

    index = 0;
    while (index < nlen + ndist) {
        sym = decode(lencode);
        if (sym < 0) return arc_fail("bad code-length symbol");
        if (sym < 16) {
            lengths[index++] = sym;
        } else {
            len = 0;
            if (sym == 16) {
                if (index == 0) return arc_fail("repeat with nothing before it");
                len = lengths[index - 1];
                rep = 3 + bits(2);
            } else if (sym == 17) rep = 3 + bits(3);
            else rep = 11 + bits(7);
            if (index + rep > nlen + ndist) return arc_fail("too many lengths");
            while (rep > 0) { lengths[index++] = len; --rep; }
        }
    }
    if (lengths[256] == 0) return arc_fail("no end-of-block code");

    err = construct(lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode->count[0] != 1)) return arc_fail("bad literal/length lengths");
    err = construct(distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode->count[0] != 1)) return arc_fail("bad distance lengths");
    return codes(lencode, distcode);
}

int arc_inflate(char *in, int inlen, char *out, int outcap, int *used) {
    int last, type, err;

    if (!lengths) {
        lengths = (int *)ualloc((MAXLCODES + MAXDCODES) * sizeof(int));
        lencode = huff_new(MAXLCODES);
        distcode = huff_new(MAXDCODES);
    }
    in_buf = in; in_len = inlen; in_pos = 0;
    out_buf = out; out_cap = outcap; out_pos = 0;
    bitbuf = 0; bitcnt = 0; in_short = 0;
    arc_error = 0;

    last = 0;
    while (!last) {
        last = bits(1);
        type = bits(2);
        if (in_short) return arc_fail("input ends before a block");
        if (type == 0) err = stored();
        else if (type == 1) err = fixed();
        else if (type == 2) err = dynamic();
        else err = arc_fail("bad block type");
        if (err < 0) return err;
    }
    // whole bytes still in the bit buffer were not consumed
    *used = in_pos - (bitcnt >> 3);
    return out_pos;
}

// ---- gzip, RFC 1952 ----

static int le32(char *p) {
    return ((p[0] & 255) | ((p[1] & 255) << 8) | ((p[2] & 255) << 16) | ((p[3] & 255) << 24)) & 0xFFFFFFFF;
}

int arc_gunzip(char *in, int inlen, char **out, int *outlen) {
    int flags, pos, size, n, used, crc;
    char *buf;

    if (inlen < 18 || (in[0] & 255) != 0x1F || (in[1] & 255) != 0x8B) return arc_fail("not gzip");
    if (in[2] != 8) return arc_fail("gzip, but not deflate");
    flags = in[3] & 255;
    pos = 10;
    if (flags & 4) pos = pos + 2 + ((in[pos] & 255) | ((in[pos + 1] & 255) << 8));   // FEXTRA
    if (flags & 8) { while (pos < inlen && in[pos]) ++pos; ++pos; }                  // FNAME
    if (flags & 16) { while (pos < inlen && in[pos]) ++pos; ++pos; }                 // FCOMMENT
    if (flags & 2) pos = pos + 2;                                                    // FHCRC
    if (pos >= inlen - 8) return arc_fail("gzip header runs off the end");

    // ISIZE is the size mod 2^32; nothing here is that big
    size = le32(in + inlen - 4);
    if (size < 0) return arc_fail("gzip member too large");
    buf = (char *)ualloc(size + 1);
    if (!buf) return arc_fail("out of memory for the inflated data");
    n = arc_inflate(in + pos, inlen - pos - 8, buf, size, &used);
    if (n < 0) { free(buf); return n; }
    if (n != size) { free(buf); return arc_fail("inflated size differs from the trailer"); }
    crc = arc_crc32(0, buf, n);
    if (crc != le32(in + inlen - 8)) { free(buf); return arc_fail("CRC mismatch: the archive is damaged"); }
    *out = buf;
    *outlen = n;
    return 0;
}

// ---- files ----

char *arc_readfile(char *path, int *plen) {
    int st[4], fd, got, n;
    char *buf;
    if (ustat(path, st) < 0 || st[0] != 2) {
        // not in the VFS: the host may still have it (uopen falls back)
        st[1] = 0;
    }
    fd = uopen(path, O_RD);
    if (fd < 0) { arc_error = "cannot open"; return 0; }
    if (st[1] <= 0) st[1] = 65536;
    buf = (char *)ualloc(st[1] + 1);
    got = 0;
    while (1) {
        if (got == st[1]) {
            // bigger than stat said (a host file): grow
            char *nb;
            nb = (char *)ualloc(st[1] * 2 + 1);
            memcpy(nb, buf, got);
            free(buf);
            buf = nb;
            st[1] = st[1] * 2;
        }
        n = uread(fd, buf + got, st[1] - got);
        if (n <= 0) break;
        got = got + n;
    }
    uclose(fd);
    *plen = got;
    return buf;
}

int arc_mkdirs(char *path) {
    char part[256];
    int i, st[4];
    i = 0;
    while (path[i] && i < 255) {
        part[i] = path[i];
        if (path[i] == '/' && i > 0) {
            part[i] = 0;
            if (ustat(part, st) < 0) umkdir(part);
            part[i] = '/';
        }
        ++i;
    }
    part[i] = 0;
    if (i > 0 && ustat(part, st) < 0) umkdir(part);
    return ustat(part, st) == 0 && st[0] == 1 ? 0 : 0 - 1;
}

static int octal(char *p, int n) {
    int v, i;
    v = 0; i = 0;
    while (i < n && (p[i] == ' ' || p[i] == 0)) ++i;
    while (i < n && p[i] >= '0' && p[i] <= '7') { v = v * 8 + (p[i] - '0'); ++i; }
    return v;
}

// DEST + "/" + NAME, with any "./" in front of NAME dropped
static void dest_path(char *out, char *dest, char *name) {
    int o, i;
    o = 0;
    i = 0;
    while (dest[i] && o < 250) out[o++] = dest[i++];
    if (o > 0 && out[o - 1] == '/') --o;
    while (name[0] == '.' && name[1] == '/') name = name + 2;
    while (name[0] == '/') ++name;
    out[o++] = '/';
    i = 0;
    while (name[i] && o < 255) out[o++] = name[i++];
    // a directory entry ends in '/'; mkdir does not want it
    while (o > 1 && out[o - 1] == '/') --o;
    out[o] = 0;
}

static int put_file(char *path, char *data, int len) {
    int fd, n, slash, i;
    char dir[256];
    slash = 0 - 1;
    i = 0;
    while (path[i]) { if (path[i] == '/') slash = i; ++i; }
    if (slash > 0) {
        memcpy(dir, path, slash);
        dir[slash] = 0;
        arc_mkdirs(dir);
    }
    fd = uopen(path, O_WR + O_CREATE + O_TRUNCATE);
    if (fd < 0) return 0 - 1;
    n = len > 0 ? write(fd, data, len) : 0;
    uclose(fd);
    return n == len ? 0 : 0 - 1;
}

// one entry: list it, or write it out
static int entry(char *dest, char *name, int isdir, char *data, int size, int mode, int verbose) {
    char path[256];
    dest_path(path, dest, name);
    if (mode == ARC_LIST) {
        uprintf("%s%s %d\n", name, isdir ? "/" : "", size);
        return 0;
    }
    if (isdir) {
        if (verbose > 1) uprintf("%s/\n", path);
        return arc_mkdirs(path);
    }
    if (verbose) uprintf("%s\n", path);
    if (put_file(path, data, size) < 0) {
        ufprintf(STDERR, "tar: cannot write %s\n", path);
        return 0 - 1;
    }
    return 0;
}

static int is_c4tar(char *buf, int len) {
    return len >= 7 && buf[0] == 'C' && buf[1] == '4' && buf[2] == 'T' && buf[3] == 'A' &&
           buf[4] == 'R' && buf[5] == '1' && buf[6] == 10;
}

// C4TAR1 (Makefile c4tar_build): "C4TAR1\n", then "NAME LEN\n" and LEN bytes, repeated
static int untar_c4(char *buf, int len, char *dest, int mode, int verbose) {
    int pos, start, sp, size, files;
    char name[256];
    pos = 7; files = 0;
    while (pos < len) {
        start = pos;
        while (pos < len && buf[pos] != 10) ++pos;
        if (pos >= len) break;
        sp = pos - 1;
        while (sp > start && buf[sp] != ' ') --sp;
        if (sp <= start || sp - start > 255) return arc_fail("bad C4TAR1 header line");
        memcpy(name, buf + start, sp - start);
        name[sp - start] = 0;
        size = 0;
        ++sp;
        while (sp < pos) { size = size * 10 + (buf[sp] - '0'); ++sp; }
        ++pos;
        if (pos + size > len) return arc_fail("C4TAR1 entry runs off the end");
        if (entry(dest, name, 0, buf + pos, size, mode, verbose) < 0) return 0 - 1;
        ++files;
        pos = pos + size;
    }
    return files;
}

int arc_untar(char *buf, int len, char *dest, int mode, int verbose) {
    int pos, size, files, type, n, i;
    char name[256];
    char *h;

    arc_error = 0;
    if (is_c4tar(buf, len)) return untar_c4(buf, len, dest, mode, verbose);
    if (len < TARBLOCK || buf[257] != 'u' || buf[258] != 's' || buf[259] != 't') return arc_fail("not a tar archive");

    pos = 0; files = 0;
    while (pos + TARBLOCK <= len) {
        h = buf + pos;
        if (h[0] == 0) break;                         // the zero blocks at the end
        // ustar: PREFIX "/" NAME, when there is a prefix
        n = 0;
        if (h[345]) {
            i = 0;
            while (i < 155 && h[345 + i]) name[n++] = h[345 + i++];
            name[n++] = '/';
        }
        i = 0;
        while (i < 100 && h[i]) name[n++] = h[i++];
        name[n] = 0;
        size = octal(h + 124, 12);
        type = h[156];
        pos = pos + TARBLOCK;
        if (pos + size > len) return arc_fail("tar entry runs off the end");
        if (type == '0' || type == 0 || type == '7') {
            if (entry(dest, name, 0, buf + pos, size, mode, verbose) < 0) return 0 - 1;
            ++files;
        } else if (type == '5') {
            if (entry(dest, name, 1, 0, 0, mode, verbose) < 0) return 0 - 1;
        }
        // links, devices and pax headers have nothing C4IX can hold
        pos = pos + ((size + TARBLOCK - 1) / TARBLOCK) * TARBLOCK;
    }
    return files;
}

int arc_extract(char *path, char *dest, int mode, int verbose) {
    char *raw, *tarbuf;
    int rawlen, tarlen, files;

    raw = arc_readfile(path, &rawlen);
    if (!raw) return 0 - 1;
    if (rawlen >= 2 && (raw[0] & 255) == 0x1F && (raw[1] & 255) == 0x8B) {
        if (arc_gunzip(raw, rawlen, &tarbuf, &tarlen) < 0) { free(raw); return 0 - 1; }
        free(raw);
    } else {
        tarbuf = raw;
        tarlen = rawlen;
    }
    files = arc_untar(tarbuf, tarlen, dest, mode, verbose);
    free(tarbuf);
    return files;
}
