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
 *     (port 0xF4), then ACPI's S5 (acpi.c: VMware's, a real PC's), then
 *     QEMU's, Bochs's and VirtualBox's fixed ports, then a halt; status 82
 *     (C4IX's reboot, C4IX_EXIT_REBOOT) restarts the PC instead
 *   - the ACPI power button, by its SCI: SIGTERM to the program (C4IX
 *     shuts down, sched.c), a second press the machine off at once
 */
#include "../c4pc.h"
#include "../clib.h"
#include "../snd.h"
const char *sb16_init(snd *s);
void sb16_poll(void);
const char *hda_init(snd *s);
void hda_poll(void);
extern u32 sb16_underruns, hda_underruns;
static void (*audio_poll)(void) = sb16_poll;   /* the card under the machine's */

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

/* the text screen, kept here (pcv: vterm.h) and shown on the VGA's while
 * it is in text mode; on a framebuffer, screen.c draws it (screen_console) */
#include "vterm.h"
static vterm pcv = { .attr = 0x0700, .mirror = (volatile uint16_t *)0xB8000 };
static void vga_cursor(void) {
  u32 pos = (u32)vt_cursor(&pcv);
  outb(0x3D4, 0x0F); outb(0x3D5, (u8)pos); outb(0x3D4, 0x0E); outb(0x3D5, (u8)(pos >> 8));
}
static void vga_put(int c) { if (vt_put(&pcv, c)) vga_cursor(); }
/* the PC's log: what the console and the serial line were given, the last
 * PCLOG bytes of it (VM 0 reads it through the hypervisor: hv.c, C_LOG) */
enum { PCLOG = 32768 };
static char pclog[PCLOG];
static u32 pclog_n;
u32 pc_log_copy(char *out, u32 max) {
  u32 have = pclog_n < PCLOG ? pclog_n : PCLOG, n = have < max ? have : max;
  u32 from = pclog_n - n;
  for (u32 k = 0; k < n; k++) out[k] = pclog[(from + k) % PCLOG];
  return n;
}
static int vga_dirty, com1_only;
static void con_put(int c) {
  if (c != '\r') { pclog[pclog_n % PCLOG] = (char)c; pclog_n++; }
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
/* the store's disk given up (store.c, store_release): a block device now,
 * the next of CPU 0's machine's, which C4IX finds by BLK_COUNT */
static machine *pc_m;
void pc_store_to_blk(u32 io, int slave) {
  extern blkdev *ata_one(u32, int);
  blkdev *b;
  if (!pc_m || pc_m->nblk >= 8 || !(b = ata_one(io, slave))) return;
  pc_m->blk[pc_m->nblk++] = b;
  con_puts("c4pc: block device "); con_dec(pc_m->nblk - 1); con_puts(": "); con_puts(b->what);
  con_puts(", "); con_dec(b->sectors / 2048); con_puts(" MB (the store's)\n");
}
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
void acpi_init(void (*log)(const char *));
int acpi_button(i64 now);
void acpi_sci(void);
extern int acpi_irq;
void acpi_off(void);
void acpi_reset(void);
enum { STATUS_REBOOT = 82 };          /* C4IX_EXIT_REBOOT: reboot, shutdown -r */
static void power_off(int status) {
  con_flush();
  if (status == STATUS_REBOOT) { con_puts("c4pc: restarting the PC\n"); con_flush(); acpi_reset(); }
  outb(0xF4, (u8)(status & 0x7f));   /* QEMU's isa-debug-exit: exits (v << 1) | 1 */
  acpi_off();                        /* S5, by the FADT and the DSDT (acpi.c) */
  outw(0x604, 0x2000);               /* QEMU's ACPI power-off */
  outw(0xB004, 0x2000);              /* Bochs's, and older QEMU's */
  outw(0x4004, 0x3400);              /* VirtualBox's */
  for (;;) __asm__ volatile("cli; hlt");
}
void plat_fatal(const char *msg) { con_puts(msg); con_puts("\n"); power_off(70); for (;;) ; }

/* ---- the clock: the TSC where it keeps time, else the PIT's ticks ----
 * The PIT's channel 0 interrupts at 1 kHz (cpu_init), and counting them
 * was the clock. Under a hypervisor that is the guest's "apparent" time:
 * VMware delivers timer interrupts late while the VM is not running and
 * then in a burst to catch up, so a clock that counts them runs slow and
 * then fast -- games sped up and slowed down (Quake, Duke Nukem 3D) though
 * the machine's TIME was the real clock. A VM's TSC runs at a steady
 * rate with the host's time, as does an invariant TSC on a real CPU:
 * measured once against the PIT's channel 2 (polled, no interrupts, as
 * Linux's pit_calibrate_tsc), it is the clock. Without a hypervisor or an
 * invariant TSC (DOSBox, whose TSC follows its emulated cycles; old PCs)
 * the ticks stay. --clock tsc or --clock pit chooses. */
static volatile i64 ticks;
static u32 tsc_per_ms; static uint64_t tsc_at; static i64 tsc_ms;
static inline uint64_t rdtsc(void) { u32 lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return (uint64_t)hi << 32 | lo; }
static i64 ticks_now(void) {
  i64 t;
  do { t = ticks; } while (t != ticks);       /* two words: read until they agree */
  return t;
}
/* The TSC's cycles since tsc_at, which no one changes after clock_init:
 * every processor reads the clock (the machine's TIME is the real one on
 * each), and this once folded tsc_at and tsc_ms on as it went, unlocked.
 * Two processors folding at once gave a time 256 ms out, a 64-bit tsc_at
 * read half-changed (32-bit) was any time at all, and a processor whose
 * TSC is a little behind the first's (a VM's vCPUs) made the difference
 * negative, which folded on for ever: a game's clock leapt ahead (Quake
 * on CPU 1 played 20 s of its demo in a few). Now the cycles since are
 * divided whole (divl twice: no 64-bit divide in the bare build), a TSC
 * behind is no time since, and the clock never goes back across
 * processors (clock_seen) */
static volatile u32 clock_seen;               /* the latest ms any processor read (since tsc_ms) */
/* Under VMware the host's own time (vmware.c, the backdoor), from
 * vmw_ms0 on: VMware's TSC is the guest's apparent time, which runs fast
 * for a while after the VM fell behind (Quake played the first seconds of
 * its demo fast, 2026-11-24). A backdoor read is a VM exit, a microsecond
 * or two: cheaper than a game's clock that races */
static int clock_vmw;
static uint64_t vmw_us0;
static i64 vmw_ms0;
static volatile u32 vmw_seen;
static uint64_t tsc_since(void) {
  uint64_t t = rdtsc();
  return t > tsc_at ? t - tsc_at : 0;
}
/* N / D, N up to 2^64 and D 32 bits, as two 32-bit divides */
static uint64_t udiv64_32(uint64_t n, u32 d) {
  u32 hi = (u32)(n >> 32), lo = (u32)n, qhi = hi / d, r = hi % d, qlo;
  __asm__("divl %4" : "=a"(qlo), "=d"(r) : "a"(lo), "d"(r), "rm"(d));
  return (uint64_t)qhi << 32 | qlo;
}
static uint64_t udiv64_32(uint64_t n, u32 d);
i64 plat_now_ms(void) {
  if (clock_vmw) {
    uint64_t us;
    if (vmw_host_us(&us)) {
      u32 ms = us > vmw_us0 ? (u32)udiv64_32(us - vmw_us0, 1000) : 0, seen = __atomic_load_n(&vmw_seen, __ATOMIC_RELAXED);
      while ((i32)(ms - seen) > 0 && !__atomic_compare_exchange_n(&vmw_seen, &seen, ms, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { }
      if ((i32)(ms - seen) < 0) ms = seen;
      return vmw_ms0 + ms;
    }
  }
  if (!tsc_per_ms) return ticks_now();
  u32 ms = (u32)udiv64_32(tsc_since(), tsc_per_ms), seen = __atomic_load_n(&clock_seen, __ATOMIC_RELAXED);
  /* behind what another processor read (its TSC a little ahead): that */
  while ((i32)(ms - seen) > 0 && !__atomic_compare_exchange_n(&clock_seen, &seen, ms, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { }
  if ((i32)(ms - seen) < 0) ms = seen;
  return tsc_ms + ms;
}
/* the same in microseconds, for the sound card (snd.c) */
double plat_now_us(void) {
  if (clock_vmw) {
    uint64_t us;
    if (vmw_host_us(&us)) return (double)vmw_ms0 * 1000 + (double)(i64)(us > vmw_us0 ? us - vmw_us0 : 0);
  }
  if (!tsc_per_ms) return (double)ticks_now() * 1000;
  return (double)tsc_ms * 1000 + (double)(i64)tsc_since() * 1000 / tsc_per_ms;
}
static inline void cpuid_(u32 l, u32 *a, u32 *b, u32 *c, u32 *d) { __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(l), "c"(0)); }
/* the TSC's cycles in 30 ms of the PIT's channel 2 (1193182 Hz) */
static uint64_t tsc_30ms(void) {
  u32 latch = 1193182 * 30 / 1000;
  outb(0x61, (u8)((inb(0x61) & ~0x02) | 0x01));   /* the gate on, the speaker off */
  outb(0x43, 0xB0);                               /* channel 2, both bytes, mode 0 */
  outb(0x42, (u8)latch); outb(0x42, (u8)(latch >> 8));
  uint64_t t0 = rdtsc();
  for (u32 n = 0; !(inb(0x61) & 0x20); n++) if (n > 100000000u) return 0;   /* OUT2 at the count's end */
  return rdtsc() - t0;
}
/* want: 1 the TSC, 0 the ticks, -1 as the CPU suggests. What it chose, for the console */
static const char *clock_init(int want) {
  u32 a, b, c, d, inv = 0, hyp;
  cpuid_(0, &a, &b, &c, &d);
  if (a < 1) return "the PIT's ticks (no CPUID)";
  cpuid_(1, &a, &b, &c, &d);
  if (!(d & 0x10)) return "the PIT's ticks (no TSC)";
  hyp = c >> 31;
  cpuid_(0x80000000u, &a, &b, &c, &d);
  if (a >= 0x80000007u) { cpuid_(0x80000007u, &a, &b, &c, &d); inv = (d >> 8) & 1; }
  if (want == 0 || (want < 0 && !hyp && !inv)) return "the PIT's ticks";
  uint64_t s[3];
  for (int i = 0; i < 3; i++) s[i] = tsc_30ms();
  /* the middle one of three */
  uint64_t m = s[0] > s[1] ? (s[1] > s[2] ? s[1] : (s[0] > s[2] ? s[2] : s[0])) : (s[0] > s[2] ? s[0] : (s[1] > s[2] ? s[2] : s[1]));
  if (m < 30 * 1000) return "the PIT's ticks (the TSC would not time)";
  u32 per = (u32)(m / 30);
  tsc_at = rdtsc(); tsc_ms = ticks_now(); tsc_per_ms = per;
  return hyp ? "the TSC (a hypervisor's)" : "the TSC (invariant)";
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
static int k_shift, k_ctrl, k_caps, k_e0, k_alt;
static volatile int kbd_to_gui;
/* The PC's own keys, whoever has the keyboard:
 *   Ctrl+Alt+Del   restart: C4IX shuts down (SIGTERM, as the power
 *                  button), then the PC resets; pressed again, at once
 *   Ctrl+Alt+F1    the text console on the screen, the keys to it
 *   Ctrl+Alt+F2    the display again */
static volatile int want_reboot, vt_text, vt_back;
static volatile int show_fps;
static int hv;                          /* --hv: VM 0 of the hypervisor (hv.c) */
static volatile int hv_detach;          /* Ctrl+Alt+Home: the screen VM 0's again */
void hv_init(machine *m, double mhz, int mmu_on, int w, int h);
int hv_run(void);
machine *hv_shown(machine *m0);
const u16 *hv_text(machine *m, int *cur);   /* a guest's text screen, if it shows one */
int hv_key(int c);                          /* a key for an attached text guest: 1 if it took it */
void hv_attach(int id);           /* Ctrl+Alt+F11: frames a second, on the screen (screen_fps) */
static int reboot_after;              /* Ctrl+Alt+Del asked: the PC restarts when C4IX ends */
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
  if (sc == 0x38) { k_alt = !up; return; }
  if (!up && k_ctrl && k_alt) {
    if (sc == 0x53) { want_reboot++; return; }
    if (sc == 0x3C && vt_text) { vt_text = 0; vt_back = 1; return; }
    if (sc == 0x47 && hv) { hv_detach = 1; return; }                 /* Home: back to VM 0 from a text guest */
    if (sc == 0x3B) return;
  }
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
  if (!up && g_ctrl && g_alt) {
    if (sc == 0x53) { want_reboot++; return; }                   /* Del, or the keypad's */
    if (sc == 0x3B) { vt_text = 1; return; }                     /* F1: the text console */
    if (sc == 0x3C) return;                                      /* F2: the display already */
    if (sc == 0x57) { show_fps = !show_fps; vt_back = 1; return; }  /* F11: frames a second */
    if (sc == 0x47 && hv) { hv_detach = 1; return; }               /* Home: back to VM 0 (hv.c) */
  }
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
  kbd_to_gui = gui_attached(g) && !vt_text;
  while (scq_head != scq_tail) { u8 sc = scq[scq_head % sizeof scq]; scq_head++; key_event(g, sc); }
  if (vt_text) { kbd_to_gui = 0; screen_console(pcv.cells, vt_cursor(&pcv)); display_ms += plat_now_ms() - t; return; }
  if (vt_back) { vt_back = 0; g_ctrl = g_alt = 0; gui_redraw_all(g); }
  mouse_poll(g);
  gui_tick(g);
  static u32 flips; static i64 flip_at = -100000;
  if (gui_flips(g) != flips) { flips = gui_flips(g); flip_at = t; }
  if (gui_ring(g) || t - flip_at < 1000) {
    screen_show(g);
    /* the display's frames a second (its presents and flips: a game's,
     * or the desktop's, which draws the windowed ones'), a second at a time */
    if (show_fps) {
      static i64 fps_at; static u32 fps_n0; static int fps;
      u32 c, pr, ev, fl; gui_stats(g, &c, &pr, &ev, &fl);
      if (t - fps_at >= 1000) { fps = fps_at ? (int)((pr + fl - fps_n0) * 1000 / (u32)(t - fps_at)) : 0; fps_at = t; fps_n0 = pr + fl; }
      extern void screen_fps(int); screen_fps(fps);
    }
  }
  else {
    /* an attached guest with no display ring: its own text screen (hv.c) */
    int cur; const u16 *cells = hv ? hv_text(m, &cur) : 0;
    if (cells) screen_console(cells, cur); else screen_console(pcv.cells, vt_cursor(&pcv));
  }
  display_ms += plat_now_ms() - t;
}

/* ---- the CPU: GDT, IDT, PIC, PIT ---- */
#ifdef __x86_64__
/* boot64.S's isr_common: every register, then the vector, the error code and the CPU's frame */
struct regs { uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rbx, rdx, rcx, rax, vec, err, eip, cs, eflags, rsp, ss; };
#define GATE_WORDS 4                    /* a long mode gate is 16 bytes */
#else
struct regs { u32 edi, esi, ebp, esp, ebx, edx, ecx, eax, vec, err, eip, cs, eflags; };
#define GATE_WORDS 2
#endif
static u32 gdt[6];
static struct { uint16_t lim; uintptr_t base; } __attribute__((packed)) gdtr, idtr;
static u32 idt[256 * GATE_WORDS];

static void pic_eoi(u32 irq) { if (irq >= 8) outb(0xA0, 0x20); outb(0x20, 0x20); }

void isr(struct regs *r) {
  if (r->vec < 32) {
    static const char *names[] = { "divide error", "debug", "NMI", "breakpoint", "overflow", "bound", "invalid opcode",
      "no FPU", "double fault", "FPU segment", "bad TSS", "no segment", "stack fault", "general protection",
      "page fault", "?", "x87 error", "alignment", "machine check", "SIMD" };
    con_puts("\nc4pc: CPU exception ");
    con_dec(r->vec); con_puts(" ("); con_puts(r->vec < 20 ? names[r->vec] : "?");
    con_puts(") at eip "); con_hex((u32)r->eip); con_puts(", error "); con_hex((u32)r->err);
    con_puts(". Halting.\n");
    power_off(71);
  }
  /* the other processors' wake-up IPI (ap.c), and the local APIC's spurious one */
  if (r->vec == 240) { extern void lapic_eoi(void); lapic_eoi(); return; }
  if (r->vec == 255) return;
  u32 irq = (u32)r->vec - 32;
  if (irq == 0) { extern void ap_tick(void); ticks++; ap_tick(); }
  else if (irq == 1) kbd_irq();
  else if (irq == 12) { u8 st = inb(0x64); if (st & 1) { u8 b = inb(0x60); if (st & 0x20) mouse_byte(b); } }
  else if (irq == 4) { while (inb(0x3FD) & 1) { int c = inb(0x3F8); inq_push(c == '\r' ? '\n' : c); } }
  else if ((int)irq == acpi_irq) acpi_sci();   /* ACPI's: the power button (acpi.c) */
  else if (irq == 7 || irq == 15) {       /* spurious: no EOI for the one that did not happen */
    if (irq == 7) { outb(0x20, 0x0B); if (!(inb(0x20) & 0x80)) return; }
    else { outb(0xA0, 0x0B); if (!(inb(0xA0) & 0x80)) { outb(0x20, 0x20); return; } }
  }
  pic_eoi(irq);
}

static void idt_set(int n, void (*f)(void)) {
  uintptr_t a = (uintptr_t)f;
  idt[n * GATE_WORDS] = ((u32)a & 0xffff) | (0x08 << 16);
  idt[n * GATE_WORDS + 1] = ((u32)a & 0xffff0000) | 0x8E00;
#ifdef __x86_64__
  idt[n * GATE_WORDS + 2] = (u32)((uint64_t)a >> 32);
  idt[n * GATE_WORDS + 3] = 0;
#endif
}

#define STUBS(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) \
  X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39) X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47) X(240) X(255)
#define DECL(n) void isr##n(void);
STUBS(DECL)

static void cpu_init(void) {
  /* flat 4 GB code and data, ring 0 */
  gdt[0] = 0; gdt[1] = 0;
#ifdef __x86_64__
  gdt[2] = 0x0000ffff; gdt[3] = 0x00af9a00;   /* long mode code */
#else
  gdt[2] = 0x0000ffff; gdt[3] = 0x00cf9a00;
#endif
  gdt[4] = 0x0000ffff; gdt[5] = 0x00cf9200;
  gdtr.lim = sizeof gdt - 1; gdtr.base = (uintptr_t)gdt;
  gdt_load(&gdtr);
#define SET(n) idt_set(n, isr##n);
  STUBS(SET)
  idtr.lim = sizeof idt - 1; idtr.base = (uintptr_t)idt;
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

/* another processor's (ap.c): the kernel's GDT and IDT, the x87 */
/* CPU 0 with nothing to do: hlt, until an interrupt -- or, on several
 * processors, another CPU's request (par.c), whose IPI ends it */
static void idle0(machine *m) {
  if (m->smp && m->smp->par) par_idle0(m->smp);
  else __asm__ volatile("sti; hlt");
}
void cpu_init_ap(void) {
  gdt_load(&gdtr);
  __asm__ volatile("lidt %0" : : "m"(idtr));
  uint16_t cw = 0x027F;
  __asm__ volatile("fninit; fldcw %0" : : "m"(cw));
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
  vt_clear(&pcv, 0, 2000);
  cpu_init();
  con_puts(WB == 8 ? "c4pc: the 64-bit c4 machine on a PC\n" : "c4pc: the c4 machine on a PC\n");
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
  u32 mb_arena = 0 /* all: below */; i64 maxc = -1; double mhz = -1 /* auto */; int stats = 0, nofpu = 0, fast = 0, realtime = 1, wc = 1, sound = 1, store = 1, store_create = 0, cdrom = 1, acpi = 1, clock_want = -1, mmu_on = 0, noaccel = 0, noosaccel = 0, gui_w = 0, gui_h = 0, cpus = 0, quantum = 0, vmware = 1;
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
    else if (!strcmp(args[a], "--no-sound")) sound = 0;
    else if (!strcmp(args[a], "--store")) store_create = 1;      /* a blank ATA disk may become the store */
    else if (!strcmp(args[a], "--no-store")) store = 0;
    else if (!strcmp(args[a], "--no-cdrom")) cdrom = 0;      /* the CD drives left be (cdrom.c): a session libjs can match */
    else if (!strcmp(args[a], "--hv")) hv = 1;                  /* the hypervisor: this machine VM 0 (hv.c) */
    else if (!strcmp(args[a], "--no-vmware")) vmware = 0;    /* VMware's backdoor left be (vmware.c) */
    else if (!strcmp(args[a], "--vmware-lb")) vmware = 2;    /* ... its messages 4 bytes at a time, no high-bandwidth port */
    else if (!strcmp(args[a], "--no-acpi")) acpi = 0;        /* ACPI left be (acpi.c): off by the fixed ports, no power button */
    else if (!strcmp(args[a], "--debug-sound")) { extern void (*hda_log)(const char *); hda_log = con_puts; }
    else if (!strcmp(args[a], "--clock") && a + 1 < nargs) { a++; clock_want = !strcmp(args[a], "tsc") ? 1 : !strcmp(args[a], "pit") ? 0 : !strcmp(args[a], "vmware") ? 2 : -1; }
    else if (!strcmp(args[a], "--mmu")) mmu_on = 1;
    /* several processors (smp.c, par.c): the PC's own, one each, or taking
     * turns on this one where it has no more; all, as many as it has */
    else if (!strcmp(args[a], "--cpus") && a + 1 < nargs) {
      a++; cpus = 0;
      if (!strcmp(args[a], "all")) cpus = 32;
      else for (char *p = args[a]; *p; p++) cpus = cpus * 10 + (*p - '0');
    }
    else if (!strcmp(args[a], "-q") && a + 1 < nargs) { quantum = 0; for (char *p = args[++a]; *p; p++) quantum = quantum * 10 + (*p - '0'); }
    else if (!strcmp(args[a], "--no-mmu")) mmu_on = 0;
    else if (!strcmp(args[a], "--")) { a++; break; }
    else break;
  }

  if (acpi) acpi_init(con_puts);       /* the power button's and off's registers (acpi.c) */
  { const char *how = clock_init(clock_want == 2 ? -1 : clock_want);
    con_puts("c4pc: the clock: "); con_puts(how);
    if (tsc_per_ms) { con_puts(", "); con_dec(tsc_per_ms / 1000); con_puts(" MHz"); }
    con_puts("\n"); }

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
      d->kind = nd ? "tar" : "boot"; d->size_kb = d->used_kb = n >> 10;
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
  /* drive 1 kept on a disk (store.c), where C4IX's persist saves, as the
   * page keeps it in IndexedDB: the store's disk, or with --store a blank
   * one made one */
  if (store && nd < 16) {
    extern drive *store_open(int, void (*)(const char *), char *, u32);
    char what[128];
    drive *sd = store_open(store_create, con_puts, what, sizeof what);
    if (sd) {
      for (int i = nd; i > 1; i--) drives[i] = drives[i - 1];
      drives[1] = sd; nd++;
      con_puts("c4pc: drive 1: "); con_puts(what); con_puts("\n");
    }
  }
  /* the IDE controller's DMA (pc/ata.c), for the CD drives and disks below */
  { extern const char *ide_pci_init(void); const char *dma = ide_pci_init();
    if (dma) { con_puts("c4pc: disks and CDs by "); con_puts(dma); con_puts("\n"); } }
  /* the CD drives (cdrom.c), from drive 2 on: drive 1 is the store's
   * place (persist's), an empty read-only one where there is no store */
  {
    drive *cds[4]; int nc = 0;
    if (cdrom) cdrom_find(cds, &nc, 4, nd < 2 ? 2 : nd, con_puts);
    if (nc && nd < 2) { drive *e = drive_new(0); e->kind = "none"; drives[nd++] = e; }
    for (int i = 0; i < nc && nd < 16; i++) drives[nd++] = cds[i];
  }
  /* VMware's (vmware.c): its shared folders and clipboard, a drive after
   * the CDs */
  if (vmware && vmw_detect(vmware == 2)) {
    /* the clock: the host's, on VMware (or --clock vmware: QEMU's vmport) */
    uint64_t us;
    u32 ha, hb, hc, hd;
    cpuid_(0x40000000u, &ha, &hb, &hc, &hd);
    int on_vmware = hb == 0x61774d56u && hc == 0x4d566572u && hd == 0x65726177u;   /* "VMwareVMware" */
    if ((clock_want == 2 || (clock_want < 0 && on_vmware)) && vmw_host_us(&us)) {
      vmw_ms0 = plat_now_ms(); vmw_us0 = us; clock_vmw = 1;
      con_puts("c4pc: the clock: VMware's host time (its TSC is the VM's apparent time)\n");
    } else if (clock_want == 2) {
      extern u32 vmw_time_regs[4];
      con_puts("c4pc: the clock: VMware's host time asked for, not given: GETTIMEFULL EAX EBX EDX ESI");
      for (int k = 0; k < 4; k++) { con_puts(" "); con_dec(vmw_time_regs[k]); }
      con_puts("\n");
    }
    if (nd < 2) { drive *e = drive_new(0); e->kind = "none"; drives[nd++] = e; }
    if (nd < 16) {
      drives[nd++] = vmw_drive();
      con_puts("c4pc: drive "); con_dec(nd - 1); con_puts(": VMware's, its shared folders (hgfs/) and clipboard\n");
    }
  }
  /* the IDE disks but the store's, as block devices (ata.c), for C4IX's
   * own filesystem (docs/c4ix-ext2.md) */
  static blkdev *blks[8]; int nblk = 0;
  {
    extern int ata_find(blkdev **, int *, int, u32, int);
    extern u32 store_io; extern int store_slave;
    ata_find(blks, &nblk, 8, store_io, store_slave);
    for (int i = 0; i < nblk; i++) {
      con_puts("c4pc: block device "); con_dec(i); con_puts(": "); con_puts(blks[i]->what);
      con_puts(", "); con_dec(blks[i]->sectors / 2048); con_puts(" MB\n");
    }
  }

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
  /* the arena: -m's, or as much of the PC's memory as a power of two
   * fits in, leaving c4pc 160 MB of its own (the drives' unpacked files, 96
   * MB at most; the display's buffers; swap's pages where there is no
   * swap partition), at most 1 GB: C4IX's user window goes above it, and
   * ends by 2 GB (src/c4ix/mmu.c mmu_wbase) */
  int mem_all = !mb_arena;
  if (mem_all) {
    extern u32 pc_heap_free_bytes;
    u32 room = pc_heap_free_bytes >> 20;
    room = room > 160 + 32 ? room - 160 : 32;
    mb_arena = 32;
    /* the 64-bit word's MMU reaches 1 GB of addresses, the window's end
     * among them: 512 MB of memory there (c4ix.h MMU_UWIN_LAST) */
    while (mb_arena * 2 <= room && mb_arena < (WB == 8 ? 512u : 1024u)) mb_arena *= 2;
    /* the hypervisor's VM 0: a quarter of it, at most 256 MB, the rest the guests' */
    if (hv) { u32 c = room / 4 > 256 ? 256 : room / 4; while (mb_arena > 64 && mb_arena > c) mb_arena /= 2; }
  }
  machine *m = machine_new(mb_arena << 20, drives, nd, mhz);
  m->on_byte = on_byte;
  m->no_fpu = nofpu;
  if (noaccel) m->accel = 0;
  if (noosaccel) m->osaccel = 0;
  if (mmu_on) mmu_new(m);
  for (int i = 0; i < nblk; i++) m->blk[m->nblk++] = blks[i];
  /* swap: the first IDE disk's partition of type 82h (install makes one),
   * else the host's memory, 256 MB of it, or with all the memory
   * C4IX's, as much as c4pc can spare */
  {
    static u8 mbr[512];
    int got = 0;
    for (int i = 0; i < nblk && !got; i++) {
      blkdev *b = blks[i];
      if (b->rw(b, 0, 0, 1, mbr) || mbr[510] != 0x55 || mbr[511] != 0xAA) continue;
      for (int k = 0; k < 4 && !got; k++) {
        u8 *e = mbr + 446 + 16 * k;
        u32 lba = e[8] | (u32)e[9] << 8 | (u32)e[10] << 16 | (u32)e[11] << 24;
        u32 n = e[12] | (u32)e[13] << 8 | (u32)e[14] << 16 | (u32)e[15] << 24;
        if (e[4] != 0x82 || n < 2048 || lba >= b->sectors || n > b->sectors - lba) continue;
        dev_swap_disk(m, b, lba, n);
        got = 1;
        con_puts("c4pc: swap on block device "); con_dec(i); con_puts("'s partition "); con_dec(k + 1);
        con_puts(", "); con_dec(n / 2048); con_puts(" MB\n");
      }
    }
    /* -m N: 256 MB, as libjs's (the tests hold c4pc's boot to its, byte
     * for byte); all the memory C4IX's: what c4pc can spare */
    if (!got && mem_all) {
      extern u32 pc_heap_free_bytes;
      u32 spare = pc_heap_free_bytes > (128u << 20) ? (pc_heap_free_bytes - (128u << 20)) / 4096 : 0;
      m->dv->swap_slots = spare < 65536 ? spare : 65536;
    }
  }
  pc_m = m;
  /* the display: GRUB's framebuffer, or the BGA's at --gui's size */
  if (screen_from_multiboot(mb) || (gui_w > 0 && gui_h > 0 && screen_bga(gui_w, gui_h))) {
    int w, h; screen_size(&w, &h);
    { extern void screen_describe(void); screen_describe(); }
    if (wc) { extern const char *screen_wc(void); con_puts("c4pc: the framebuffer: "); con_puts(screen_wc()); con_puts("\n"); }
    m->gui = gui_new(m, w, h);
    dbg_m = m;
    int mouse = mouse_init();
    if (mouse) outb(0xA1, (u8)(inb(0xA1) & 0xEF));   /* IRQ 12 (the SCI's may be unmasked: acpi.c) */
    int vmouse = mouse && vmw_mouse_init();
    con_puts("c4pc: a display of "); con_dec(w); con_puts(" x "); con_dec(h);
    con_puts(vmouse ? ", and VMware's absolute mouse (the pointer leaves at the edge)\n" : mouse ? ", and a mouse\n" : ", and no mouse\n");
    pcv.mirror = 0;                     /* graphics: the VGA's text is not shown */
    screen_console(pcv.cells, vt_cursor(&pcv));
    if (hv) hv_init(m, (double)m->cycles_per_ms / 1000, mmu_on, w, h);
  }
  /* the sound card (snd.c), at the keyboard, on a Sound Blaster 16 (sb16.c):
   * scripted or --fast, the machine's clock is not the one it would play to */
  if (paced && sound) {
    snd *s = snd_new(m, 44100, 1);
    const char *how = sb16_init(s);
    if (!how && (how = hda_init(s))) audio_poll = hda_poll;
    /* the Ensoniq AudioPCI: VMware's ES1371 for many guest types (es137x.c) */
    { extern const char *es137x_init(snd *); extern void es137x_poll(void);
      if (!how && (how = es137x_init(s))) audio_poll = es137x_poll; }
    if (how) { m->snd = s; con_puts("c4pc: sound: "); con_puts(how); con_puts("\n"); }
    else {
      extern const char *hda_why, *es137x_why;
      con_puts("c4pc: no sound: no Sound Blaster 16 at 220h or 240h; "); con_puts(hda_why);
      con_puts("; "); con_puts(es137x_why); con_puts("\n");
    }
  }
  /* the processors: the PC's others started (ap.c), each the machine's
   * CPU on a processor of its own (par.c); with none, the machine's CPUs
   * take turns on this one (smp.c) */
  int par = 0;
  if (cpus > 1) {
    extern int ap_start(int, const char **);
    const char *why = "";
    int n = 1;
    u32 mbl = (u32)(uintptr_t)mb;
    if (mbl + sizeof *mb > 0x7000 && mbl < 0x8000) why = "the loader's information is where the processors would start";
    else n = ap_start(cpus, &why);
    if (n > 1) {
      smp_new(m, n, quantum);
      par = 1;
      con_puts("c4pc: "); con_dec(n); con_puts(" processors, each the machine's CPU on one of the PC's own\n");
    } else {
      smp_new(m, cpus, quantum);
      con_puts("c4pc: "); con_dec(cpus); con_puts(" CPUs taking turns on this processor ("); con_puts(why); con_puts(")\n");
    }
  }
  if (have_in) { for (u32 i = 0; i < in_len; i++) kbd_push(m, in[i]); m->rx_eof = 1; }
  con_puts("c4pc: "); con_dec(m->size >> 20); con_puts(mmu_on ? " MB arena with an MMU" : " MB arena");
  con_puts(mem_all ? " (the PC's memory but c4pc's own; -m N for less), running " : ", running ");
  con_puts(img_name); con_puts("\n");
  if (machine_boot(m, img, img_len, pargc, pargv)) power_off(75);
  if (par) par_start(m->smp);

  i64 t0 = plat_now_ms(), parked = 0, idle_ms = 0;
  /* At the keyboard the machine's clock is the real one (--no-realtime:
   * its own, cycles / MHz, stretching when the PC falls behind). Given the
   * cycles' clock and pulled up to the real one only between slices, as
   * libjs's page does, TIME crawled through a frame and then jumped: Duke
   * Nukem 3D said 40 to 240 frames a second, and turned at a speed that
   * jumped with them. Timers (PIT, the display's TICKS) were real already */
  if (paced && realtime) { m->wall = 1; m->wall_base = t0; }
  i64 limit = maxc < 0 ? INT64_MAX : maxc;
  int status;
  for (;;) {
    /* what the keyboard and COM1 brought: cooked as a terminal would,
     * unless a program has the raw keyboard open (cli.js -i) */
    while (inq_head != inq_tail) {
      int c = inq[inq_head % sizeof inq]; inq_head++;
      if (have_in) continue;                  /* scripted: the keys are not the program's */
      if (hv && hv_key(c)) continue;          /* an attached guest's, with a text screen */
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
    /* the power button (acpi.c: its SCI counted it): SIGTERM to the
     * program, which C4IX takes as a shutdown (sched.c); a second press a
     * second later, or a program that takes no SIGTERM, the machine off */
    {
      static i64 pressed;
      i64 now = plat_now_ms();
      /* VMware's requests (vmware.c): its Shut Down Guest, as the button */
      static i64 vmw_at;
      int vmw_halt = 0;
      if (now - vmw_at >= 100) { vmw_at = now; vmw_halt = vmw_poll(); }
      if (vmw_halt) con_puts("\nc4pc: VMware asked the guest to shut down\n");
      /* Ctrl+Alt+Del: C4IX shuts down, and then the PC restarts */
      static int reboot_seen;
      if (want_reboot != reboot_seen) {
        int first = !reboot_seen;
        reboot_seen = want_reboot;
        reboot_after = 1;
        if (!first || !m->sigh[15]) { con_puts("\nc4pc: Ctrl+Alt+Del: restarting now\n"); status = STATUS_REBOOT; break; }
        con_puts("\nc4pc: Ctrl+Alt+Del: C4IX shuts down, then the PC restarts\n");
        m->pending_signal = 15;
      }
      if (acpi_button(now) || vmw_halt) {
        if (!m->sigh[15] || (pressed && now - pressed >= 1000)) { con_puts("\nc4pc: the power button: off\n"); status = 0; break; }
        if (!pressed) { pressed = now ? now : 1; m->pending_signal = 15; }
      }
    }
    con_flush();
    i64 left = limit - machine_clock(m);
    if (left <= 0) { con_puts("c4pc: stopped at cycle budget\n"); status = 9; break; }
    /* someone at the keyboard: slices of 4 simulated milliseconds (as
     * libjs/worker.js's), paced to the machine's own clock (cli.js -i), so
     * that a simulated second takes a real one and top redraws once a
     * second; scripted, or with --fast, as fast as it goes */
    i64 slice = paced ? (i64)m->cycles_per_ms * 4 : 20000000;
    int r = machine_run(m, left < slice ? left : slice);
    if (m->snd) audio_poll();
    if (m->gui) display_tick(hv ? hv_shown(m) : m);
    if (hv) { if (hv_detach) { hv_detach = 0; hv_attach(0); } hv_run(); }
    /* -s at the keyboard: every 10 s, on COM1 alone, how the PC kept up */
    if (stats && paced) {
      static i64 rep_at, rep_cyc, rep_disp, rep_idle;
      i64 now = plat_now_ms();
      if (!rep_at) { rep_at = now; rep_cyc = m->cycle - m->skipped; }
      else if (now - rep_at >= 10000) {
        i64 dt = now - rep_at, ran = m->cycle - m->skipped;
        com1_only = 1;
        con_puts("c4pc: "); con_dec((ran - rep_cyc) / dt); con_puts("k inst/s, ");
        con_dec((display_ms - rep_disp) * 100 / dt); con_puts("% drawing to the screen, ");
        con_dec((idle_ms - rep_idle) * 100 / dt); con_puts("% idle");
        if (m->snd) { con_puts(", "); con_dec(sb16_underruns + hda_underruns); con_puts(" sound underruns"); }
        con_puts("\n");
        /* the other processors' requests of CPU 0, since the boot (par.c) */
        if (m->smp && m->smp->par) { static char b[700]; par_stats(m->smp, b, sizeof b); con_puts("c4pc: "); con_puts(b); con_puts("\n"); }
        com1_only = 0;
        rep_at = now; rep_cyc = ran; rep_disp = display_ms; rep_idle = idle_ms;
      }
    }
    /* the parallel engine: CPU 0 parked or halted waits for the others,
     * answering them (par.c) */
    if (par && !m->halted) {
      int k = par_after(m->smp, r, &status);
      if (k == 2) break;
      if (k == 1) continue;
    }
    if (r == STOP_HALT) { status = m->status; break; }
    if (r == STOP_INPUT) {
      if (have_in) { con_puts("c4pc: blocked on input with nothing left to read\n"); status = 8; break; }
      con_flush();
      i64 t = plat_now_ms();
      if (m->snd) audio_poll();
      idle0(m);                               /* until a key, the next tick, or another CPU asking */
      parked += plat_now_ms() - t;
      continue;
    }
    if (paced && m->wall) {
      /* a nap: wait for the real time it asked to wake at, or the PIT's
       * next tick, or input (which ends it early, as an interrupt would) */
      if (r == STOP_SLEEP) {
        con_flush();
        i64 now = plat_now_ms(), until = t0 + m->nap_until;
        if (m->pit_ms && m->pit_next < until) until = m->pit_next;
        if (until > now + 100) until = now + 100;
        while (plat_now_ms() < until && inq_head == inq_tail && scq_head == scq_tail && !mouse_pending()) {
          if (m->snd) audio_poll();
          /* the hypervisor's guests run while VM 0 naps; their screen is kept */
          if (hv) { int ran = hv_run(); if (m->gui) display_tick(hv_shown(m)); if (hv_detach) { hv_detach = 0; hv_attach(0); } if (ran) continue; }
          idle0(m);
        }
        idle_ms += plat_now_ms() - now;
      }
    } else if (paced) {
      i64 ahead = machine_sim_ms(m) - (plat_now_ms() - t0 - parked);
      if (ahead > 2) {
        con_flush();
        i64 until = plat_now_ms() + (ahead < 100 ? ahead : 100);
        while (plat_now_ms() < until && inq_head == inq_tail && scq_head == scq_tail && !mouse_pending()) {
          if (m->snd) audio_poll();
          idle0(m);
        }
        /* input during a nap: the guest wakes now, and its clock gets only
         * what it slept (libjs/worker.js wake). Kept whole, every pointer
         * move gave it a nap's worth of time, and two clicks a moment apart
         * were seconds apart to the desktop: never a double click */
        if (r == STOP_SLEEP && (inq_head != inq_tail || scq_head != scq_tail || mouse_pending())) {
          i64 a2 = machine_sim_ms(m) - (plat_now_ms() - t0 - parked);
          if (a2 > 0) { i64 cut = a2 < m->nap_ms ? a2 : m->nap_ms; m->sleep_ms -= cut; m->nap_ms -= cut; }
        }
      }
      /* hopelessly behind: stop trying to catch up (worker.js) */
      else if (ahead < -1000) parked += -ahead - 1000;
    }
  }
  con_flush();
  if (stats) {
    i64 dt = plat_now_ms() - t0;
    i64 ran = m->cycle - m->skipped;
    con_puts("c4pc: "); con_dec(ran); con_puts(" cycles in "); con_dec(dt); con_puts(" ms (");
    con_dec(dt > 0 ? ran / dt : 0); con_puts("k inst/s), status "); con_dec(status); con_puts("\n");
    if (m->gui) { con_puts("c4pc: "); con_dec(display_ms); con_puts(" ms drawing to the screen\n"); }
    if (m->wall) { con_puts("c4pc: "); con_dec(idle_ms); con_puts(" ms idle, on the real clock\n"); }
    con_puts("c4pc: heap "); con_dec(heap_used(m->heap) / 1024); con_puts(" KB in use at exit, ");
    con_dec(heap_peak(m->heap) / 1024); con_puts(" KB at most; host memory free "); con_dec(pc_heap_free_bytes >> 10); con_puts(" KB\n");
  }
  if (reboot_after && status == 0) status = STATUS_REBOOT;
  power_off(status);
}

void ram_sink(drive *d, const char *name, blob *b) { (void)d; (void)name; (void)b; }
/* the hypervisor's messages and its guests' consoles (hv.c): the serial line alone */
void hv_out(const char *s) { com1_only = 1; con_puts(s); com1_only = 0; }
