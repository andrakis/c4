/* sb16.c -- a Sound Blaster 16 under the machine's sound card (snd.c):
 * what snd.c mixes (the AdLib through opl.c, the samples, the speaker)
 * played as 16-bit stereo at 44100 Hz, round and round a ring by the
 * DSP's auto-initialised DMA. QEMU (-device sb16), DOSBox and DOSBox-X,
 * VMware (sound.virtualDev = "sb16" in the .vmx) and real cards have one.
 *
 * No interrupt: the run loop calls sb16_poll after every slice and at
 * every tick it waits through, which reads how far the DMA has read and
 * mixes the ring that far ahead again, FILL frames past it. The DSP's
 * interrupt at each half of the ring is acknowledged there and never
 * reaches the CPU (its PIC line stays masked); the DMA goes round
 * regardless, as auto-init does.
 *
 * The card is found at 0x220 or 0x240 (the DSP's reset answers 0xAA), its
 * version must be 4 or more, and its 16-bit DMA channel is the one its
 * mixer's register 0x81 names (5 if none). */
#include "../c4pc.h"
#include "../clib.h"
#include "../snd.h"

static inline void outb(u32 p, u8 v) { __asm__ volatile("outb %0,%w1" : : "a"(v), "Nd"(p)); }
static inline u8 inb(u32 p) { u8 v; __asm__ volatile("inb %w1,%0" : "=a"(v) : "Nd"(p)); return v; }

#define RATE 44100
#define RING 8192                  /* frames round the ring: 186 ms */
#define FILL 2048                  /* frames mixed ahead of the DMA: 46 ms */
static int16_t ring[RING * 2] __attribute__((aligned(65536)));
static u32 base, hdma, ring_words;
static u32 played, made, last_pos;  /* frames: read by the DMA, mixed, the DMA's place last time */
static snd *card;
u32 sb16_underruns;

static const u8 ADDR[4] = {0xC0, 0xC4, 0xC8, 0xCC}, COUNT[4] = {0xC2, 0xC6, 0xCA, 0xCE}, PAGE[4] = {0x8F, 0x8B, 0x89, 0x8A};

static void io_delay(void) { for (int i = 0; i < 8; i++) inb(0x80); }
static int dsp_write(u8 v) {
  for (u32 n = 0; n < 100000; n++) if (!(inb(base + 0xC) & 0x80)) { outb(base + 0xC, v); return 1; }
  return 0;
}
static int dsp_read(void) {
  for (u32 n = 0; n < 100000; n++) if (inb(base + 0xE) & 0x80) return inb(base + 0xA);
  return -1;
}
static int dsp_reset(u32 b) {
  base = b;
  outb(b + 6, 1); io_delay(); io_delay(); outb(b + 6, 0);
  return dsp_read() == 0xAA;
}

/* the DMA's place in the ring, in frames: its current address, read twice
 * so that a carry between the two bytes is not taken for a jump */
static u32 dma_pos(void) {
  u32 c = hdma & 3, start = ((u32)(uintptr_t)ring >> 1) & 0xffff, a, b;
  for (int k = 0; k < 4; k++) {
    outb(0xD8, 0); a = inb(ADDR[c]); a |= (u32)inb(ADDR[c]) << 8;
    outb(0xD8, 0); b = inb(ADDR[c]); b |= (u32)inb(ADDR[c]) << 8;
    if (a == b || ((a ^ b) & 0xff00) == 0) break;
  }
  u32 w = (b - start) & 0xffff;
  if (w >= ring_words) w = 0;
  return w / 2;
}

/* found and playing: what to say on the console; 0 if there is no card */
const char *sb16_init(snd *s) {
  static char desc[64];
  if ((u32)(uintptr_t)ring + sizeof ring > 0x1000000) return 0;      /* the ISA DMA reaches 16 MB */
  if (!dsp_reset(0x220) && !dsp_reset(0x240)) return 0;
  if (!dsp_write(0xE1)) return 0;
  int major = dsp_read(), minor = dsp_read();
  if (major < 4) return 0;
  outb(base + 4, 0x81);
  u8 dmas = inb(base + 5);
  hdma = dmas & 0x80 ? 7 : dmas & 0x40 ? 6 : 5;
  memset(ring, 0, sizeof ring);
  ring_words = RING * 2;
  /* the DMA: single, auto-init, memory to the card, from the ring's start */
  u32 c = hdma & 3, phys = (u32)(uintptr_t)ring, w = phys >> 1;
  outb(0xD4, (u8)(4 | c));
  outb(0xD8, 0);
  outb(0xD6, (u8)(0x58 | c));
  outb(ADDR[c], (u8)w); outb(ADDR[c], (u8)(w >> 8));
  outb(PAGE[c], (u8)((phys >> 16) & 0xfe));
  outb(0xD8, 0);
  outb(COUNT[c], (u8)(ring_words - 1)); outb(COUNT[c], (u8)((ring_words - 1) >> 8));
  outb(0xD4, (u8)c);
  /* the DSP: the rate, then 16-bit stereo signed output, auto-init, an
   * interrupt every half of the ring */
  dsp_write(0xD1);
  dsp_write(0x41); dsp_write(RATE >> 8); dsp_write(RATE & 0xff);
  dsp_write(0xB6); dsp_write(0x30);
  dsp_write((u8)(RING - 1)); dsp_write((u8)((RING - 1) >> 8));
  card = s;
  played = made = last_pos = 0;
  /* "SB16 at 220h, DSP 4.5, 16-bit DMA 5" */
  char *p = desc;
  const char *t = "SB16 at ";
  while (*t) *p++ = *t++;
  *p++ = "0123456789ABCDEF"[(base >> 8) & 15]; *p++ = "0123456789ABCDEF"[(base >> 4) & 15]; *p++ = '0'; *p++ = 'h';
  t = ", DSP "; while (*t) *p++ = *t++;
  *p++ = (char)('0' + major % 10); *p++ = '.'; *p++ = (char)('0' + (minor / 10) % 10); *p++ = (char)('0' + minor % 10);
  t = ", 16-bit DMA "; while (*t) *p++ = *t++;
  *p++ = (char)('0' + hdma); *p = 0;
  return desc;
}

/* the ring mixed FILL frames past where the DMA reads */
void sb16_poll(void) {
  if (!card) return;
  inb(base + 0xF);                                  /* the 16-bit interrupt, acknowledged */
  u32 pos = dma_pos();
  played += (pos - last_pos + RING) % RING;
  last_pos = pos;
  if ((i32)(made - played) < 0) { made = played; sb16_underruns++; }   /* fallen behind: start again here */
  u32 ahead = made - played;
  if (ahead >= FILL) return;
  u32 n = FILL - ahead;
  while (n > 0) {
    u32 at = made % RING, k = RING - at < n ? RING - at : n;
    snd_fill(card, ring + 2 * at, k);
    made += k; n -= k;
  }
}
