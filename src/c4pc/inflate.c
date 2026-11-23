/* inflate.c -- gzip (RFC 1952) holding DEFLATE (RFC 1951), for the boot
 * disk's compressed files (drive.c, pc/packdisk.py): a whole member at a
 * time, into a buffer of the size its trailer gives. Stored, fixed and
 * dynamic blocks; canonical Huffman codes decoded a bit at a time, which
 * is simple and fast enough for files read once and kept (some tens of MB
 * a second). The trailer's CRC-32 is checked. */
#include "c4pc.h"
#include "clib.h"

typedef struct { const u8 *in; u32 inlen, ip; u32 bb; int bc; u8 *out; u32 outlen, op; } inf;
/* a code: how many of each length, and the symbols in code order */
typedef struct { short count[16]; short sym[320]; } huff;

/* N bits (to 16), the first in the low bit; -1 past the end of the input */
static int bits(inf *s, int n) {
  u32 v = s->bb;
  while (s->bc < n) {
    if (s->ip >= s->inlen) return -1;
    v |= (u32)s->in[s->ip++] << s->bc;
    s->bc += 8;
  }
  s->bb = v >> n;
  s->bc -= n;
  return (int)(v & ((1u << n) - 1));
}

/* the code whose N lengths are LEN: 0 complete, > 0 incomplete, < 0 too
 * many codes of some length (not a code) */
static int build(huff *h, const u8 *len, int n) {
  short offs[16];
  int left = 1;
  for (int l = 0; l < 16; l++) h->count[l] = 0;
  for (int i = 0; i < n; i++) h->count[len[i]]++;
  if (h->count[0] == n) return 0;
  for (int l = 1; l < 16; l++) { left <<= 1; left -= h->count[l]; if (left < 0) return left; }
  offs[1] = 0;
  for (int l = 1; l < 15; l++) offs[l + 1] = (short)(offs[l] + h->count[l]);
  for (int i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (short)i;
  return left;
}
/* the next symbol in code H; -1 if the input ends or no code matches */
static int decode(inf *s, const huff *h) {
  int code = 0, first = 0, index = 0;
  for (int l = 1; l < 16; l++) {
    int b = bits(s, 1);
    if (b < 0) return -1;
    code |= b;
    int c = h->count[l];
    if (code - c < first) return h->sym[index + (code - first)];
    index += c; first += c;
    first <<= 1; code <<= 1;
  }
  return -1;
}

static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
  257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

/* a block's literals and matches, to its end code */
static int codes(inf *s, const huff *lit, const huff *dist) {
  for (;;) {
    int sym = decode(s, lit);
    if (sym < 0) return -1;
    if (sym < 256) {
      if (s->op >= s->outlen) return -1;
      s->out[s->op++] = (u8)sym;
      continue;
    }
    if (sym == 256) return 0;
    sym -= 257;
    if (sym >= 29) return -1;
    int e = bits(s, lext[sym]);
    if (e < 0) return -1;
    u32 len = (u32)(lbase[sym] + e);
    int ds = decode(s, dist);
    if (ds < 0 || ds >= 30) return -1;
    e = bits(s, dext[ds]);
    if (e < 0) return -1;
    u32 d = (u32)(dbase[ds] + e);
    if (d > s->op || len > s->outlen - s->op) return -1;
    /* byte by byte: a match may overlap what it copies */
    u8 *o = s->out + s->op, *from = o - d;
    for (u32 i = 0; i < len; i++) o[i] = from[i];
    s->op += len;
  }
}

static int stored(inf *s) {
  s->bb = 0; s->bc = 0;                      /* to the byte */
  if (s->ip + 4 > s->inlen) return -1;
  u32 len = s->in[s->ip] | (u32)s->in[s->ip + 1] << 8;
  u32 nlen = s->in[s->ip + 2] | (u32)s->in[s->ip + 3] << 8;
  s->ip += 4;
  if (len != (~nlen & 0xffff) || len > s->inlen - s->ip || len > s->outlen - s->op) return -1;
  memcpy(s->out + s->op, s->in + s->ip, len);
  s->ip += len; s->op += len;
  return 0;
}

static int fixed(inf *s) {
  static huff lit, dist;
  static int made;
  if (!made) {
    u8 l[288];
    int i = 0;
    for (; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    build(&lit, l, 288);
    for (i = 0; i < 30; i++) l[i] = 5;
    build(&dist, l, 30);
    made = 1;
  }
  return codes(s, &lit, &dist);
}

static int dynamic(inf *s) {
  static const u8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
  u8 len[320];
  huff lc, lit, dist;
  int nlen = bits(s, 5), ndist = bits(s, 5), ncode = bits(s, 4);
  if (nlen < 0 || ndist < 0 || ncode < 0) return -1;
  nlen += 257; ndist += 1; ncode += 4;
  if (nlen > 286 || ndist > 30) return -1;
  int i;
  for (i = 0; i < 19; i++) len[i] = 0;
  for (i = 0; i < ncode; i++) { int b = bits(s, 3); if (b < 0) return -1; len[order[i]] = (u8)b; }
  if (build(&lc, len, 19) != 0) return -1;
  i = 0;
  while (i < nlen + ndist) {
    int sym = decode(s, &lc);
    if (sym < 0) return -1;
    if (sym < 16) { len[i++] = (u8)sym; continue; }
    int rep, val = 0;
    if (sym == 16) { if (!i) return -1; val = len[i - 1]; rep = bits(s, 2); if (rep < 0) return -1; rep += 3; }
    else if (sym == 17) { rep = bits(s, 3); if (rep < 0) return -1; rep += 3; }
    else { rep = bits(s, 7); if (rep < 0) return -1; rep += 11; }
    if (i + rep > nlen + ndist) return -1;
    while (rep--) len[i++] = (u8)val;
  }
  if (!len[256]) return -1;                  /* no end code */
  int e = build(&lit, len, nlen);
  if (e < 0 || (e > 0 && nlen - lit.count[0] != 1)) return -1;
  e = build(&dist, len + nlen, ndist);
  if (e < 0 || (e > 0 && ndist - dist.count[0] != 1)) return -1;
  return codes(s, &lit, &dist);
}

static u32 crc_tab[256];
static u32 crc32_of(const u8 *p, u32 n) {
  if (!crc_tab[1]) for (u32 i = 0; i < 256; i++) { u32 c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_tab[i] = c; }
  u32 c = 0xffffffffu;
  for (u32 i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 255] ^ (c >> 8);
  return c ^ 0xffffffffu;
}

/* a gzip member's size when inflated (its trailer's ISIZE): 0, or -1 if Z
 * is not one */
int gz_size(const u8 *z, u32 n, u32 *size) {
  if (n < 18 || z[0] != 0x1f || z[1] != 0x8b || z[2] != 8) return -1;
  *size = z[n - 4] | (u32)z[n - 3] << 8 | (u32)z[n - 2] << 16 | (u32)z[n - 1] << 24;
  return 0;
}
/* the gzip member Z (N bytes) inflated into OUT, exactly LEN bytes: 0, or -1 */
int gz_inflate(const u8 *z, u32 n, u8 *out, u32 len) {
  if (n < 18 || z[0] != 0x1f || z[1] != 0x8b || z[2] != 8) return -1;
  int flg = z[3];
  u32 p = 10;
  if (flg & 4) { if (p + 2 > n) return -1; p += 2 + (z[p] | (u32)z[p + 1] << 8); }
  if (flg & 8) { while (p < n && z[p]) p++; p++; }
  if (flg & 16) { while (p < n && z[p]) p++; p++; }
  if (flg & 2) p += 2;
  if (p + 8 > n) return -1;
  inf S = { z + p, n - 8 - p, 0, 0, 0, out, len, 0 };
  int last;
  do {
    last = bits(&S, 1);
    int type = bits(&S, 2);
    int r = -1;
    if (last < 0 || type < 0) return -1;
    if (type == 0) r = stored(&S);
    else if (type == 1) r = fixed(&S);
    else if (type == 2) r = dynamic(&S);
    if (r) return -1;
  } while (!last);
  if (S.op != len) return -1;
  u32 crc = z[n - 8] | (u32)z[n - 7] << 8 | (u32)z[n - 6] << 16 | (u32)z[n - 5] << 24;
  return crc32_of(out, len) == crc ? 0 : -1;
}
