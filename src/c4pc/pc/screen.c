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
#include "../font.h"

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
  pc_log("c4pc: screen at "); pc_logn((i64)(uintptr_t)fb); pc_log(", "); pc_logn(sw); pc_log(" x "); pc_logn(sh); pc_log(" x "); pc_logn(bpp);
  pc_log(", pitch "); pc_logn(fb_pitch); pc_log(", red "); pc_logn(rpos); pc_log("/"); pc_logn(rsz);
  pc_log(" green "); pc_logn(gpos); pc_log("/"); pc_logn(gsz); pc_log(" blue "); pc_logn(bpos); pc_log("/"); pc_logn(bsz); pc_log("\n");
}

/* ---- write-combining: a BIOS leaves the framebuffer uncached (the
 * MTRRs' default type, or a range of its own over the PCI hole, as
 * SeaBIOS's), where every store is a bus cycle of its own; write-combined,
 * the CPU gathers a row's stores into bursts. Two ways, as Linux's:
 *  - the PAT: paging on, every 4 GB mapped to itself in 4 MB pages, and
 *    the framebuffer's pages given the PAT's entry 1, set to WC. A page's
 *    WC wins over an MTRR's UC (Intel SDM vol. 3, 11.5.2.2);
 *  - without a PAT, a variable MTRR saying WC, where no range covers the
 *    framebuffer already (11.11.8: caches off and flushed, MTRRs off, the
 *    pair written, all back on).
 * Under KVM the guest's memory types mean nothing; VMware and real PCs
 * heed them. --no-wc leaves all of it be ---- */
static inline void cpuid(u32 l, u32 *a, u32 *b, u32 *c, u32 *d) { __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(l), "c"(0)); }
static inline uint64_t rdmsr(u32 r) { u32 lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(r)); return (uint64_t)hi << 32 | lo; }
static inline void wrmsr(u32 r, uint64_t v) { __asm__ volatile("wrmsr" : : "c"(r), "a"((u32)v), "d"((u32)(v >> 32))); }
#ifdef __x86_64__
/* long mode: paging is boot64.S's already, 2 MB pages over 4 GB; the
 * framebuffer's get the PAT's entry 1 */
extern uint64_t boot_pd[2048];
static const char *pat_wc(void) {
  u32 base = (u32)(uintptr_t)fb, end = base + fb_pitch * (u32)sh;
  uintptr_t eflags; __asm__ volatile("pushf; pop %0; cli" : "=r"(eflags));
  uint64_t pat = rdmsr(0x277);
  pat = (pat & ~((uint64_t)0xff << 8)) | ((uint64_t)0x01 << 8);   /* entry 1: WC */
  __asm__ volatile("wbinvd" : : : "memory");
  wrmsr(0x277, pat);
  for (u32 i = 0; i < 2048; i++) {
    uint64_t at = (uint64_t)i << 21;
    if (at + (1u << 21) > base && at < end) boot_pd[i] |= 0x08;   /* PWT: the PAT's entry 1 */
  }
  uintptr_t cr3; __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
  __asm__ volatile("wbinvd" : : : "memory");
  if (eflags & 0x200) __asm__ volatile("sti");
  return "write-combining (PAT)";
}
#else
static u32 pdir[1024] __attribute__((aligned(4096)));
static const char *pat_wc(void) {
  u32 base = (u32)(uintptr_t)fb, end = base + fb_pitch * (u32)sh;
  for (u32 i = 0; i < 1024; i++) {
    u32 at = i << 22;
    pdir[i] = at | 0x83;                              /* 4 MB, writable, present */
    if (at + (1u << 22) > base && at < end) pdir[i] |= 0x08;   /* PWT: the PAT's entry 1 */
  }
  uintptr_t eflags; __asm__ volatile("pushf; pop %0; cli" : "=r"(eflags));
  uint64_t pat = rdmsr(0x277);
  pat = (pat & ~((uint64_t)0xff << 8)) | ((uint64_t)0x01 << 8);   /* entry 1: WC */
  __asm__ volatile("wbinvd" : : : "memory");
  wrmsr(0x277, pat);
  uintptr_t cr4; __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
  __asm__ volatile("mov %0, %%cr4" : : "r"(cr4 | 0x10));          /* PSE */
  __asm__ volatile("mov %0, %%cr3" : : "r"((u32)(uintptr_t)pdir) : "memory");
  uintptr_t cr0; __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  __asm__ volatile("mov %0, %%cr0; jmp 1f; 1:" : : "r"(cr0 | 0x80000000u) : "memory");
  __asm__ volatile("wbinvd" : : : "memory");
  if (eflags & 0x200) __asm__ volatile("sti");
  return "write-combining (PAT)";
}
#endif
/* a page of devices' registers (the local APIC's, ap.c) uncached, where
 * paging is on: the 2 MB or 4 MB page it is in (i386: pat_wc's tables, when it ran) */
#ifdef __x86_64__
void screen_uncached(u32 addr) {
  boot_pd[addr >> 21] |= 0x18;                          /* PCD, PWT: the PAT's entry 3, UC */
  uintptr_t cr3; __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
}
#else
void screen_uncached(u32 addr) {
  uintptr_t cr0; __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  if (!(cr0 & 0x80000000u)) return;                     /* no paging: the MTRRs have it */
  pdir[addr >> 22] |= 0x18;
  uintptr_t cr3; __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
}
#endif
/* what happened, for the console */
const char *screen_wc(void) {
  if (!fb) return "no framebuffer";
  u32 a, b, c, d;
  cpuid(0, &a, &b, &c, &d);
  if (a < 1) return "no CPUID 1";
  cpuid(1, &a, &b, &c, &d);
  if ((d & (1u << 16)) && (d & (1u << 3))) return pat_wc();     /* PAT, PSE */
  if (!(d & (1u << 12))) return "no MTRRs";
  uint64_t cap = rdmsr(0xFE);
  int vcnt = (int)(cap & 255);
  if (!(cap & (1u << 10))) return "no write-combining type";
  u32 phys = 36;
  cpuid(0x80000000u, &a, &b, &c, &d);
  if (a >= 0x80000008u) { cpuid(0x80000008u, &a, &b, &c, &d); phys = a & 255; }
  uint64_t pmask = ((uint64_t)1 << phys) - 1;
  u32 base = (u32)(uintptr_t)fb, need = fb_pitch * (u32)sh, size = 1u << 12;
  while (size < need && size < (1u << 30)) size <<= 1;
  base &= ~(size - 1);                    /* the range, aligned to its size */
  if ((u32)(uintptr_t)fb + need > base + size) { size <<= 1; base &= ~(size - 1); }
  int free_ = -1;
  for (int i = 0; i < vcnt; i++) {
    uint64_t pb = rdmsr(0x200 + 2 * i), pm = rdmsr(0x201 + 2 * i);
    if (!(pm & 0x800)) { if (free_ < 0) free_ = i; continue; }
    uint64_t m = pm & pmask & ~(uint64_t)0xfff;
    if (((uint64_t)base & m) == (pb & m)) {          /* a range over the framebuffer already */
      int t = (int)(pb & 0xff);
      return t == 1 ? "already write-combining" : t == 0 ? "uncached by the BIOS's own range" : "already cached";
    }
  }
  if (free_ < 0) return "no free MTRR";
  uintptr_t eflags; __asm__ volatile("pushf; pop %0; cli" : "=r"(eflags));
  uintptr_t cr0; __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
  __asm__ volatile("mov %0, %%cr0; wbinvd" : : "r"((cr0 | 0x40000000u) & ~0x20000000u) : "memory");
  uint64_t def = rdmsr(0x2FF);
  wrmsr(0x2FF, def & ~(uint64_t)0x800);
  wrmsr(0x200 + 2 * free_, (uint64_t)base | 1);
  wrmsr(0x201 + 2 * free_, (~(uint64_t)(size - 1) & pmask & ~(uint64_t)0xfff) | 0x800);
  __asm__ volatile("wbinvd" : : : "memory");
  wrmsr(0x2FF, def);
  __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
  if (eflags & 0x200) __asm__ volatile("sti");
  return "write-combining";
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

/* A row at a time: made in memory, then written to the framebuffer in
 * one run of stores. The framebuffer may be uncached (a BIOS's MTRRs
 * leave it so, and a VM's screen is then slow to write), and either way
 * a run is the fast way in; a pixel at a time, with a 64-bit division for
 * each, a 1024 x 768 frame cost more than the frame did */
static u32 *rowbuf; static int *xmap;
static inline int native(void) { return bpp == 32 && rpos == 16 && gpos == 8 && bpos == 0; }
static void row_out(int y, int x0, int x1) {
  u8 *p = fb + (size_t)y * fb_pitch;
  if (bpp == 32) memcpy(p + (size_t)x0 * 4, rowbuf + x0, (size_t)(x1 - x0) * 4);
  else if (bpp == 16) { u16 *q = (u16 *)p; for (int x = x0; x < x1; x++) q[x] = (u16)rowbuf[x]; }
  else { p += x0 * 3; for (int x = x0; x < x1; x++) { u32 v = rowbuf[x]; *p++ = (u8)v; *p++ = (u8)(v >> 8); *p++ = (u8)(v >> 16); } }
}
static void blank(void) {
  if (!rowbuf) rowbuf = malloc((size_t)sw * 4);
  memset(rowbuf, 0, (size_t)sw * 4);
  for (int y = 0; y < sh; y++) row_out(y, 0, sw);
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
  if (!xmap) xmap = malloc((size_t)sw * sizeof *xmap);
  for (int x = 0; x < sw; x++) { int sx = x < ox ? 0 : (x - ox) * den / num; xmap[x] = sx < w ? sx : w - 1; }
  blank();
}
/* screen pixels [x0, x1) x [y0, y1) from the display's front */
static void copy(const u32 *px, int x0, int y0, int x1, int y1) {
  int ex = ox + cw * num / den, ey = oy + ch * num / den;
  if (x0 < ox) x0 = ox;
  if (y0 < oy) y0 = oy;
  if (x1 > ex) x1 = ex;
  if (y1 > ey) y1 = ey;
  if (x1 <= x0 || y1 <= y0) return;
  int nat = native(), made = -1;
  for (int y = y0; y < y1; y++) {
    int sy = (y - oy) * den / num;
    const u32 *row = px + (size_t)sy * cw;
    if (num == den && nat) {
      memcpy(fb + (size_t)y * fb_pitch + (size_t)x0 * 4, row + (x0 - ox), (size_t)(x1 - x0) * 4);
      continue;
    }
    if (sy != made) {        /* a row scaled up is the same row again */
      if (nat) for (int x = x0; x < x1; x++) rowbuf[x] = row[xmap[x]];
      else for (int x = x0; x < x1; x++) rowbuf[x] = pix(row[xmap[x]]);
      made = sy;
    }
    row_out(y, x0, x1);
  }
}

/* ---- the console on it: pc.c's 80 x 25 text screen, drawn in DejaVu
 * Sans Mono while no program has the display. A GRUB may set a graphics
 * mode though the entry says "gfxpayload=text" (VMware's did), and a PC
 * may have no text mode at all; the VGA's text then is not on the screen,
 * and without this the screen showed nothing but the pointer ---- */
static const u32 VGA16[16] = {
  0x000000, 0x0000aa, 0x00aa00, 0x00aaaa, 0xaa0000, 0xaa00aa, 0xaa5500, 0xaaaaaa,
  0x555555, 0x5555ff, 0x55ff55, 0x55ffff, 0xff5555, 0xff55ff, 0xffff55, 0xffffff };
static u16 con_seen[2000]; static int con_on, con_cur = -1;
static int tw, th, tx0, ty0, tsize16, tbase;
static u32 *strip;
static void con_layout(void) {
  tw = sw / 80; if (tw < 5) tw = 5;
  th = sh / 25; if (th > tw * 2) th = tw * 2;
  tsize16 = tw * 16 * 2048 / 1233;                 /* the face's advance is 1233/2048 em */
  while (tsize16 > 64 && font_advance(0, tsize16, 'M') > tw * 64) tsize16 -= 4;
  int em = tsize16 / 16;
  tbase = (th - em) / 2 + font_top(0, tsize16);
  tx0 = (sw - 80 * tw) / 2; ty0 = (sh - 25 * th) / 2;
  if (tx0 < 0) tx0 = 0;
  if (ty0 < 0) ty0 = 0;
  if (!strip) strip = malloc((size_t)80 * tw * th * 4);
  if (!rowbuf) rowbuf = malloc((size_t)sw * 4);
}
static void con_row(const u16 *cells, int r, int cur) {
  int W = 80 * tw;
  for (int c = 0; c < 80; c++) {
    u16 v = cells[r * 80 + c];
    u32 fg = VGA16[(v >> 8) & 15], bg = VGA16[(v >> 12) & 15];
    u32 *cell = strip + c * tw;
    for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) cell[y * W + x] = bg;
    const glyph *gl = (v & 255) > 32 ? font_glyph(0, tsize16, v & 255) : 0;
    if (gl) for (int j = 0; j < gl->h; j++) {
      int y = tbase + gl->y0 + j;
      if (y < 0 || y >= th) continue;
      for (int i = 0; i < gl->w; i++) {
        int x = gl->x0 + i, a = gl->a[j * gl->w + i];
        if (x < 0 || x >= tw || !a) continue;
        u32 o = 0;
        for (int sh8 = 0; sh8 < 24; sh8 += 8) {
          u32 f = (fg >> sh8) & 255, b = (bg >> sh8) & 255;
          o |= ((b * (255 - (u32)a) + f * (u32)a + 127) / 255) << sh8;
        }
        cell[y * W + x] = o;
      }
    }
    if (r * 80 + c == cur) for (int y = th - 2 - th / 12; y < th - th / 12; y++) for (int x = 0; x < tw; x++) cell[y * W + x] = fg;
  }
  int nat = native();
  for (int y = 0; y < th && ty0 + r * th + y < sh; y++) {
    const u32 *s = strip + y * W;
    for (int x = 0; x < W && tx0 + x < sw; x++) rowbuf[tx0 + x] = nat ? s[x] : pix(s[x]);
    row_out(ty0 + r * th + y, tx0, tx0 + W < sw ? tx0 + W : sw);
  }
}
/* the text screen's cells (VGA's: character, then attribute), and the
 * cursor's cell (-1 for none): the rows that changed */
void screen_console(const u16 *cells, int cur) {
  if (!fb) return;
  if (!con_on) {
    con_layout(); blank();
    memset(con_seen, 0xff, sizeof con_seen); con_cur = -1;
    con_on = 1;
    cw = ch = 0;                       /* the display, when it comes back, all of it */
  }
  for (int r = 0; r < 25; r++) {
    int moved = cur != con_cur && (cur / 80 == r || con_cur / 80 == r);
    if (!moved && !memcmp(cells + r * 80, con_seen + r * 80, 160)) continue;
    con_row(cells, r, cur);
    memcpy(con_seen + r * 80, cells + r * 80, 160);
  }
  con_cur = cur;
}

/* an 80 x 25 text screen (vterm.h's cells) as pixels, 8 x 16 a cell, into
 * OUT (640 x 400): a hypervisor guest's, small in Machines (hv.c, THUMB) */
void screen_text_pixels(const u16 *cells, int cur, u32 *out) {
  enum { W = 8, H = 16 };
  static int size16 = -1, base;
  if (size16 < 0) {
    size16 = W * 16 * 2048 / 1233;
    while (size16 > 64 && font_advance(0, size16, 'M') > W * 64) size16 -= 4;
    base = (H - size16 / 16) / 2 + font_top(0, size16);
  }
  for (int i = 0; i < 2000; i++) {
    u16 v = cells[i];
    u32 fg = VGA16[(v >> 8) & 15], bg = VGA16[(v >> 12) & 15];
    u32 *cell = out + (i / 80) * H * 640 + (i % 80) * W;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) cell[y * 640 + x] = bg;
    const glyph *gl = (v & 255) > 32 ? font_glyph(0, size16, v & 255) : 0;
    if (gl) for (int j = 0; j < gl->h; j++) {
      int y = base + gl->y0 + j;
      if (y < 0 || y >= H) continue;
      for (int k = 0; k < gl->w; k++) {
        int x = gl->x0 + k, a = gl->a[j * gl->w + k];
        if (x < 0 || x >= W || !a) continue;
        u32 o = 0;
        for (int sh8 = 0; sh8 < 24; sh8 += 8) {
          u32 f = (fg >> sh8) & 255, b = (bg >> sh8) & 255;
          o |= ((b * (255 - (u32)a) + f * (u32)a + 127) / 255) << sh8;
        }
        cell[y * 640 + x] = o;
      }
    }
    if (i == cur) for (int y = H - 3; y < H - 1; y++) for (int x = 0; x < W; x++) cell[y * 640 + x] = fg;
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

/* the next look copies all of the display, not what changed: another
 * machine's is on the screen now (hv.c) */
void screen_full(void) { cw = ch = 0; }
/* what changed since the last look, and the pointer */
void screen_show(gui *g) {
  if (!fb) return;
  con_on = 0;
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

/* N frames a second, in a box in the screen's top right corner, over
 * whatever is there (pc.c: Ctrl+Alt+F11) */
void screen_fps(int fps) {
  if (!fb || con_on) return;
  char t[16]; int n = 0, d[8], k = 0, v = fps < 0 ? 0 : fps;
  do d[k++] = v % 10; while ((v /= 10) && k < 8);
  while (k) t[n++] = (char)('0' + d[--k]);
  const char *u = " fps";
  while (*u) t[n++] = *u++;
  int size16 = 16 * 16, em = 16, adv = 0;
  for (int i = 0; i < n; i++) adv += font_advance(0, size16, t[i]);
  int w = adv / 64 + 12, h = em + 10, x0 = sw - w - 6, y0 = 6, base = 5 + font_top(0, size16);
  if (x0 < 0) return;
  for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) put(x, y, pix(0x202020));
  int pen = (x0 + 6) * 64;
  for (int i = 0; i < n; i++) {
    const glyph *gl = font_glyph(0, size16, t[i]);
    if (gl) for (int j = 0; j < gl->h; j++) for (int q = 0; q < gl->w; q++) {
      int a = gl->a[j * gl->w + q];
      if (a < 96) continue;
      int x = pen / 64 + gl->x0 + q, y = y0 + base + gl->y0 + j;
      if (x >= 0 && x < sw && y >= 0 && y < sh) put(x, y, pix(a > 200 ? 0x40ff40 : 0x30a030));
    }
    pen += font_advance(0, size16, t[i]);
  }
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
  uintptr_t fl;
  __asm__ volatile("pushf; pop %0; cli" : "=r"(fl));
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
  /* VMware's absolute mouse (vmware.c): the PS/2 packets only say there
   * is something; the positions are the backdoor's */
  if (vmw_mouse()) {
    mq_head = mq_tail;
    if (vmw_mouse_poll(g, gui_locked(g))) return;
    pc_log("c4pc: VMware's mouse stopped answering: the PS/2 mouse's motion again\n");
  }
  while (mq_head != mq_tail) {
    mpkt p = *(mpkt *)&mq[mq_head & 255];
    mq_head++;
    gui_mouse(g, (int)((i64)p.dx * 256 * den / num), (int)((i64)p.dy * 256 * den / num), p.buttons, p.wheel);
    if (mouse_debug) { int x, y; gui_pointer(g, &x, &y); pc_log("pointer "); pc_logn(p.dx); pc_log(" "); pc_logn(p.dy); pc_log(" buttons "); pc_logn(p.buttons); pc_log(" now "); pc_logn(x); pc_log(" "); pc_logn(y); pc_log("#"); }
  }
}
