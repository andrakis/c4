/* pc.c -- c4pc on a bare PC: what the engine needs from the machine
 * (docs/c4pc.md).
 *
 *   - multiboot v1: the memory, the kernel's command line, and modules:
 *     the program first (a .c4r), then drives (ustar archives) and the
 *     keyboard's input, each named by its module string:
 *        c4ix32.c4r            the program; argv[0]
 *        drive [rw]            a drive, from a tar (rw: writable, in RAM)
 *        stdin                 fed to the keyboard up front, then end of input
 *   - the command line: cli.js's flags, -m MB -c CYCLES -s -hz MHZ, then
 *     the program's arguments after --
 *   - a flat GDT, an IDT, the 8259s remapped to 0x20, the PIT at 1 kHz
 *   - COM1 at 115200 both ways, VGA text with enough ANSI for a shell,
 *     the 8042 keyboard
 *   - when the machine stops: its status out through QEMU's isa-debug-exit
 *     (port 0xF4), then ACPI power-off for QEMU and Bochs, then a halt.
 */
#include "../c4pc.h"
#include "../clib.h"

/* the clock -hz auto sets: this share, in percent, of machine_kips's
 * loop rate (pc_main) */
#define CLOCK_SHARE 75

void pc_malloc_init(void *lo, void *hi);
extern u32 pc_heap_free_bytes;
extern char _end[];
void gdt_load(void *ptr);

static inline void outb(u32 p, u8 v) { __asm__ volatile("outb %0,%w1" : : "a"(v), "Nd"(p)); }
static inline void outw(u32 p, uint16_t v) { __asm__ volatile("outw %0,%w1" : : "a"(v), "Nd"(p)); }
static inline u8 inb(u32 p) { u8 v; __asm__ volatile("inb %w1,%0" : "=a"(v) : "Nd"(p)); return v; }

/* ---- the console: COM1 and the VGA text screen ---- */
static int have_com1;
static void com1_init(void) {
  outb(0x3F9, 0x00);                 /* no interrupts while set up */
  outb(0x3FB, 0x80); outb(0x3F8, 1); outb(0x3F9, 0);   /* 115200 */
  outb(0x3FB, 0x03);                 /* 8N1 */
  outb(0x3FA, 0xC7);                 /* FIFOs */
  outb(0x3FC, 0x0B);                 /* DTR, RTS, OUT2 (the IRQ line) */
  have_com1 = inb(0x3FD) != 0xFF;
  if (have_com1) outb(0x3F9, 0x01);  /* an interrupt when a byte comes in */
}
static void com1_put(int c) {
  if (!have_com1) return;
  for (int n = 0; n < 100000 && !(inb(0x3FD) & 0x20); n++) ;
  outb(0x3F8, (u8)c);
}

/* the text screen, kept here (vtext) and shown on the VGA's while it is
 * in text mode; on a framebuffer, screen.c draws it (screen_console) */
static volatile uint16_t *const VGA = (volatile uint16_t *)0xB8000;
static uint16_t vtext[2000]; static int vga_real = 1;
#define VSET(i, v) do { int i_ = (i); vtext[i_] = (uint16_t)(v); if (vga_real) VGA[i_] = vtext[i_]; } while (0)
static int vx, vy; static uint16_t vattr = 0x0700;
static int esc_state; static int esc_args[8], esc_n;
static void vga_cursor(void) {
  u32 pos = (u32)(vy * 80 + vx);
  outb(0x3D4, 0x0F); outb(0x3D5, (u8)pos); outb(0x3D4, 0x0E); outb(0x3D5, (u8)(pos >> 8));
}
static void vga_scroll(void) {
  memmove(vtext, vtext + 80, 80 * 24 * 2);
  for (int i = 80 * 24; i < 80 * 25; i++) vtext[i] = vattr | ' ';
  if (vga_real) for (int i = 0; i < 80 * 25; i++) VGA[i] = vtext[i];
  vy = 24;
}
static void vga_clear(int from, int to) { for (int i = from; i < to; i++) VSET(i, vattr | ' '); }
static const u8 ansi_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
static void vga_csi(int c) {
  int a0 = esc_n > 0 ? esc_args[0] : 0, a1 = esc_n > 1 ? esc_args[1] : 0;
  switch (c) {
    case 'A': vy -= a0 ? a0 : 1; if (vy < 0) vy = 0; break;
    case 'B': vy += a0 ? a0 : 1; if (vy > 24) vy = 24; break;
    case 'C': vx += a0 ? a0 : 1; if (vx > 79) vx = 79; break;
    case 'D': vx -= a0 ? a0 : 1; if (vx < 0) vx = 0; break;
    case 'H': case 'f': vy = (a0 ? a0 : 1) - 1; vx = (a1 ? a1 : 1) - 1;
      if (vy > 24) vy = 24;
      if (vx > 79) vx = 79;
      break;
    case 'J':
      if (a0 == 2) { vga_clear(0, 2000); }
      else if (a0 == 1) vga_clear(0, vy * 80 + vx + 1);
      else vga_clear(vy * 80 + vx, 2000);
      break;
    case 'K':
      if (a0 == 2) vga_clear(vy * 80, vy * 80 + 80);
      else if (a0 == 1) vga_clear(vy * 80, vy * 80 + vx + 1);
      else vga_clear(vy * 80 + vx, vy * 80 + 80);
      break;
    case 'm':
      if (!esc_n) { vattr = 0x0700; break; }
      for (int i = 0; i < esc_n; i++) {
        int v = esc_args[i];
        if (v == 0) vattr = 0x0700;
        else if (v == 1) vattr |= 0x0800;
        else if (v == 7) vattr = (uint16_t)(((vattr & 0x0f00) << 4) | ((vattr & 0xf000) >> 4));
        else if (v >= 30 && v <= 37) vattr = (uint16_t)((vattr & 0xf800) | ansi_vga[v - 30] << 8);
        else if (v >= 40 && v <= 47) vattr = (uint16_t)((vattr & 0x0fff) | ansi_vga[v - 40] << 12);
        else if (v == 39) vattr = (uint16_t)((vattr & 0xf800) | 0x0700);
        else if (v == 49) vattr &= 0x0fff;
      }
      break;
  }
}
static void vga_put(int c) {
  if (esc_state == 1) {
    if (c == '[') { esc_state = 2; esc_n = 0; esc_args[0] = 0; return; }
    esc_state = 0; return;
  }
  if (esc_state == 2) {
    if (c >= '0' && c <= '9') { if (!esc_n) esc_n = 1; esc_args[esc_n - 1] = esc_args[esc_n - 1] * 10 + (c - '0'); return; }
    if (c == ';') { if (!esc_n) esc_n = 1; if (esc_n < 8) esc_args[esc_n++] = 0; return; }
    if (c == '?') return;
    esc_state = 0;
    vga_csi(c);
    vga_cursor();
    return;
  }
  if (c == 27) { esc_state = 1; return; }
  if (c == '\n') { vx = 0; if (++vy > 24) vga_scroll(); }
  else if (c == '\r') vx = 0;
  else if (c == '\b') { if (vx) vx--; }
  else if (c == '\t') { vx = (vx + 8) & ~7; if (vx > 79) { vx = 0; if (++vy > 24) vga_scroll(); } }
  else if (c >= 32 || c < 0) {
    VSET(vy * 80 + vx, vattr | (u8)c);
    if (++vx > 79) { vx = 0; if (++vy > 24) vga_scroll(); }
  }
}
static int vga_dirty, com1_only;
static void con_put(int c) {
  if (c == '\n') com1_put('\r');
  com1_put(c);
  if (com1_only) return;
  vga_put(c);
  vga_dirty = 1;
}
static void con_puts(const char *s) { while (*s) con_put(*s++); }
static void con_flush(void) { if (vga_dirty) { vga_cursor(); vga_dirty = 0; } }
static void con_dec(i64 v) { char t[24]; int n = 0; int neg = v < 0; uint64_t u = neg ? (uint64_t)-v : (uint64_t)v;
  do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u); if (neg) con_put('-'); while (n) con_put(t[--n]); }
void pc_log(const char *s) { con_puts(s); }
static machine *dbg_m;
static void trace_event(i32 type, const i32 *pl, int n, int sent) {
  con_puts("event "); con_dec(type);
  if (dbg_m) { con_puts(" sim "); con_dec(machine_sim_ms(dbg_m)); con_puts(" real "); con_dec(plat_now_ms()); con_puts(" nap "); con_dec(dbg_m->nap_ms); con_puts(" :"); }
  for (int i = 0; i < n; i++) { con_puts(" "); con_dec(pl[i]); }
  con_puts(sent ? " sent#" : " masked#");
}
void pc_logn(i64 v) { con_dec(v); }
static void con_hex(u32 v) { con_puts("0x"); for (int i = 7; i >= 0; i--) con_put("0123456789abcdef"[(v >> (4 * i)) & 15]); }

/* ---- leaving ---- */
static void power_off(int status) {
  con_flush();
  outb(0xF4, (u8)(status & 0x7f));   /* QEMU's isa-debug-exit: exits (v << 1) | 1 */
  outw(0x604, 0x2000);               /* QEMU's ACPI power-off */
  outw(0xB004, 0x2000);              /* Bochs's, and older QEMU's */
  outw(0x4004, 0x3400);              /* VirtualBox's */
  for (;;) __asm__ volatile("cli; hlt");
}
void plat_fatal(const char *msg) { con_puts(msg); con_puts("\n"); power_off(70); for (;;) ; }

/* ---- the clock: the PIT's channel 0 at 1 kHz ---- */
static volatile i64 ticks;
i64 plat_now_ms(void) {
  i64 t;
  do { t = ticks; } while (t != ticks);       /* two words: read until they agree */
  return t;
}

/* the local time of day from the RTC (CMOS: BCD unless status B says
 * binary, 12-hour unless it says 24), for the display's CLOCK */
static u8 cmos(u8 r) { outb(0x70, r); return inb(0x71); }
i32 plat_clock_secs(void) {
  u8 s, mi, h, b;
  while (cmos(0x0a) & 0x80) ;                 /* not while it updates */
  s = cmos(0x00); mi = cmos(0x02); h = cmos(0x04); b = cmos(0x0b);
  int pm = h & 0x80; h &= 0x7f;
  if (!(b & 4)) { s = (s >> 4) * 10 + (s & 15); mi = (mi >> 4) * 10 + (mi & 15); h = (h >> 4) * 10 + (h & 15); }
  if (!(b & 2)) { if (h == 12) h = 0; if (pm) h += 12; }
  return h * 3600 + mi * 60 + s;
}

/* ---- input: what the interrupts bring, for the run loop ---- */
static volatile u8 inq[4096];
static volatile u32 inq_head, inq_tail;
static void inq_push(int c) {
  u32 t = inq_tail;
  if (t - inq_head >= sizeof inq) return;
  inq[t % sizeof inq] = (u8)c;
  inq_tail = t + 1;
}

/* scancode set 1 to ASCII, unshifted and shifted */
static const char km_lo[128] = {
  0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, '\t',
  'q','w','e','r','t','y','u','i','o','p','[',']', '\n', 0, 'a','s',
  'd','f','g','h','j','k','l',';','\'','`', 0, '\\','z','x','c','v',
  'b','n','m',',','.','/', 0, '*', 0, ' ', 0 };
static const char km_hi[128] = {
  0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', 8, '\t',
  'Q','W','E','R','T','Y','U','I','O','P','{','}', '\n', 0, 'A','S',
  'D','F','G','H','J','K','L',':','"','~', 0, '|','Z','X','C','V',
  'B','N','M','<','>','?', 0, '*', 0, ' ', 0 };
static int k_shift, k_ctrl, k_caps, k_e0;
static volatile int kbd_to_gui;
static volatile u8 scq[256];
static volatile u32 scq_head, scq_tail;
void mouse_byte(u8 b);
static void kbd_irq(void) {
  if (inb(0x64) & 0x20) { mouse_byte(inb(0x60)); return; }     /* the mouse's byte, after all */
  u8 sc = inb(0x60);
  if (kbd_to_gui) { u32 t = scq_tail; if (t - scq_head < sizeof scq) { scq[t % sizeof scq] = sc; scq_tail = t + 1; } return; }
  if (sc == 0xE0) { k_e0 = 1; return; }
  int up = sc & 0x80; sc &= 0x7f;
  int e0 = k_e0; k_e0 = 0;
  if (sc == 0x2A || sc == 0x36) { k_shift = !up; return; }
  if (sc == 0x1D) { k_ctrl = !up; return; }
  if (up) return;
  if (sc == 0x3A) { k_caps = !k_caps; return; }
  if (e0 || (sc >= 0x47 && sc <= 0x53)) {
    const char *seq = 0;
    switch (sc) {
      case 0x48: seq = "\033[A"; break; case 0x50: seq = "\033[B"; break;
      case 0x4D: seq = "\033[C"; break; case 0x4B: seq = "\033[D"; break;
      case 0x47: seq = "\033[H"; break; case 0x4F: seq = "\033[F"; break;
      case 0x53: seq = "\033[3~"; break;
      case 0x1C: inq_push('\n'); return;
      case 0x35: inq_push('/'); return;
    }
    if (seq) while (*seq) inq_push(*seq++);
    return;
  }
  char c = k_shift ? km_hi[sc] : km_lo[sc];
  if (!c) return;
  if (k_caps && c >= 'a' && c <= 'z') c -= 32;
  else if (k_caps && c >= 'A' && c <= 'Z' && k_shift) c += 32;
  if (k_ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c &= 0x1f;
  inq_push(c);
}

/* ---- the keyboard to the display: while a program has the display's
 * keys (gui_attached), scancodes go to it as the page's keydown and keyup:
 * the DOM's keyCode, the character typed (0 for none) and the modifiers
 * (1 shift, 2 ctrl, 4 alt); COM1 stays the console's ---- */
int mouse_init(void);
int mouse_pending(void);
void mouse_poll(gui *g);
int screen_from_multiboot(const void *info);
int screen_bga(int w, int h);
void screen_size(int *w, int *h);
void screen_show(gui *g);
void screen_console(const uint16_t *cells, int cur);
int gui_ring(gui *g);
u32 gui_flips(gui *g);
/* set 1 to keyCode: plain, and after E0 */
static const u8 kc[0x59] = {
  0, 27, 49, 50, 51, 52, 53, 54, 55, 56, 57, 48, 189, 187, 8, 9,
  81, 87, 69, 82, 84, 89, 85, 73, 79, 80, 219, 221, 13, 17, 65, 83,
  68, 70, 71, 72, 74, 75, 76, 186, 222, 192, 16, 220, 90, 88, 67, 86,
  66, 78, 77, 188, 190, 191, 16, 106, 18, 32, 20, 112, 113, 114, 115, 116,
  117, 118, 119, 120, 121, 144, 145, 36, 38, 33, 109, 37, 12, 39, 107, 35,
  40, 34, 45, 46, 0, 0, 0, 122, 123 };
static int g_shift, g_ctrl, g_alt, g_caps, g_e0;
static void key_event(gui *g, u8 sc) {
  if (sc == 0xE0) { g_e0 = 1; return; }
  int up = sc & 0x80, e0 = g_e0; sc &= 0x7f; g_e0 = 0;
  if (sc >= sizeof kc) return;
  int code = kc[sc];
  if (e0) {
    if (sc == 0x35) code = 111; else if (sc == 0x5B) code = 91;
    else if (sc == 0x2A || sc == 0x37) return;             /* PrtSc's fake shifts */
  }
  if (!code) return;
  if (code == 16) g_shift = !up;
  if (code == 17) g_ctrl = !up;
  if (code == 18) g_alt = !up;
  if (code == 20 && !up) g_caps = !g_caps;
  int ch = 0;
  if (!e0 && sc < 0x3A && sc != 0x1D && sc != 0x2A && sc != 0x36 && sc != 0x38) {
    ch = (u8)(g_shift ? km_hi[sc] : km_lo[sc]);
    if (g_caps && ch >= 'a' && ch <= 'z') ch -= 32;
    else if (g_caps && ch >= 'A' && ch <= 'Z' && g_shift) ch += 32;
    if (ch == '\n') ch = 10;
  } else if (e0 && sc == 0x1C) ch = 10;
  else if (e0 && sc == 0x35) ch = '/';
  gui_key(g, !up, code, ch, (g_shift ? 1 : 0) | (g_ctrl ? 2 : 0) | (g_alt ? 4 : 0));
}
/* after a slice: the keys and the mouse to the display, and what it drew
 * to the screen -- or, while no program has it (no ring, no frame flipped
 * this last second), the console's text screen */
static i64 display_ms;                  /* the time this takes, for -s */
static void display_tick(machine *m) {
  gui *g = m->gui;
  i64 t = plat_now_ms();
  kbd_to_gui = gui_attached(g);
  while (scq_head != scq_tail) { u8 sc = scq[scq_head % sizeof scq]; scq_head++; key_event(g, sc); }
  mouse_poll(g);
  gui_tick(g);
  static u32 flips; static i64 flip_at = -100000;
  if (gui_flips(g) != flips) { flips = gui_flips(g); flip_at = t; }
  if (gui_ring(g) || t - flip_at < 1000) screen_show(g);
  else screen_console(vtext, vy * 80 + vx);
  display_ms += plat_now_ms() - t;
}

/* ---- the CPU: GDT, IDT, PIC, PIT ---- */
struct regs { u32 edi, esi, ebp, esp, ebx, edx, ecx, eax, vec, err, eip, cs, eflags; };
static u32 gdt[6];
static struct { uint16_t lim; u32 base; } __attribute__((packed)) gdtr, idtr;
static u32 idt[256 * 2];

static void pic_eoi(u32 irq) { if (irq >= 8) outb(0xA0, 0x20); outb(0x20, 0x20); }

void isr(struct regs *r) {
  if (r->vec < 32) {
    static const char *names[] = { "divide error", "debug", "NMI", "breakpoint", "overflow", "bound", "invalid opcode",
      "no FPU", "double fault", "FPU segment", "bad TSS", "no segment", "stack fault", "general protection",
      "page fault", "?", "x87 error", "alignment", "machine check", "SIMD" };
    con_puts("\nc4pc: CPU exception ");
    con_dec(r->vec); con_puts(" ("); con_puts(r->vec < 20 ? names[r->vec] : "?");
    con_puts(") at eip "); con_hex(r->eip); con_puts(", error "); con_hex(r->err);
    con_puts(". Halting.\n");
    power_off(71);
  }
  u32 irq = r->vec - 32;
  if (irq == 0) ticks++;
  else if (irq == 1) kbd_irq();
  else if (irq == 12) { u8 st = inb(0x64); if (st & 1) { u8 b = inb(0x60); if (st & 0x20) mouse_byte(b); } }
  else if (irq == 4) { while (inb(0x3FD) & 1) { int c = inb(0x3F8); inq_push(c == '\r' ? '\n' : c); } }
  else if (irq == 7 || irq == 15) {       /* spurious: no EOI for the one that did not happen */
    if (irq == 7) { outb(0x20, 0x0B); if (!(inb(0x20) & 0x80)) return; }
    else { outb(0xA0, 0x0B); if (!(inb(0xA0) & 0x80)) { outb(0x20, 0x20); return; } }
  }
  pic_eoi(irq);
}

static void idt_set(int n, void (*f)(void)) {
  u32 a = (u32)(uintptr_t)f;
  idt[n * 2] = (a & 0xffff) | (0x08 << 16);
  idt[n * 2 + 1] = (a & 0xffff0000) | 0x8E00;
}

#define STUBS(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) \
  X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39) X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47)
#define DECL(n) void isr##n(void);
STUBS(DECL)

static void cpu_init(void) {
  /* flat 4 GB code and data, ring 0 */
  gdt[0] = 0; gdt[1] = 0;
  gdt[2] = 0x0000ffff; gdt[3] = 0x00cf9a00;
  gdt[4] = 0x0000ffff; gdt[5] = 0x00cf9200;
  gdtr.lim = sizeof gdt - 1; gdtr.base = (u32)(uintptr_t)gdt;
  gdt_load(&gdtr);
#define SET(n) idt_set(n, isr##n);
  STUBS(SET)
  idtr.lim = sizeof idt - 1; idtr.base = (u32)(uintptr_t)idt;
  __asm__ volatile("lidt %0" : : "m"(idtr));
  /* the 8259s: IRQ 0-7 at 0x20, 8-15 at 0x28 */
  outb(0x20, 0x11); outb(0xA0, 0x11);
  outb(0x21, 0x20); outb(0xA1, 0x28);
  outb(0x21, 0x04); outb(0xA1, 0x02);
  outb(0x21, 0x01); outb(0xA1, 0x01);
  outb(0x21, ~(1 | 2 | 4 | 16) & 0xff);   /* PIT, keyboard, the cascade, COM1 */
  outb(0xA1, 0xff);
  /* the PIT: channel 0, rate generator, 1193182 / 1193 ~ 1000 Hz */
  outb(0x43, 0x34); outb(0x40, 1193 & 0xff); outb(0x40, 1193 >> 8);
  /* the x87: every exception masked, rounding to double (fpu.c) */
  uint16_t cw = 0x027F;
  __asm__ volatile("fninit; fldcw %0" : : "m"(cw));
  /* the keyboard's buffer, emptied */
  for (int i = 0; i < 16 && (inb(0x64) & 1); i++) (void)inb(0x60);
}

/* ---- multiboot ---- */
struct mb_info { u32 flags, mem_lower, mem_upper, boot_device, cmdline, mods_count, mods_addr; };
struct mb_mod { u32 start, end, string, reserved; };

static void on_byte(machine *m, int b) { (void)m; con_put(b); }

static int split_args(char *s, char **argv, int max) {
  int n = 0;
  while (*s && n < max) {
    while (*s == ' ') s++;
    if (!*s) break;
    argv[n++] = s;
    while (*s && *s != ' ') s++;
    if (*s) *s++ = 0;
  }
  return n;
}

void pc_main(u32 magic, struct mb_info *mb) {
  com1_init();
  vga_clear(0, 2000);
  cpu_init();
  con_puts("c4pc: the c4 machine on a PC\n");
  if (magic != 0x2BADB002) { con_puts("c4pc: not started by a multiboot loader\n"); power_off(72); }

  /* memory: everything above the kernel and its modules, to the top of
   * what the loader says there is */
  u32 lo = (u32)(uintptr_t)_end;
  struct mb_mod *mods = (mb->flags & 8) ? (struct mb_mod *)(uintptr_t)mb->mods_addr : 0;
  u32 nmods = mods ? mb->mods_count : 0;
  for (u32 i = 0; i < nmods; i++) {
    if (mods[i].end > lo) lo = mods[i].end;
    u32 s = mods[i].string;
    if (s) { u32 e = s + (u32)strlen((char *)(uintptr_t)s) + 1; if (e > lo && s >= (u32)(uintptr_t)_end) lo = e; }
  }
  if (mb->flags & 4) { u32 s = mb->cmdline, e = s + (u32)strlen((char *)(uintptr_t)s) + 1; if (e > lo && s >= (u32)(uintptr_t)_end) lo = e; }
  u32 hi = 0x100000 + (mb->flags & 1 ? mb->mem_upper : 0) * 1024u;
  if (hi <= lo + (4u << 20)) { con_puts("c4pc: too little memory\n"); power_off(73); }
  pc_malloc_init((void *)(uintptr_t)lo, (void *)(uintptr_t)hi);
  con_puts("c4pc: "); con_dec((hi - lo) >> 20); con_puts(" MB free above the modules, ");
  con_dec(nmods); con_puts(" modules\n");

  __asm__ volatile("sti");

  /* the command line: the loader's own first word is the kernel's path */
  static char cmd[1024];
  cmd[0] = 0;
  if (mb->flags & 4) { const char *s = (const char *)(uintptr_t)mb->cmdline; size_t n = strlen(s); if (n > 1023) n = 1023; memcpy(cmd, s, n); cmd[n] = 0; }
  char *args[64]; int nargs = split_args(cmd, args, 64);
  u32 mb_arena = 32; i64 maxc = -1; double mhz = -1 /* auto */; int stats = 0, nofpu = 0, fast = 0, realtime = 1, wc = 1, mmu_on = 0, noaccel = 0, noosaccel = 0, gui_w = 0, gui_h = 0;
  char *pargv[64]; int pargc = 0;
  int a = 0;
  if (nargs && args[0][0] != '-') a = 1;       /* the kernel's own name */
  for (; a < nargs; a++) {
    if (!strcmp(args[a], "-m") && a + 1 < nargs) { mb_arena = 0; for (char *p = args[++a]; *p; p++) mb_arena = mb_arena * 10 + (u32)(*p - '0'); }
    else if (!strcmp(args[a], "-c") && a + 1 < nargs) { maxc = 0; for (char *p = args[++a]; *p; p++) maxc = maxc * 10 + (*p - '0'); }
    else if (!strcmp(args[a], "-hz") && a + 1 < nargs && !strcmp(args[a + 1], "auto")) { mhz = -1; a++; }
    else if (!strcmp(args[a], "-hz") && a + 1 < nargs) { mhz = 0; for (char *p = args[++a]; *p; p++) mhz = mhz * 10 + (*p - '0'); }
    else if (!strcmp(args[a], "-s")) stats = 1;
    else if (!strcmp(args[a], "--no-fpu")) nofpu = 1;
    else if (!strcmp(args[a], "--debug-mouse")) { extern int mouse_debug; extern void (*gui_trace)(i32, const i32 *, int, int); mouse_debug = 1; gui_trace = trace_event; }
    else if (!strcmp(args[a], "--gui") && a + 1 < nargs) {      /* WxH: a display, by the BGA where GRUB set none */
      char *p = args[++a]; gui_w = 0; gui_h = 0;
      while (*p >= '0' && *p <= '9') gui_w = gui_w * 10 + (*p++ - '0');
      if (*p == 'x') p++;
      while (*p >= '0' && *p <= '9') gui_h = gui_h * 10 + (*p++ - '0');
    }
    else if (!strcmp(args[a], "--no-accel")) noaccel = 1;
    else if (!strcmp(args[a], "--no-osaccel")) noosaccel = 1;
    else if (!strcmp(args[a], "--fast")) fast = 1;
    else if (!strcmp(args[a], "--no-realtime")) realtime = 0;
    else if (!strcmp(args[a], "--no-wc")) wc = 0;
    else if (!strcmp(args[a], "--mmu")) mmu_on = 1;
    else if (!strcmp(args[a], "--no-mmu")) mmu_on = 0;
    else if (!strcmp(args[a], "--")) { a++; break; }
    else break;
  }

  /* the modules: the program, the drives, the input */
  drive *drives[16]; int nd = 0;
  u8 *img = 0; u32 img_len = 0;
  const char *img_name = "c4pc.c4r";
  u8 *in = 0; u32 in_len = 0; int have_in = 0;
  for (u32 i = 0; i < nmods; i++) {
    u8 *p = (u8 *)(uintptr_t)mods[i].start; u32 n = mods[i].end - mods[i].start;
    char *s = mods[i].string ? (char *)(uintptr_t)mods[i].string : "";
    /* QEMU passes the whole "-initrd" item as the string: the path, then words */
    char *w[8]; static char buf[16][256];
    size_t sl = strlen(s); if (sl > 255) sl = 255;
    memcpy(buf[i & 15], s, sl); buf[i & 15][sl] = 0;
    int nw = split_args(buf[i & 15], w, 8);
    const char *kind = nw > 1 ? w[1] : nw ? w[0] : "";
    if (!strcmp(kind, "drive") && nd < 16) {
      drive *d = drive_new(0);
      if (nw > 2 && !strcmp(w[2], "rw")) { d->writable = 1; d->sink = 0; }
      int k = drive_add_tar(d, p, n);
      con_puts("c4pc: drive "); con_dec(nd); con_puts(": "); con_dec(k); con_puts(" files, ");
      con_dec(n >> 10); con_puts(" KB\n");
      drives[nd++] = d;
    } else if (!strcmp(kind, "stdin")) { in = p; in_len = n; have_in = 1; }
    else if (!img) {
      img = p; img_len = n;
      const char *nm = nw ? w[0] : "c4pc.c4r";
      const char *sl2 = strrchr(nm, '/');
      img_name = sl2 ? sl2 + 1 : nm;
    }
  }
  if (!img) { con_puts("c4pc: no program: give one as the first module\n"); power_off(74); }
  if (!nd) drives[nd++] = drive_new(0);
  /* a writable drive keeps what is written in RAM: the drive itself is
   * the copy (a sink that does nothing makes it writable) */
  extern void ram_sink(drive *, const char *, blob *);
  for (int i = 0; i < nd; i++) if (drives[i]->writable) drives[i]->sink = ram_sink;

  pargv[pargc++] = (char *)img_name;
  for (; a < nargs && pargc < 63; a++) pargv[pargc++] = args[a];

  /* the clock. Scripted (a stdin module) or --fast, libjs's 20 MHz, so
   * that a run is cycle for cycle libjs's; at the keyboard, unless -hz
   * names one, what this PC keeps up with (engine.c machine_kips): the
   * loop's rate (measured: within a fifth of C4IX's own, under KVM and
   * under TCG), less a quarter, so that the clock mostly keeps time; a
   * machine that falls behind its clock only runs slow */
  int paced = !have_in && !fast;
  if (mhz < 0 && !paced) mhz = 20;
  if (mhz < 0) {
    u32 sz = 16u << 20;                 /* the scratch arena, freed again */
    while (sz > (1u << 20) && sz + (2u << 20) > pc_heap_free_bytes) sz >>= 1;
    u32 k = machine_kips(100, sz);
    u32 hz = (u32)((i64)k * CLOCK_SHARE / 100000);    /* MHz */
    if (hz < 2) hz = 2;
    if (hz > 1000) hz = 1000;
    mhz = hz;
    con_puts("c4pc: "); con_dec(k / 1000); con_puts("M c4 instructions a second here, so a ");
    con_dec(hz); con_puts(" MHz clock (-hz N to choose)\n");
  }
  machine *m = machine_new(mb_arena << 20, drives, nd, mhz);
  m->on_byte = on_byte;
  m->no_fpu = nofpu;
  if (noaccel) m->accel = 0;
  if (noosaccel) m->osaccel = 0;
  if (mmu_on) mmu_new(m);
  /* the display: GRUB's framebuffer, or the BGA's at --gui's size */
  if (screen_from_multiboot(mb) || (gui_w > 0 && gui_h > 0 && screen_bga(gui_w, gui_h))) {
    int w, h; screen_size(&w, &h);
    { extern void screen_describe(void); screen_describe(); }
    if (wc) { extern const char *screen_wc(void); con_puts("c4pc: the framebuffer: "); con_puts(screen_wc()); con_puts("\n"); }
    m->gui = gui_new(m, w, h);
    dbg_m = m;
    int mouse = mouse_init();
    if (mouse) outb(0xA1, 0xEF);           /* IRQ 12 */
    con_puts("c4pc: a display of "); con_dec(w); con_puts(" x "); con_dec(h);
    con_puts(mouse ? ", and a mouse\n" : ", and no mouse\n");
    vga_real = 0;                       /* graphics: the VGA's text is not shown */
    screen_console(vtext, vy * 80 + vx);
  }
  if (have_in) { for (u32 i = 0; i < in_len; i++) kbd_push(m, in[i]); m->rx_eof = 1; }
  con_puts("c4pc: "); con_dec(m->size >> 20); con_puts(mmu_on ? " MB arena with an MMU, running " : " MB arena, running ");
  con_puts(img_name); con_puts("\n");
  if (machine_boot(m, img, img_len, pargc, pargv)) power_off(75);

  i64 t0 = plat_now_ms(), parked = 0, behind_ms = 0;
  i64 limit = maxc < 0 ? INT64_MAX : maxc;
  int status;
  for (;;) {
    /* what the keyboard and COM1 brought: cooked as a terminal would,
     * unless a program has the raw keyboard open (cli.js -i) */
    while (inq_head != inq_tail) {
      int c = inq[inq_head % sizeof inq]; inq_head++;
      if (have_in) continue;                  /* scripted: the keys are not the program's */
      if (c == 4) { m->rx_eof = 1; continue; }
      if (c == 3) { if (m->sigh[2]) m->pending_signal = 2; continue; }
      if (m->raw_kbd > 0) { kbd_push(m, c); continue; }
      if (c == 127 || c == 8) {
        if (m->rx_tail > m->rx_head && m->rx[m->rx_tail - 1] != 10) { m->rx_tail--; con_puts("\b \b"); }
        continue;
      }
      kbd_push(m, c);
      con_put(c);
    }
    con_flush();
    i64 left = limit - m->cycle;
    if (left <= 0) { con_puts("c4pc: stopped at cycle budget\n"); status = 9; break; }
    /* someone at the keyboard: slices of 4 simulated milliseconds (as
     * libjs/worker.js's), paced to the machine's own clock (cli.js -i), so
     * that a simulated second takes a real one and top redraws once a
     * second; scripted, or with --fast, as fast as it goes */
    i64 slice = paced ? (i64)m->cycles_per_ms * 4 : 20000000;
    int r = machine_run(m, left < slice ? left : slice);
    if (m->gui) display_tick(m);
    /* -s at the keyboard: every 10 s, on COM1 alone, how the PC kept up */
    if (stats && paced) {
      static i64 rep_at, rep_cyc, rep_disp, rep_behind;
      i64 now = plat_now_ms();
      if (!rep_at) { rep_at = now; rep_cyc = m->cycle - m->skipped; }
      else if (now - rep_at >= 10000) {
        i64 dt = now - rep_at, ran = m->cycle - m->skipped;
        com1_only = 1;
        con_puts("c4pc: "); con_dec((ran - rep_cyc) / dt); con_puts("k inst/s, ");
        con_dec((display_ms - rep_disp) * 100 / dt); con_puts("% drawing to the screen, ");
        con_dec((behind_ms - rep_behind) * 100 / dt); con_puts("% of the clock skipped\n");
        com1_only = 0;
        rep_at = now; rep_cyc = ran; rep_disp = display_ms; rep_behind = behind_ms;
      }
    }
    if (r == STOP_HALT) { status = m->status; break; }
    if (r == STOP_INPUT) {
      if (have_in) { con_puts("c4pc: blocked on input with nothing left to read\n"); status = 8; break; }
      con_flush();
      i64 t = plat_now_ms();
      __asm__ volatile("sti; hlt");           /* until a key, or the next tick */
      parked += plat_now_ms() - t;
      continue;
    }
    if (paced) {
      i64 ahead = machine_sim_ms(m) - (plat_now_ms() - t0 - parked);
      if (ahead > 2) {
        con_flush();
        i64 until = plat_now_ms() + (ahead < 100 ? ahead : 100);
        while (plat_now_ms() < until && inq_head == inq_tail && scq_head == scq_tail && !mouse_pending()) __asm__ volatile("sti; hlt");
        /* input during a nap: the guest wakes now, and its clock gets only
         * what it slept (libjs/worker.js wake). Kept whole, every pointer
         * move gave it a nap's worth of time, and two clicks a moment apart
         * were seconds apart to the desktop: never a double click */
        if (r == STOP_SLEEP && (inq_head != inq_tail || scq_head != scq_tail || mouse_pending())) {
          i64 a2 = machine_sim_ms(m) - (plat_now_ms() - t0 - parked);
          if (a2 > 0) { i64 cut = a2 < m->nap_ms ? a2 : m->nap_ms; m->sleep_ms -= cut; m->nap_ms -= cut; }
        }
      }
      /* Behind: the machine's clock is the wall's (libjs's realtime, the
       * page's default). TIME moves on by what the CPU could not get done,
       * so a PC slower than the clock asked for gives the guest fewer
       * cycles a second, not a slower second: a game drops frames rather
       * than playing in slow motion. What falls behind: the games' own
       * work, dearer than -hz auto's loop, and the drawing and the screen.
       * --no-realtime: the machine's second stretches instead */
      else if (realtime && ahead < -20) { m->sleep_ms += -ahead; behind_ms += -ahead; }
      else if (!realtime && ahead < -1000) parked += -ahead - 1000;
    }
  }
  con_flush();
  if (stats) {
    i64 dt = plat_now_ms() - t0;
    i64 ran = m->cycle - m->skipped;
    con_puts("c4pc: "); con_dec(ran); con_puts(" cycles in "); con_dec(dt); con_puts(" ms (");
    con_dec(dt > 0 ? ran / dt : 0); con_puts("k inst/s), status "); con_dec(status); con_puts("\n");
    if (m->gui) { con_puts("c4pc: "); con_dec(display_ms); con_puts(" ms drawing to the screen\n"); }
    if (behind_ms) { con_puts("c4pc: the clock moved on "); con_dec(behind_ms); con_puts(" ms the machine could not keep up with\n"); }
    con_puts("c4pc: heap "); con_dec(heap_used(m->heap) / 1024); con_puts(" KB in use at exit, ");
    con_dec(heap_peak(m->heap) / 1024); con_puts(" KB at most; host memory free "); con_dec(pc_heap_free_bytes >> 10); con_puts(" KB\n");
  }
  power_off(status);
}

void ram_sink(drive *d, const char *name, blob *b) { (void)d; (void)name; (void)b; }
