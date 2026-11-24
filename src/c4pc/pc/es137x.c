/* es137x.c -- the Ensoniq AudioPCI under the machine's sound card: the
 * ES1371 (and its CT5880 and EV1938 kin), VMware's sound device for many
 * guest types, and the ES1370 before it, QEMU's `-device ES1370`.
 * Registers and the order of the ES1371's setting up are Linux's
 * (sound/pci/ens1370.c); docs/c4pc.md, "Sound".
 *
 * One stream: DAC2, 16-bit stereo at 44.1 kHz, looping round the same
 * ring as the SB16's and HD Audio's (8,192 frames), and polled: its
 * position is the DAC2 frame's count, and the run loop mixes FILL frames
 * past it (es137x_poll). No interrupt reaches the CPU.
 *   - the ES1370 divides its clock for the rate (CONTROL's PCLKDIV) and
 *     has an AK4531 codec, reset and its mixer opened;
 *   - the ES1371 sends DAC2 through its sample-rate converter, whose RAM
 *     must be set before it is enabled (a converter enabled unset locks
 *     the chip), and has an AC'97 codec: reset, then master, headphone
 *     and PCM out at 0 dB, unmuted.
 *
 * What it did is said to c4pc's log (its console: Ctrl+Alt+F1; the
 * hypervisor's Log): the codec's id and the volumes read back, and for
 * the first seconds DAC2's position and what was mixed, so a card that
 * is found but silent (VMware, 2026-11-24) says whether its DMA moves.
 */
#include "../c4pc.h"
#include "../clib.h"
#include "../snd.h"

static inline void outl(u32 p, u32 v) { __asm__ volatile("outl %0,%w1" : : "a"(v), "Nd"(p)); }
static inline u32 inl(u32 p) { u32 v; __asm__ volatile("inl %w1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outw(u32 p, u16 v) { __asm__ volatile("outw %0,%w1" : : "a"(v), "Nd"(p)); }
static inline u16 inw(u32 p) { u16 v; __asm__ volatile("inw %w1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline u8 inb(u32 p) { u8 v; __asm__ volatile("inb %w1,%0" : "=a"(v) : "Nd"(p)); return v; }
static u32 pci_rd(u32 bus, u32 dev, u32 fn, u32 off) {
  outl(0xCF8, 0x80000000u | bus << 16 | dev << 11 | fn << 8 | (off & 0xfc));
  return inl(0xCFC);
}
static void pci_wr(u32 bus, u32 dev, u32 fn, u32 off, u32 v) {
  outl(0xCF8, 0x80000000u | bus << 16 | dev << 11 | fn << 8 | (off & 0xfc));
  outl(0xCFC, v);
}
static void udelay(u32 us) { for (u32 i = 0; i < us; i++) inb(0x80); }

enum {
  CONTROL = 0x00, STATUS = 0x04, MEM_PAGE = 0x0c, CODEC1370 = 0x10, SMPRATE = 0x10, CODEC1371 = 0x14,
  LEGACY = 0x18, SERIAL = 0x20, DAC2_COUNT = 0x28, DAC2_FRAME = 0x38, DAC2_SIZE = 0x3c, PAGE_DAC = 0x0c,
};
#define DAC2_EN        (1u << 5)
#define CDC_EN_1370    (1u << 1)
#define SYNC_RES_1371  (1u << 14)
#define PCLKDIV(o)     (((u32)(o) & 0x1fff) << 16)
#define SRTODIV(x)     (1411200 / (x) - 2)
#define CSTAT_1370     (1u << 10)
#define P2_MODE(o)     (((u32)(o) & 3) << 2)
#define P2_END_INC(o)  (((u32)(o) & 7) << 19)
#define SRC_RAM_ADDR(o) (((u32)(o) & 0x7f) << 25)
#define SRC_RAM_WE     (1u << 24)
#define SRC_RAM_BUSY   (1u << 23)
#define SRC_DISABLE    (1u << 22)
#define DIS_P1         (1u << 21)
#define DIS_P2         (1u << 20)
#define DIS_R1         (1u << 19)
#define SRC_STATE      (SRC_DISABLE | DIS_P1 | DIS_P2 | DIS_R1)
#define CODEC_WIP      (1u << 30)
#define CODEC_RDY      (1u << 31)
#define CODEC_PIRD     (1u << 23)
#define P2_INT_EN      (1u << 9)
#define ST_DAC2        (1u << 1)
#define CODEC_WRITE(a, d) ((((u32)(a) & 0x7f) << 16) | ((u32)(d) & 0xffff))

#define RATE 44100
#define RING 8192                      /* frames round the ring: 186 ms */
#define FILL 2048                      /* frames mixed ahead of the DMA: 46 ms */
static int16_t ring[RING * 2] __attribute__((aligned(64)));
static u32 io;                         /* the chip's I/O ports */
static int is1371;
static u32 played, made, last_pos;
static snd *card;
static i64 t_first;                    /* the first poll's time: the log's seconds count from it */
static int logged;
u32 es137x_underruns, es137x_acks;
void pc_log(const char *s);
void pc_logn(i64 v);
static void say_hex(const char *what, u32 v) {
  static const char hx[] = "0123456789abcdef";
  char b[12]; b[0] = '0'; b[1] = 'x';
  for (int i = 0; i < 8; i++) b[2 + i] = hx[(v >> (28 - 4 * i)) & 15];
  b[10] = 0;
  pc_log(what); pc_log(b);
}
const char *es137x_why = "no Ensoniq AudioPCI";

/* ---- the ES1371's sample-rate converter and AC'97 codec ---- */
static u32 src_ready(void) {
  u32 r = 0;
  for (u32 i = 0; i < 0xa000; i++) { r = inl(io + SMPRATE); if (!(r & SRC_RAM_BUSY)) return r; }
  return r;
}
static void src_write(u32 reg, u32 v) {
  u32 r = src_ready() & SRC_STATE;
  outl(io + SMPRATE, r | SRC_RAM_ADDR(reg) | (v & 0xffff) | SRC_RAM_WE);
}
static void codec1371(u32 reg, u32 v) {
  for (u32 i = 0; i < 0xa000 && (inl(io + CODEC1371) & CODEC_WIP); i++) { }
  u32 x = src_ready();
  outl(io + SMPRATE, (x & SRC_STATE) | 0x00010000);
  /* the converter at the point the codec may be written */
  for (u32 i = 0; i < 0xa000 && (inl(io + SMPRATE) & 0x00870000) != 0; i++) { }
  for (u32 i = 0; i < 0xa000 && (inl(io + SMPRATE) & 0x00870000) != 0x00010000; i++) { }
  outl(io + CODEC1371, CODEC_WRITE(reg, v));
  src_ready();
  outl(io + SMPRATE, x);
}
/* an AC'97 register read back (snd_es1371_codec_read), or 0xffffffff */
static u32 codec1371_rd(u32 reg) {
  for (u32 i = 0; i < 0xa000 && (inl(io + CODEC1371) & CODEC_WIP); i++) { }
  u32 x = src_ready();
  outl(io + SMPRATE, (x & SRC_STATE) | 0x00010000);
  for (u32 i = 0; i < 0xa000 && (inl(io + SMPRATE) & 0x00870000) != 0; i++) { }
  for (u32 i = 0; i < 0xa000 && (inl(io + SMPRATE) & 0x00870000) != 0x00010000; i++) { }
  outl(io + CODEC1371, ((reg & 0x7f) << 16) | CODEC_PIRD);
  src_ready();
  outl(io + SMPRATE, x);
  for (u32 i = 0; i < 0xa000 && (inl(io + CODEC1371) & CODEC_WIP); i++) { }
  for (u32 i = 0; i < 0xa000; i++) { u32 v = inl(io + CODEC1371); if (v & CODEC_RDY) return v & 0xffff; }
  return 0xffffffffu;
}
/* DAC2's rate through the converter (snd_es1371_dac2_rate): its integer
 * and fraction registers, 0x75's low byte kept (0 after the RAM's clearing) */
static void dac2_rate(u32 rate) {
  u32 freq = ((rate << 15) + 1500) / 3000;
  outl(io + SMPRATE, (src_ready() & (SRC_DISABLE | DIS_P1 | DIS_R1)) | DIS_P2);
  src_write(0x75, (16u << 10 & 0x00ff) | ((freq >> 5) & 0xfc00));
  src_write(0x77, freq & 0x7fff);
  outl(io + SMPRATE, src_ready() & (SRC_DISABLE | DIS_P1 | DIS_R1));
}
static void init1371(void) {
  outl(io + CONTROL, 0);
  outl(io + SERIAL, 0);
  outl(io + LEGACY, 0);
  /* the AC'97 link's warm reset */
  outl(io + CONTROL, SYNC_RES_1371);
  (void)inl(io + CONTROL);
  udelay(20);
  outl(io + CONTROL, 0);
  /* the converter's RAM, cleared and set with it disabled */
  src_ready();
  outl(io + SMPRATE, SRC_DISABLE);
  for (u32 r = 0; r < 0x80; r++) src_write(r, 0);
  src_write(0x70, 16u << 4); src_write(0x71, 16u << 10);
  src_write(0x74, 16u << 4); src_write(0x75, 16u << 10);
  src_write(0x6c, 1u << 12); src_write(0x6d, 1u << 12);
  src_write(0x7c, 1u << 12); src_write(0x7d, 1u << 12);
  src_write(0x7e, 1u << 12); src_write(0x7f, 1u << 12);
  dac2_rate(RATE);
  /* the converter on for DAC2; DAC1 and the ADC, unused and unset, off */
  src_ready();
  outl(io + SMPRATE, DIS_P1 | DIS_R1);
  /* the codec: reset (register 0), a moment, then its outputs open --
   * master and headphone 0 dB (0x0000), PCM out 0 dB (0x0808), powered */
  codec1371(0x00, 0);
  udelay(20000);
  codec1371(0x26, 0);
  codec1371(0x02, 0x0000);
  codec1371(0x04, 0x0000);
  codec1371(0x06, 0x0000);
  codec1371(0x18, 0x0808);
}

/* ---- the ES1370's AK4531 ---- */
static void codec1370(u32 reg, u32 v) {
  for (u32 i = 0; i < 0xa000 && (inl(io + STATUS) & CSTAT_1370); i++) { }
  outw(io + CODEC1370, (u16)(((reg & 0xff) << 8) | (v & 0xff)));
  (void)inw(io + CODEC1370);
}
static void init1370(void) {
  outl(io + CONTROL, CDC_EN_1370 | PCLKDIV(SRTODIV(RATE)));
  outl(io + SERIAL, 0);
  /* the AK4531: reset, its clock, the master and voice (PCM) at 0 dB and
   * the voice routed to the output mixer */
  codec1370(0x16, 0x02); udelay(100);
  codec1370(0x16, 0x03); udelay(100);
  codec1370(0x17, 0x00);
  codec1370(0x00, 0x00); codec1370(0x01, 0x00);
  codec1370(0x02, 0x06); codec1370(0x03, 0x06);
  codec1370(0x11, 0x0c);
}

/* the first AudioPCI on any bus (VMware puts its devices behind bridges) */
static int find(u32 *pbus, u32 *pdev, u32 *pfn, u32 *pid) {
  for (u32 bus = 0; bus < 256; bus++)
    for (u32 dev = 0; dev < 32; dev++)
      for (u32 fn = 0; fn < 8; fn++) {
        u32 id = pci_rd(bus, dev, fn, 0);
        if ((id & 0xffff) == 0xffff) { if (fn == 0) break; continue; }
        u32 vid = id & 0xffff, did = id >> 16;
        if ((vid == 0x1274 && (did == 0x5000 || did == 0x1371 || did == 0x5880)) || (vid == 0x1102 && did == 0x8938)) {
          *pbus = bus; *pdev = dev; *pfn = fn; *pid = id;
          return 1;
        }
        if (fn == 0 && !(pci_rd(bus, dev, 0, 0x0C) & 0x800000)) break;   /* one function */
      }
  return 0;
}

const char *es137x_init(snd *s) {
  static char desc[64];
  u32 bus, dev, fn, id;
  if (!find(&bus, &dev, &fn, &id)) return 0;
  u32 bar = pci_rd(bus, dev, fn, 0x10);
  es137x_why = "Ensoniq AudioPCI: its registers are not I/O ports";
  if (!(bar & 1)) return 0;
  io = bar & ~3u;
  pci_wr(bus, dev, fn, 4, pci_rd(bus, dev, fn, 4) | 5);              /* I/O space, bus master */
  is1371 = (id >> 16) != 0x5000;
  if (is1371) init1371(); else init1370();
  /* DAC2 round the ring: its frame in the DAC page, the size in longwords
   * less one; looping (P2_LOOP_SEL clear), 16-bit stereo, no interrupt */
  memset(ring, 0, sizeof ring);
  u32 ctrl = inl(io + CONTROL) & ~DAC2_EN;
  outl(io + CONTROL, ctrl);
  outl(io + MEM_PAGE, PAGE_DAC);
  outl(io + DAC2_FRAME, (u32)(uintptr_t)ring);
  outl(io + DAC2_SIZE, sizeof ring / 4 - 1);
  outl(io + SERIAL, P2_MODE(3) | P2_END_INC(2));
  outl(io + DAC2_COUNT, sizeof ring / 8 - 1);
  outl(io + CONTROL, ctrl | DAC2_EN);
  card = s;
  played = made = last_pos = 0;
  t_first = 0; logged = 0;
  pc_log("c4pc: es137x: ports at "); pc_logn((i64)io); say_hex(", id ", id);
  pc_log(" (pci "); pc_logn((i64)bus); pc_log(":"); pc_logn((i64)dev); pc_log("."); pc_logn((i64)fn); pc_log(")\n");
  if (is1371) {
    pc_log("c4pc: es137x: AC'97 codec"); say_hex(" vendor ", codec1371_rd(0x7c) << 16 | codec1371_rd(0x7e));
    say_hex(", master ", codec1371_rd(0x02)); say_hex(", PCM ", codec1371_rd(0x18));
    say_hex(", powerdown ", codec1371_rd(0x26)); pc_log("\n");
  }
  say_hex("c4pc: es137x: CONTROL ", inl(io + CONTROL)); say_hex(", STATUS ", inl(io + STATUS));
  say_hex(", SERIAL ", inl(io + SERIAL)); if (is1371) say_hex(", SMPRATE ", inl(io + SMPRATE));
  pc_log("\n");
  const char *t = is1371 ? ((id >> 16) == 0x5880 ? "Ensoniq AudioPCI (CT5880)" : (id & 0xffff) == 0x1102 ? "Ensoniq AudioPCI (EV1938)" : "Ensoniq AudioPCI ES1371")
                         : "Ensoniq AudioPCI ES1370";
  char *p = desc;
  while (*t) *p++ = *t++;
  t = ", DAC2 at 44.1 kHz";
  while (*t) *p++ = *t++;
  *p = 0;
  return desc;
}

/* the ring mixed FILL frames past where DAC2 reads: its frame's count,
 * in longwords (a frame each) */
void es137x_poll(void) {
  if (!card) return;
  /* DAC2's interrupt pending (STATUS bit 1, though its enable is off):
   * VMware's ES1371 stopped at the frame it came at, 4096, until it was
   * acknowledged (the user's log, 2026-11-24) -- as Linux does it, its
   * enable set and cleared, here with the CPU's interrupts held off so
   * that the line is never seen up (it may be the ACPI SCI's) */
  if (inl(io + STATUS) & ST_DAC2) {
    unsigned long fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) : : "memory");
    u32 sc = inl(io + SERIAL);
    outl(io + SERIAL, sc | P2_INT_EN);
    outl(io + SERIAL, sc & ~P2_INT_EN);
    __asm__ volatile("push %0; popf" : : "r"(fl) : "memory", "cc");
    es137x_acks++;
  }
  outl(io + MEM_PAGE, PAGE_DAC);
  u32 pos = (inl(io + DAC2_SIZE) >> 16) % RING;
  played += (pos - last_pos + RING) % RING;
  last_pos = pos;
  if ((i32)(made - played) < 0) { made = played; es137x_underruns++; }
  /* the first seconds, once a second: is DAC2 moving */
  if (logged < 4) {
    i64 now = plat_now_ms();
    if (!t_first) t_first = now;
    if (now - t_first >= (i64)logged * 1000) {
      pc_log("c4pc: es137x: "); pc_logn(now - t_first); pc_log(" ms: DAC2 at frame "); pc_logn((i64)pos);
      pc_log(", "); pc_logn((i64)played); pc_log(" played, "); pc_logn((i64)made); pc_log(" mixed, ");
      pc_logn((i64)es137x_underruns); pc_log(" underruns, "); pc_logn((i64)es137x_acks); say_hex(" acks; CONTROL ", inl(io + CONTROL));
      say_hex(", STATUS ", inl(io + STATUS)); pc_log("\n");
      logged++;
    }
  }
  u32 ahead = made - played;
  if (ahead >= FILL) return;
  u32 n = FILL - ahead;
  while (n > 0) {
    u32 at = made % RING, k = RING - at < n ? RING - at : n;
    snd_fill(card, ring + 2 * at, k);
    made += k; n -= k;
  }
}
