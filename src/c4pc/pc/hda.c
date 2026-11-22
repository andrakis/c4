/* hda.c -- Intel High Definition Audio under the machine's sound card
 * (snd.c), where there is no Sound Blaster 16: VMware's default sound
 * device, VirtualBox's, QEMU's -device intel-hda -device hda-output, and
 * most PCs since 2004. As sb16.c: 16-bit stereo at 44100 Hz round a ring
 * the controller reads by DMA, its two halves a buffer descriptor list of
 * two, and hda_poll mixing the ring FILL frames past where the stream's
 * link position (LPIB) says it is. No interrupt.
 *
 * The controller: the first PCI function of class 04h, subclass 03h, its
 * registers at BAR0 (below 4 GB, which c4pc maps to itself). Reset, then
 * the codecs that answer (STATESTS), each spoken to through the CORB and
 * the RIRB (rings of verbs and of their answers). In the first codec's
 * audio function group every pin that can output and is connected (its
 * default configuration: a line out, a speaker or headphones) is walked
 * back through its connections to a DAC; every node on the way is
 * powered, its connection chosen, its amplifiers unmuted at their
 * default gain (0 dB where the steps say so), and every such DAC put on
 * the one stream. */
#include "../c4pc.h"
#include "../clib.h"
#include "../snd.h"

static inline void outl(u32 p, u32 v) { __asm__ volatile("outl %0,%w1" : : "a"(v), "Nd"(p)); }
static inline u32 inl(u32 p) { u32 v; __asm__ volatile("inl %w1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline u8 inb(u32 p) { u8 v; __asm__ volatile("inb %w1,%0" : "=a"(v) : "Nd"(p)); return v; }

#define RATE 44100
#define RING 8192                      /* frames round the ring: 186 ms */
#define FILL 2048                      /* frames mixed ahead of the DMA: 46 ms */
static int16_t ring[RING * 2] __attribute__((aligned(128)));
static u32 bdl[8] __attribute__((aligned(128)));     /* two entries of 16 bytes */
static u32 corb[256] __attribute__((aligned(128)));
static u32 rirb[512] __attribute__((aligned(128)));
static volatile u8 *R;                 /* the controller's registers */
static volatile u8 *SD;                /* the output stream's descriptor */
static u32 corb_wp, rirb_rp;
static u32 played, made, last_pos;
static snd *card;
u32 hda_underruns;
void (*hda_log)(const char *s);   /* --debug-sound: the codec's graph, on the console */
static void logx(const char *what, u32 v) {
  if (!hda_log) return;
  char b[16]; const char *h = "0123456789abcdef"; int k = 0;
  for (int i = 28; i >= 0; i -= 4) b[k++] = h[(v >> i) & 15];
  b[k++] = ' '; b[k] = 0;
  hda_log(what); hda_log("="); hda_log(b);
}
const char *hda_why = "no HD Audio controller";   /* why hda_init gave up */

#define R8(o) (*(volatile u8 *)(R + (o)))
#define R16(o) (*(volatile u16 *)(R + (o)))
#define R32(o) (*(volatile u32 *)(R + (o)))

static u32 pci_rd(u32 bus, u32 dev, u32 fn, u32 off) {
  outl(0xCF8, 0x80000000u | bus << 16 | dev << 11 | fn << 8 | (off & 0xfc));
  return inl(0xCFC);
}
static void pci_wr(u32 bus, u32 dev, u32 fn, u32 off, u32 v) {
  outl(0xCF8, 0x80000000u | bus << 16 | dev << 11 | fn << 8 | (off & 0xfc));
  outl(0xCFC, v);
}
/* a microsecond or so: a read of an unused port */
static void udelay(u32 us) { for (u32 i = 0; i < us; i++) inb(0x80); }

/* a verb to codec cad, node nid; its answer, or -1 */
static i64 verb(u32 cad, u32 nid, u32 v) {
  corb_wp = (corb_wp + 1) & 255;
  corb[corb_wp] = cad << 28 | nid << 20 | v;
  R16(0x48) = (u16)corb_wp;
  for (u32 n = 0; n < 200000; n++) {
    u32 wp = R16(0x58) & 255;
    if (wp != rirb_rp) {
      rirb_rp = (rirb_rp + 1) & 255;
      R8(0x5D) = 5;                     /* RIRBSTS: a controller may stop at RINTCNT until it is cleared */
      return rirb[rirb_rp * 2];
    }
    udelay(1);
  }
  return -1;
}
static u32 param(u32 cad, u32 nid, u32 p) { i64 r = verb(cad, nid, 0xF0000 | p); return r < 0 ? 0 : (u32)r; }

/* ---- the codec's graph ---- */
#define MAXN 128
static u32 caps[MAXN], conn_n[MAXN]; static u8 conn[MAXN][16]; static u32 nstart, ncount;
static int wtype(u32 n) { return (int)((caps[n] >> 20) & 15); }   /* 0 out, 1 in, 2 mixer, 3 selector, 4 pin */
static int valid(u32 n) { return n >= nstart && n < nstart + ncount && n < MAXN; }

static void read_conns(u32 cad, u32 n) {
  conn_n[n] = 0;
  if (!(caps[n] & (1u << 8))) return;                 /* no connection list */
  u32 len = param(cad, n, 0x0E);
  if (len & 0x80) return;                             /* long form: none here have it */
  len &= 0x7f;
  if (len > 16) len = 16;
  for (u32 i = 0; i < len; i += 4) {
    i64 r = verb(cad, n, 0xF0200 | i);
    if (r < 0) break;
    for (u32 k = 0; k < 4 && i + k < len; k++) conn[n][i + k] = (u8)(r >> (8 * k));
  }
  conn_n[n] = len;
}
/* the path from node n back to a DAC: path[0] = n, ..., the DAC last */
static int find_dac(u32 n, u8 *path, int depth) {
  path[depth] = (u8)n;
  if (wtype(n) == 0) return depth + 1;
  if (depth >= 6) return 0;
  for (u32 i = 0; i < conn_n[n]; i++) {
    u32 c = conn[n][i];
    if (!valid(c)) continue;
    int t = wtype(c);
    if (t != 0 && t != 2 && t != 3) continue;
    int l = find_dac(c, path, depth + 1);
    if (l) { path[depth + 1] = (u8)c; return l; }
  }
  return 0;
}
/* an amplifier's gain at 0 dB where its steps say where that is, else its middle */
static u32 amp_gain(u32 ac) { u32 off = ac & 0x7f, steps = (ac >> 8) & 0x7f; return off <= steps ? off : steps / 2; }

static void unmute_path(u32 cad, u32 afg, const u8 *path, int len) {
  for (int i = 0; i < len; i++) {
    u32 n = path[i];
    verb(cad, n, 0x70500);                            /* power: D0 */
    if (i + 1 < len) {                                /* where it takes its input from */
      u32 next = path[i + 1], idx = 0;
      for (u32 k = 0; k < conn_n[n]; k++) if (conn[n][k] == next) idx = k;
      if (wtype(n) != 2) verb(cad, n, 0x70100 | idx); /* a mixer takes them all */
      if (caps[n] & (1u << 1)) {                      /* an input amplifier */
        u32 ac = (caps[n] & (1u << 3)) ? param(cad, n, 0x0D) : param(cad, afg, 0x0D);
        verb(cad, n, 0x30000 | 0x7000 | idx << 8 | amp_gain(ac));
      }
    }
    if (caps[n] & (1u << 2)) {                        /* an output amplifier */
      u32 ac = (caps[n] & (1u << 3)) ? param(cad, n, 0x12) : param(cad, afg, 0x12);
      verb(cad, n, 0x30000 | 0xB000 | amp_gain(ac));
    }
  }
}

static int codec_setup(u32 cad) {
  u32 sub = param(cad, 0, 0x04), fstart = (sub >> 16) & 0xff, fcount = sub & 0xff, afg = 0;
  for (u32 f = fstart; f < fstart + fcount; f++) if ((param(cad, f, 0x05) & 0xff) == 1) { afg = f; break; }
  if (!afg) return 0;
  verb(cad, afg, 0x70500);                            /* the group: D0 */
  sub = param(cad, afg, 0x04);
  nstart = (sub >> 16) & 0xff; ncount = sub & 0xff;
  if (nstart + ncount > MAXN) ncount = nstart < MAXN ? MAXN - nstart : 0;
  logx("afg", afg); logx("nodes", nstart << 16 | ncount); if (hda_log) hda_log("\n");
  for (u32 n = nstart; n < nstart + ncount; n++) {
    caps[n] = param(cad, n, 0x09); read_conns(cad, n);
    if (hda_log) {
      logx("node", n); logx("caps", caps[n]);
      for (u32 k = 0; k < conn_n[n]; k++) logx("<-", conn[n][k]);
      if (wtype(n) == 4) { logx("pincap", param(cad, n, 0x0C)); logx("cfg", (u32)verb(cad, n, 0xF1C00)); }
      hda_log("\n");
    }
  }
  int dacs = 0;
  for (u32 n = nstart; n < nstart + ncount; n++) {
    if (wtype(n) != 4) continue;
    if (!(param(cad, n, 0x0C) & (1u << 4))) continue;               /* cannot output */
    u32 cfg = (u32)verb(cad, n, 0xF1C00);
    u32 port = cfg >> 30, dev = (cfg >> 20) & 15;
    if (port == 1) continue;                                          /* no jack, nothing built in */
    if (dev != 0 && dev != 1 && dev != 2) continue;                   /* line out, speaker, headphones */
    u8 path[8];
    int len = find_dac(n, path, 0);
    if (!len) continue;
    unmute_path(cad, afg, path, len);
    verb(cad, n, 0x70700 | 0x40 | (dev == 2 ? 0x80 : 0));            /* the pin: out (and headphones) */
    verb(cad, n, 0x70C02);                                            /* EAPD on */
    u32 dac = path[len - 1];
    verb(cad, dac, 0x20000 | 0x4011);                                 /* 44.1 kHz, 16 bits, two channels */
    verb(cad, dac, 0x70600 | 1 << 4);                                 /* stream 1, channel 0 */
    dacs++;
  }
  return dacs;
}

/* found and playing: what to say on the console; 0 if there is none */
const char *hda_init(snd *s) {
  static char desc[96];
  u32 bus, dev, fn = 0, found = 0;
  for (bus = 0; bus < 8 && !found; bus++)
    for (dev = 0; dev < 32 && !found; dev++)
      for (fn = 0; fn < 8; fn++) {
        u32 id = pci_rd(bus, dev, fn, 0);
        if ((id & 0xffff) == 0xffff) { if (fn == 0) break; continue; }
        u32 cls = pci_rd(bus, dev, fn, 8) >> 16;
        if (cls == 0x0403) { found = 1; break; }
        if (fn == 0 && !(pci_rd(bus, dev, 0, 0x0C) & 0x800000)) break;   /* one function */
      }
  if (!found) return 0;
  hda_why = "HD Audio: its registers are not memory below 4 GB";
  bus--; dev--;
  u32 bar = pci_rd(bus, dev, fn, 0x10);
  if (bar & 1) return 0;                                              /* I/O space: not HDA's */
  if ((bar & 6) == 4 && pci_rd(bus, dev, fn, 0x14)) return 0;         /* above 4 GB */
  R = (volatile u8 *)(uintptr_t)(bar & ~0xfu);
  pci_wr(bus, dev, fn, 4, pci_rd(bus, dev, fn, 4) | 6);              /* memory space, bus master */
  /* the controller out of reset, and the codecs a moment to say so */
  R32(0x08) &= ~1u;
  for (u32 n = 0; n < 100000 && (R32(0x08) & 1); n++) udelay(1);
  R32(0x08) |= 1;
  for (u32 n = 0; n < 100000 && !(R32(0x08) & 1); n++) udelay(1);
  hda_why = "HD Audio: the controller stays in reset";
  if (!(R32(0x08) & 1)) return 0;
  udelay(2000);
  u32 codecs = R16(0x0E);
  hda_why = "HD Audio: no codec answers";
  if (!codecs) return 0;
  /* the CORB and the RIRB, 256 entries each */
  R8(0x4C) = 0; R8(0x5C) = 0;
  udelay(100);
  R32(0x40) = (u32)(uintptr_t)corb; R32(0x44) = 0;
  R8(0x4E) = 2;
  R16(0x4A) = 0x8000;                                                 /* the read pointer to 0 */
  for (u32 n = 0; n < 1000 && !(R16(0x4A) & 0x8000); n++) udelay(1);
  R16(0x4A) = 0;
  for (u32 n = 0; n < 1000 && (R16(0x4A) & 0x8000); n++) udelay(1);
  R16(0x48) = 0; corb_wp = 0;
  R32(0x50) = (u32)(uintptr_t)rirb; R32(0x54) = 0;
  R8(0x5E) = 2;
  R16(0x58) = 0x8000; rirb_rp = 0;                                    /* its write pointer to 0 */
  R16(0x5A) = 255;                                                    /* RINTCNT */
  R8(0x5C) = 2;                                                       /* the RIRB's DMA on */
  R8(0x4C) = 2;                                                       /* the CORB's */
  udelay(100);
  u32 cad = 0;
  while (cad < 15 && !(codecs & (1u << cad))) cad++;
  u32 vendor = param(cad, 0, 0x00);
  int dacs = codec_setup(cad);
  hda_why = "HD Audio: no output reaches a DAC";
  if (!dacs) return 0;
  /* the first output stream: reset, its ring and the list of its two halves */
  u32 iss = (R16(0x00) >> 8) & 15;
  SD = R + 0x80 + 0x20 * iss;
  SD[0] = 1;
  for (u32 n = 0; n < 1000 && !(SD[0] & 1); n++) udelay(1);
  SD[0] = 0;
  for (u32 n = 0; n < 1000 && (SD[0] & 1); n++) udelay(1);
  memset(ring, 0, sizeof ring);
  u32 half = sizeof ring / 2;
  bdl[0] = (u32)(uintptr_t)ring; bdl[1] = 0; bdl[2] = half; bdl[3] = 0;
  bdl[4] = (u32)(uintptr_t)ring + half; bdl[5] = 0; bdl[6] = half; bdl[7] = 0;
  *(volatile u32 *)(SD + 0x18) = (u32)(uintptr_t)bdl;
  *(volatile u32 *)(SD + 0x1C) = 0;
  *(volatile u32 *)(SD + 0x08) = sizeof ring;                         /* CBL */
  *(volatile u16 *)(SD + 0x0C) = 1;                                   /* LVI: two entries */
  *(volatile u16 *)(SD + 0x12) = 0x4011;                              /* the format, as the DACs' */
  SD[2] = 1 << 4;                                                     /* stream 1 */
  SD[0] = 2;                                                          /* run */
  card = s;
  played = made = last_pos = 0;
  /* "HD Audio, codec 15ad:1975, 1 output" */
  static const char hex[] = "0123456789abcdef";
  char *p = desc;
  const char *t = "HD Audio, codec ";
  while (*t) *p++ = *t++;
  for (int k = 28; k >= 16; k -= 4) *p++ = hex[(vendor >> k) & 15];
  *p++ = ':';
  for (int k = 12; k >= 0; k -= 4) *p++ = hex[(vendor >> k) & 15];
  t = dacs > 1 ? ", outputs: " : ", outputs: ";
  while (*t) *p++ = *t++;
  *p++ = (char)('0' + (dacs > 9 ? 9 : dacs)); *p = 0;
  return desc;
}

/* the ring mixed FILL frames past where the stream reads */
void hda_poll(void) {
  if (!card) return;
  SD[3] = 0x1C;                                                       /* its status bits, cleared */
  u32 pos = (*(volatile u32 *)(SD + 0x04) / 4) % RING;                /* LPIB: bytes */
  played += (pos - last_pos + RING) % RING;
  last_pos = pos;
  if ((i32)(made - played) < 0) { made = played; hda_underruns++; }
  u32 ahead = made - played;
  if (ahead >= FILL) return;
  u32 n = FILL - ahead;
  while (n > 0) {
    u32 at = made % RING, k = RING - at < n ? RING - at : n;
    snd_fill(card, ring + 2 * at, k);
    made += k; n -= k;
  }
}
