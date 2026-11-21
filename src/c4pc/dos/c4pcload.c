/* c4pcload.c -- C4PC.COM: boots c4pc.elf from DOS, as LOADLIN boots
 * Linux, for DOSBox (which can boot no multiboot kernel) and real DOS.
 *
 *   C4PC [file.cfg]       (default C4PC.CFG)
 *
 * The configuration, a line each:
 *   kernel C4PC.ELF
 *   cmdline -s -m 32
 *   module C4IX32.C4R             the program
 *   module DISK.TAR drive         a drive (a ustar archive)
 *   module IN.TXT stdin           keyboard input, then end of input
 * A module's string is the whole rest of its line, as QEMU and GRUB pass
 * them. Everything is read with DOS and copied above 1 MB with the
 * BIOS's block move (INT 15h AH=87h); then the loader leaves DOS for
 * good: protected mode, flat segments, and a jump to the kernel with the
 * multiboot magic in EAX and its information in EBX.
 *
 * It needs real mode: under EMM386 (V86 mode) it refuses. In DOSBox,
 * xms=false and ems=false leave it all the memory to report.
 *
 * gcc -m16: 32-bit C running in real mode, every pointer an offset in
 * this one 64 KB segment. File data goes through a 32 KB buffer in the
 * next segment up.
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

void go_pm(u32 entry, u32 mbi, u32 pm32_linear, u32 gdtr_off);
extern char pm32[];

void *memset(void *d, int c, unsigned n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, unsigned n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }

static u16 cs(void) { u16 v; __asm__ volatile("mov %%cs, %0" : "=r"(v)); return v; }
static u32 linear(const void *p) { return ((u32)cs() << 4) + (u32)(unsigned)p; }

static void putch(int c) { __asm__ volatile("int $0x21" : : "a"(0x0200), "d"(c) : "cc"); }
static void puts_(const char *s) { while (*s) { if (*s == '\n') putch('\r'); putch(*s++); } }
static void putu(u32 v) { char t[12]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) putch(t[--n]); }
static void puthex(u32 v) { puts_("0x"); for (int i = 7; i >= 0; i--) putch("0123456789ABCDEF"[(v >> (4 * i)) & 15]); }
static void die(const char *s) { puts_("C4PC: "); puts_(s); puts_("\n"); __asm__ volatile("int $0x21" : : "a"(0x4c01)); for (;;) ; }

/* ---- DOS files ---- */
static int dos_open(const char *name) {
  u32 ax; u8 cf;
  __asm__ volatile("int $0x21; setc %1" : "=a"(ax), "=qm"(cf) : "a"(0x3d00), "d"(name) : "cc", "memory");
  return cf ? -1 : (int)(ax & 0xffff);
}
static void dos_close(int fd) { __asm__ volatile("int $0x21" : : "a"(0x3e00), "b"(fd) : "cc"); }
/* read n bytes into seg:0; how many, or -1 */
static int dos_read_seg(int fd, u16 seg, u16 n) {
  u32 ax; u8 cf;
  __asm__ volatile("push %%ds; mov %w4, %%ds; int $0x21; pop %%ds; setc %1"
                   : "=a"(ax), "=qm"(cf) : "a"(0x3f00), "b"(fd), "r"(seg), "c"(n), "d"(0) : "cc", "memory");
  return cf ? -1 : (int)(ax & 0xffff);
}
static int dos_read(int fd, void *buf, u16 n) {
  u32 ax; u8 cf;
  __asm__ volatile("int $0x21; setc %1" : "=a"(ax), "=qm"(cf) : "a"(0x3f00), "b"(fd), "c"(n), "d"(buf) : "cc", "memory");
  return cf ? -1 : (int)(ax & 0xffff);
}
static int dos_seek(int fd, u32 off) {
  u32 ax; u8 cf;
  __asm__ volatile("int $0x21; setc %1" : "=a"(ax), "=qm"(cf) : "a"(0x4200), "b"(fd), "c"(off >> 16), "d"(off & 0xffff) : "cc");
  return cf ? -1 : 0;
}

/* ---- memory above 1 MB ---- */
static u8 gdt87[48];
/* the BIOS's block move: n bytes (even, at most 64 KB) from src to dst, linear */
static void bios_move(u32 src, u32 dst, u32 n) {
  memset(gdt87, 0, sizeof gdt87);
  u8 *s = gdt87 + 0x10, *d = gdt87 + 0x18;
  s[0] = d[0] = 0xff; s[1] = d[1] = 0xff;
  s[2] = (u8)src; s[3] = (u8)(src >> 8); s[4] = (u8)(src >> 16); s[5] = 0x93; s[7] = (u8)(src >> 24);
  d[2] = (u8)dst; d[3] = (u8)(dst >> 8); d[4] = (u8)(dst >> 16); d[5] = 0x93; d[7] = (u8)(dst >> 24);
  u32 ax; u8 cf;
  __asm__ volatile("push %%es; push %%ds; pop %%es; int $0x15; pop %%es; setc %1"
                   : "=a"(ax), "=qm"(cf) : "a"(0x8700), "c"((n + 1) / 2), "S"(gdt87) : "cc", "memory");
  if (cf || (ax & 0xff00)) { puts_("C4PC: the BIOS block move failed at "); puthex(dst); puts_("\n"); die("cannot copy above 1 MB"); }
}

static u32 ext_kb(void) {
  u32 ax = 0, bx = 0, cx = 0, dx = 0; u8 cf;
  __asm__ volatile("int $0x15; setc %4" : "=a"(ax), "=b"(bx), "=c"(cx), "=d"(dx), "=m"(cf) : "a"(0xe801) : "cc");
  if (!cf) {
    u32 lo = ax & 0xffff, hi = bx & 0xffff;
    if (!lo && !hi) { lo = cx & 0xffff; hi = dx & 0xffff; }
    if (lo || hi) return lo + hi * 64;
  }
  __asm__ volatile("int $0x15; setc %1" : "=a"(ax), "=qm"(cf) : "a"(0x8800) : "cc");
  if (!cf && (ax & 0xffff)) return ax & 0xffff;
  return 0;
}

static void a20_on(void) {
  u32 ax;
  __asm__ volatile("int $0x15" : "=a"(ax) : "a"(0x2401) : "cc", "ebx", "ecx", "edx");
  u8 v; __asm__ volatile("inb $0x92, %0" : "=a"(v));
  if (!(v & 2)) { v = (u8)((v | 2) & ~1); __asm__ volatile("outb %0, $0x92" : : "a"(v)); }
}

/* ---- the configuration ---- */
static char cfg[4096];
static char kernel[80] = "C4PC.ELF";
static char *cmdline = "";
#define MAXMOD 16
static char *modname[MAXMOD], *modstr[MAXMOD]; static int nmod;

struct mb_info { u32 flags, mem_lower, mem_upper, boot_device, cmdline, mods_count, mods_addr; };
struct mb_mod { u32 start, end, string, reserved; };
static struct mb_info mbi;
static struct mb_mod mods[MAXMOD];
static char cmdbuf[256];
static struct { u16 lim; u32 base; } __attribute__((packed)) gdtr;
static u32 gdt[6];

static u8 hdr[512];
#define BUFSEG_OFF 0x1000u                 /* the buffer: the next 64 KB up */
#define CHUNK 32768u

/* the file's bytes from off, len of them, to linear dst */
static void load_range(int fd, u32 off, u32 len, u32 dst) {
  u16 bufseg = (u16)(cs() + BUFSEG_OFF);
  if (dos_seek(fd, off)) die("seek failed");
  while (len) {
    u16 n = (u16)(len < CHUNK ? len : CHUNK);
    int got = dos_read_seg(fd, bufseg, n);
    if (got != n) die("read failed: the file is shorter than it says");
    bios_move((u32)bufseg << 4, dst, (u32)n);
    dst += n; len -= n;
  }
}
static void zero_range(u32 dst, u32 len) {
  u16 bufseg = (u16)(cs() + BUFSEG_OFF);
  /* zeros in the buffer: a block move of them */
  u32 z = CHUNK;
  { u8 zeros[256]; memset(zeros, 0, sizeof zeros); for (u32 k = 0; k < z; k += 256) bios_move(linear(zeros), ((u32)bufseg << 4) + k, 256); }
  while (len) { u32 n = len < z ? len : z; bios_move((u32)bufseg << 4, dst, (n + 1) & ~1u); dst += n; len = len > n ? len - n : 0; }
}
static u32 file_size(int fd) {
  u32 ax, dx; u8 cf;
  __asm__ volatile("int $0x21; setc %2" : "=a"(ax), "=d"(dx), "=qm"(cf) : "a"(0x4202), "b"(fd), "c"(0), "d"(0) : "cc");
  if (cf) return 0;
  return (ax & 0xffff) | (dx & 0xffff) << 16;
}

static char *skip_ws(char *s) { while (*s == ' ' || *s == '\t') s++; return s; }
static int word_is(char **s, const char *w) {
  char *p = *s; while (*w && (*p | 32) == (*w | 32)) p++, w++;
  if (*w || (*p != ' ' && *p != '\t' && *p)) return 0;
  *s = skip_ws(p);
  return 1;
}

int main(void) {
  puts_("C4PC: the c4pc loader\n");
  u16 msw; __asm__ volatile("smsw %0" : "=r"(msw));
  if (msw & 1) die("the CPU is in virtual 8086 mode (EMM386?): boot DOS without it");

  /* the configuration's name: the command line's first word */
  char name[80] = "C4PC.CFG";
  { u8 *psp = (u8 *)0; u8 n = psp[0x80]; char *t = (char *)psp + 0x81; int k = 0;
    while (n && (*t == ' ' || *t == '\t')) t++, n--;
    while (n && *t != ' ' && *t != '\r' && k < 79) name[k++] = *t++, n--;
    if (k) name[k] = 0; }
  int fd = dos_open(name);
  if (fd < 0) { puts_("C4PC: no "); puts_(name); puts_("\n"); die("nothing to boot"); }
  int n = dos_read(fd, cfg, sizeof cfg - 1);
  dos_close(fd);
  if (n < 0) die("cannot read the configuration");
  cfg[n] = 0;
  for (char *s = cfg; *s;) {
    char *e = s; while (*e && *e != '\n' && *e != '\r') e++;
    char *next = *e ? e + 1 : e; *e = 0;
    char *p = skip_ws(s);
    if (word_is(&p, "kernel")) { int k = 0; while (p[k] && p[k] != ' ' && k < 79) kernel[k] = p[k], k++; kernel[k] = 0; }
    else if (word_is(&p, "cmdline")) cmdline = p;
    else if (word_is(&p, "module") && nmod < MAXMOD) {
      modstr[nmod] = p;
      static char names[MAXMOD][80];
      int k = 0; while (p[k] && p[k] != ' ' && k < 79) names[nmod][k] = p[k], k++; names[nmod][k] = 0;
      modname[nmod] = names[nmod];
      nmod++;
    }
    s = next;
  }

  a20_on();
  u32 kb = ext_kb();
  if (!kb) die("the BIOS reports no memory above 1 MB (in DOSBox: xms=false, ems=false)");
  puts_("C4PC: "); putu(kb >> 10); puts_(" MB above 1 MB\n");

  /* the kernel: its PT_LOAD segments where they ask to be */
  fd = dos_open(kernel);
  if (fd < 0) { puts_("C4PC: no "); puts_(kernel); puts_("\n"); die("no kernel"); }
  if (dos_read(fd, hdr, sizeof hdr) < 52 || hdr[0] != 0x7f || hdr[1] != 'E' || hdr[4] != 1) die("the kernel is not a 32-bit ELF");
  u32 entry = *(u32 *)(hdr + 24), phoff = *(u32 *)(hdr + 28);
  u16 phentsize = *(u16 *)(hdr + 42), phnum = *(u16 *)(hdr + 44);
  u32 top = 0x100000;
  for (u16 i = 0; i < phnum; i++) {
    u8 ph[32];
    if (dos_seek(fd, phoff + (u32)i * phentsize) || dos_read(fd, ph, 32) != 32) die("cannot read the program headers");
    u32 type = *(u32 *)ph, off = *(u32 *)(ph + 4), paddr = *(u32 *)(ph + 12), filesz = *(u32 *)(ph + 16), memsz = *(u32 *)(ph + 20);
    if (type != 1) continue;
    if (paddr < 0x100000) die("the kernel wants memory below 1 MB");
    load_range(fd, off, filesz, paddr);
    if (memsz > filesz) zero_range(paddr + filesz, memsz - filesz);
    if (paddr + memsz > top) top = paddr + memsz;
  }
  dos_close(fd);
  puts_("C4PC: "); puts_(kernel); puts_(" at 1 MB, entry "); puthex(entry); puts_("\n");

  /* the modules, a page apart, after it */
  u32 at = (top + 4095) & ~4095u;
  for (int i = 0; i < nmod; i++) {
    fd = dos_open(modname[i]);
    if (fd < 0) { puts_("C4PC: no "); puts_(modname[i]); puts_("\n"); die("a module is missing"); }
    u32 sz = file_size(fd);
    if ((at >> 10) + (sz >> 10) + 64 > 1024 + kb) die("the modules do not fit in memory");
    puts_("C4PC: "); puts_(modstr[i]); puts_(", "); putu(sz >> 10); puts_(" KB\n");
    load_range(fd, 0, sz, at);
    dos_close(fd);
    mods[i].start = at; mods[i].end = at + sz; mods[i].string = linear(modstr[i]);
    at = (at + sz + 4095) & ~4095u;
  }

  /* the information, here in low memory, which the kernel never touches */
  int k = 0;
  for (const char *p = kernel; *p && k < 250; ) cmdbuf[k++] = *p++;
  cmdbuf[k++] = ' ';
  for (const char *p = cmdline; *p && k < 254; ) cmdbuf[k++] = *p++;
  cmdbuf[k] = 0;
  mbi.flags = 1 | 4 | 8;
  mbi.mem_lower = 640;
  mbi.mem_upper = kb;
  mbi.cmdline = linear(cmdbuf);
  mbi.mods_count = (u32)nmod;
  mbi.mods_addr = linear(mods);

  gdt[0] = 0; gdt[1] = 0;
  gdt[2] = 0x0000ffff; gdt[3] = 0x00cf9a00;
  gdt[4] = 0x0000ffff; gdt[5] = 0x00cf9200;
  gdtr.lim = sizeof gdt - 1; gdtr.base = linear(gdt);
  puts_("C4PC: starting the kernel\n");
  go_pm(entry, linear(&mbi), linear(pm32), (u32)(unsigned)&gdtr);
  return 1;
}
