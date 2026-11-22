/* screen.c -- the display (gui.c) on a PC's screen, and the mouse.
 *
 * The screen is a linear framebuffer, from one of two places:
 *  - GRUB's: the multiboot header asks for 1024 x 768 x 32 (boot.S) and a
 *    GRUB with video sets a VBE mode and says where it is (the info's
 *    framebuffer fields, flags bit 12). VirtualBox, VMware, Hyper-V Gen 1,
 *    real PCs. An entry with "set gfxpayload=text" keeps text instead.
 *  - the Bochs display adapter (BGA, "VBE DISPI"), programmed here when
 *    there is no GRUB in front (QEMU's -kernel, Bochs): QEMU's std VGA and
 *    Bochs's are PCI 1234:1111, VirtualBox's 80EE:BEEF; BAR 0 is the
 *    framebuffer, ports 0x1CE and 0x1CF the mode.
 * What the display presents (gui.c's front buffer) is copied to it where
 * it changed, scaled by the nearest pixel when the display's size is not
 * the screen's and centred, and a pointer is drawn over it, as the page's
 * canvas has the system's.
 *
 * The mouse is the 8042's second port (PS/2): 3-byte packets on IRQ 12,
 * or 4 with a wheel where it answers the IntelliMouse knock (rates 200,
 * 100, 80, then ID 3). */
#include "../c4pc.h"
#include "../clib.h"

static inline void outb(u32 p, u8 v) { __asm__ volatile("outb %0,%w1" : : "a"(v), "Nd"(p)); }
static inline u8 inb(u32 p) { u8 v; __asm__ volatile("inb %w1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outw(u32 p, u16 v) { __asm__ volatile("outw %0,%w1" : : "a"(v), "Nd"(p)); }
static inline u16 inw(u32 p) { u16 v; __asm__ volatile("inw %w1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outl(u32 p, u32 v) { __asm__ volatile("outl %0,%w1" : : "a"(v), "Nd"(p)); }
static inline u32 inl(u32 p) { u32 v; __asm__ volatile("inl %w1,%0" : "=a"(v) : "Nd"(p)); return v; }

static u8 *fb; static u32 fb_pitch; static int sw, sh, bpp;
static int rpos = 16, gpos = 8, bpos = 0, rsz = 8, gsz = 8, bsz = 8;

/* ---- where the framebuffer comes from ---- */
struct mb_fb {
  u32 flags, mem_lower, mem_upper, boot_device, cmdline, mods_count, mods_addr;
  u32 syms[4], mmap_length, mmap_addr, drives_length, drives_addr, config_table, boot_loader_name, apm_table;
  u32 vbe_control_info, vbe_mode_info; u16 vbe_mode, vbe_seg, vbe_off, vbe_len;
  u32 fb_lo, fb_hi, fb_pitch, fb_width, fb_height; u8 fb_bpp, fb_type;
  u8 pad[2];           /* GRUB aligns the colour info (a union with a word in it) to 4 */
  u8 rp, rs, gp, gs, bp, bs;
};

int screen_from_multiboot(const void *info) {
  const struct mb_fb *mb = info;
  if (!(mb->flags & (1u << 12)) || mb->fb_type != 1 || mb->fb_hi) return 0;
  if (mb->fb_bpp != 32 && mb->fb_bpp != 24 && mb->fb_bpp != 16) return 0;
  fb = (u8 *)(uintptr_t)mb->fb_lo; fb_pitch = mb->fb_pitch;
  sw = (int)mb->fb_width; sh = (int)mb->fb_height; bpp = mb->fb_bpp;
  rpos = mb->rp; rsz = mb->rs; gpos = mb->gp; gsz = mb->gs; bpos = mb->bp; bsz = mb->bs;
  return 1;
}

static u32 pci_rd(u32 bus, u32 dev, u32 fn, u32 off) {
  outl(0xCF8, 0x80000000u | bus << 16 | dev << 11 | fn << 8 | (off & 0xfc));
  return inl(0xCFC);
}
static void bga(u16 i, u16 v) { outw(0x1CE, i); outw(0x1CF, v); }
static u16 bga_rd(u16 i) { outw(0x1CE, i); return inw(0x1CF); }

int screen_bga(int w, int h) {
  u32 bar = 0;
  for (u32 d = 0; d < 32 && !bar; d++) {
    u32 id = pci_rd(0, d, 0, 0);
    if (id == 0x11111234u || id == 0xBEEF80EEu) bar = pci_rd(0, d, 0, 0x10) & ~0xfu;
  }
  if (!bar) return 0;
  if (bga_rd(0) < 0xB0C2) return 0;           /* 32 bits a pixel needs version 2 */
  bga(4, 0);                                  /* off while it is set */
  bga(1, (u16)w); bga(2, (u16)h); bga(3, 32);
  bga(4, 0x01 | 0x40);                        /* on, with the linear framebuffer */
  fb = (u8 *)(uintptr_t)bar; sw = w; sh = h; bpp = 32; fb_pitch = (u32)w * 4;
  rpos = 16; gpos = 8; bpos = 0; rsz = gsz = bsz = 8;
  return 1;
}

void screen_size(int *w, int *h) { *w = sw; *h = sh; }
void pc_log(const char *s);
void pc_logn(i64 v);
void screen_describe(void) {
  pc_log("c4pc: screen "); pc_logn(sw); pc_log(" x "); pc_logn(sh); pc_log(" x "); pc_logn(bpp);
  pc_log(", pitch "); pc_logn(fb_pitch); pc_log(", red "); pc_logn(rpos); pc_log("/"); pc_logn(rsz);
  pc_log(" green "); pc_logn(gpos); pc_log("/"); pc_logn(gsz); pc_log(" blue "); pc_logn(bpos); pc_log("/"); pc_logn(bsz); pc_log("\n");
}

/* ---- the display on it ---- */
static inline u32 pix(u32 rgb) {
  if (bpp == 32 && rpos == 16 && gpos == 8 && bpos == 0) return rgb;
  u32 r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
  return (r >> (8 - rsz)) << rpos | (g >> (8 - gsz)) << gpos | (b >> (8 - bsz)) << bpos;
}
static inline void put(int x, int y, u32 v) {
  u8 *p = fb + (size_t)y * fb_pitch;
  if (bpp == 32) ((u32 *)p)[x] = v;
  else if (bpp == 24) { p += x * 3; p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); }
  else ((u16 *)p)[x] = (u16)v;
}

/* the display's place on the screen: scaled by n/d, at ox, oy */
static int cw, ch, ox, oy, num = 1, den = 1;
static void place(int w, int h) {
  if (w == cw && h == ch) return;
  cw = w; ch = h;
  /* the largest scale that fits, n/d with d the display's side */
  if ((i64)sw * h <= (i64)sh * w) { num = sw; den = w; } else { num = sh; den = h; }
  if (w == sw && h == sh) { num = den = 1; }
  ox = (sw - w * num / den) / 2; oy = (sh - h * num / den) / 2;
  for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) put(x, y, 0);
}
/* screen pixels [x0, x1) x [y0, y1) from the display's front */
static void copy(const u32 *px, int x0, int y0, int x1, int y1) {
  int ex = ox + cw * num / den, ey = oy + ch * num / den;
  if (x0 < ox) x0 = ox;
  if (y0 < oy) y0 = oy;
  if (x1 > ex) x1 = ex;
  if (y1 > ey) y1 = ey;
  if (x1 <= x0 || y1 <= y0) return;
  for (int y = y0; y < y1; y++) {
    int sy = (int)((i64)(y - oy) * den / num);
    const u32 *row = px + (size_t)sy * cw;
    if (num == den && bpp == 32 && rpos == 16 && gpos == 8 && bpos == 0) {
      memcpy(fb + (size_t)y * fb_pitch + (size_t)x0 * 4, row + (x0 - ox), (size_t)(x1 - x0) * 4);
      continue;
    }
    for (int x = x0; x < x1; x++) put(x, y, pix(row[(int)((i64)(x - ox) * den / num)]));
  }
}

/* the pointer: an arrow, 1 black and 2 white */
static const char *ARROW[] = {
  "1", "11", "121", "1221", "12221", "122221", "1222221", "12222221", "122222221", "1222222221",
  "12222211111", "1221221", "121 1221", "11  1221", "1    1221", "     1221", "      11",
};
#define AW 11
#define AH 17
static int px_, py_, shown;
static void pointer_draw(int x, int y) {
  for (int j = 0; j < AH; j++) for (int i = 0; ARROW[j][i]; i++) {
    char c = ARROW[j][i];
    if (c == ' ' || x + i >= sw || y + j >= sh) continue;
    put(x + i, y + j, pix(c == '1' ? 0 : 0xffffff));
  }
}

/* what changed since the last look, and the pointer */
void screen_show(gui *g) {
  if (!fb) return;
  int w, h, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  const u32 *px = gui_front(g, &w, &h);
  int full = 0;
  if (w != cw || h != ch) { place(w, h); shown = 0; full = 1; }
  if (!gui_take_dirty(g, &x0, &y0, &x1, &y1)) x1 = x0 = y1 = y0 = 0;
  if (full) { x0 = 0; y0 = 0; x1 = w; y1 = h; }
  int cx, cy, shape = gui_pointer(g, &cx, &cy);
  int sx = ox + cx * num / den, sy = oy + cy * num / den;
  int moved = !shown || sx != px_ || sy != py_ || shape < 0;
  if (x1 > x0 && y1 > y0)
    copy(px, ox + x0 * num / den, oy + y0 * num / den, ox + (x1 * num + den - 1) / den, oy + (y1 * num + den - 1) / den);
  else if (!moved) return;
  if (shown) copy(px, px_, py_, px_ + AW, py_ + AH);     /* where it was */
  shown = 0;
  if (shape >= 0) { pointer_draw(sx, sy); px_ = sx; py_ = sy; shown = 1; }
}

/* ---- the mouse ---- */
/* whole packets, for the run loop: a press and its release between two
 * looks are two changes, not none */
typedef struct { short dx, dy; signed char wheel, buttons; } mpkt;
static volatile mpkt mq[256];
static volatile u32 mq_head, mq_tail;
static u8 pkt[4]; static int pn, plen = 3;

static void wait_in(void) { for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) ; }
static int wait_out(void) { for (int i = 0; i < 100000; i++) if (inb(0x64) & 1) return 1; return 0; }
static void aux(u8 v) { wait_in(); outb(0x64, 0xD4); wait_in(); outb(0x60, v); }
static int aux_ack(u8 v) { aux(v); return wait_out() ? inb(0x60) : -1; }

/* the mouse on, IRQ 12 to the caller to unmask: 1 if there is one */
static int mouse_setup(void);
int mouse_init(void) {
  /* with interrupts off: the controller's replies would raise the
   * keyboard's IRQ 1, whose handler would take them for keys */
  u32 fl;
  __asm__ volatile("pushfl; popl %0; cli" : "=r"(fl));
  int r = mouse_setup();
  while (inb(0x64) & 1) (void)inb(0x60);      /* nothing left over */
  if (fl & 0x200) __asm__ volatile("sti");
  return r;
}
static int mouse_setup(void) {
  wait_in(); outb(0x64, 0xA8);                /* the second port on */
  wait_in(); outb(0x64, 0x20);
  if (!wait_out()) return 0;
  u8 cfg = inb(0x60);
  cfg |= 2; cfg &= ~0x20;                     /* its interrupt, its clock */
  wait_in(); outb(0x64, 0x60); wait_in(); outb(0x60, cfg);
  if (aux_ack(0xF6) != 0xFA) return 0;        /* defaults */
  /* the IntelliMouse knock: a wheel says ID 3 */
  aux_ack(0xF3); aux_ack(200); aux_ack(0xF3); aux_ack(100); aux_ack(0xF3); aux_ack(80);
  aux_ack(0xF2);
  int id = wait_out() ? inb(0x60) : 0;
  plen = id == 3 ? 4 : 3;
  aux_ack(0xF4);                              /* stream */
  return 1;
}

/* a byte from the second port (IRQ 12, or the first's handler finding it) */
int mouse_debug;
void pc_log(const char *s);
void pc_logn(i64 v);
void mouse_byte(u8 b) {
  if (mouse_debug > 1) { pc_log("mouse byte "); pc_logn(b); pc_log(pn ? "\n" : " (first)\n"); }
  if (pn == 0 && !(b & 8)) return;            /* out of step: wait for a first byte */
  pkt[pn++] = b;
  if (pn < plen) return;
  pn = 0;
  int dx = pkt[1] - ((pkt[0] << 4) & 0x100), dy = pkt[2] - ((pkt[0] << 3) & 0x100);
  if (pkt[0] & 0xC0) dx = dy = 0;             /* overflow */
  int z = 0;
  if (plen == 4) { z = pkt[3] & 15; if (z & 8) z -= 16; }
  u32 t = mq_tail;
  if (t - mq_head >= 256) return;
  mpkt *q = (mpkt *)&mq[t & 255];
  q->dx = (short)dx; q->dy = (short)-dy; q->wheel = (signed char)z;
  /* left, right, middle to the display's left, middle, right */
  q->buttons = (signed char)((pkt[0] & 1) | ((pkt[0] & 4) ? 2 : 0) | ((pkt[0] & 2) ? 4 : 0));
  mq_tail = t + 1;
}

/* what the mouse did since, to the display (screen pixels to its own) */
int mouse_pending(void) { return mq_head != mq_tail; }
void mouse_poll(gui *g) {
  while (mq_head != mq_tail) {
    mpkt p = *(mpkt *)&mq[mq_head & 255];
    mq_head++;
    gui_mouse(g, (int)((i64)p.dx * 256 * den / num), (int)((i64)p.dy * 256 * den / num), p.buttons, p.wheel);
    if (mouse_debug) { int x, y; gui_pointer(g, &x, &y); pc_log("pointer "); pc_logn(p.dx); pc_log(" "); pc_logn(p.dy); pc_log(" buttons "); pc_logn(p.buttons); pc_log(" now "); pc_logn(x); pc_log(" "); pc_logn(y); pc_log("#"); }
  }
}
