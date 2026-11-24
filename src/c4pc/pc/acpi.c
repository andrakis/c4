/* acpi.c -- the PC's power: off, the power button, and a restart
 * (docs/c4pc.md).
 *
 * Off is ACPI's sleep state S5, as an OS without an AML interpreter does
 * it: the RSDP (the BIOS area, or the EBDA's first KB), the RSDT or XSDT,
 * the FADT (its PM1a/PM1b control and event blocks, SMI_CMD and
 * ACPI_ENABLE), and in the DSDT the \_S5 package's SLP_TYPa and SLP_TYPb.
 * Writing SLP_TYP | SLP_EN to PM1a_CNT (and PM1b's) turns the machine
 * off: VMware's, QEMU's, VirtualBox's, a real PC's. QEMU's and Bochs's
 * fixed ports stay as the fallback where there is no ACPI.
 *
 * The power button (VMware's "Shut Down Guest", QEMU's system_powerdown,
 * a real one): at boot the chipset is put in ACPI mode (ACPI_ENABLE to
 * SMI_CMD, until SCI_EN) and PWRBTN_EN set in PM1_EN, so that a press is
 * PWRBTN_STS in PM1_STS rather than the firmware's to act on, and that
 * the SCI, ACPI's interrupt (the FADT's SCI_INT, an ISA IRQ: 9 on most),
 * says so. Nothing else may raise it: PM1_EN has the button alone, and
 * every GPE's enable bit is cleared (their events are the AML's, which
 * this does not run). pc.c's interrupt handler calls acpi_sci, which
 * counts a press; its run loop hands one to the program (acpi_button),
 * as Ctrl-C's SIGINT, as SIGTERM: C4IX shuts down (sched.c). A second
 * press is the machine off at once, as holding a real one is. Where the
 * SCI is not an ISA IRQ, or raises itself with nothing to say, the
 * button is polled every 50 ms instead: an I/O port read, which under a
 * hypervisor is a trip out to it -- the reason it is not always done.
 *
 * Restart: the FADT's RESET_REG where it has one in I/O space, then the
 * chipset's reset control at 0xCF9, then the keyboard controller's pulse
 * (0xFE to 0x64), then a triple fault.
 */
#include "../c4pc.h"
#include "../clib.h"

static inline void outb(u32 p, u8 v) { __asm__ volatile("outb %0,%w1" : : "a"(v), "Nd"(p)); }
static inline void outw(u32 p, uint16_t v) { __asm__ volatile("outw %0,%w1" : : "a"(v), "Nd"(p)); }
static inline u8 inb(u32 p) { u8 v; __asm__ volatile("inb %w1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint16_t inw(u32 p) { uint16_t v; __asm__ volatile("inw %w1,%0" : "=a"(v) : "Nd"(p)); return v; }

static u32 pm1a_cnt, pm1b_cnt, pm1a_evt, pm1b_evt, pm1_evt_len, smi_cmd;
static u8 acpi_enable_val, slp_a, slp_b, have_s5;
static u8 reset_ok, reset_val; static u32 reset_port;
static int button_ok;
static u32 gpe0, gpe0_len, gpe1, gpe1_len;
int acpi_irq = -1;                    /* the SCI's IRQ, or -1: polled, or none */
int acpi_ncpu;                        /* the processors the MADT lists, enabled or able to be; 0 if none */
static volatile int presses, spurious;
static int polled;

static u32 rd32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static const u8 *phys(uint64_t a) { return a && a < 0x100000000ull ? (const u8 *)(uintptr_t)a : 0; }
static int sum_ok(const u8 *p, u32 n) { u8 s = 0; for (u32 i = 0; i < n; i++) s = (u8)(s + p[i]); return s == 0; }

static const u8 *rsdp_in(u32 lo, u32 hi) {
  for (u32 a = lo & ~15u; a + 20 <= hi; a += 16) {
    const u8 *p = (const u8 *)(uintptr_t)a;
    if (!memcmp(p, "RSD PTR ", 8) && sum_ok(p, 20)) return p;
  }
  return 0;
}

/* a table by its signature, from the RSDT (32-bit entries) or XSDT (64) */
static const u8 *table(const u8 *rsdp, const char *sig) {
  const u8 *x = rsdp[15] >= 2 ? phys((uint64_t)rd32(rsdp + 24) | (uint64_t)rd32(rsdp + 28) << 32) : 0;
  const u8 *r = phys(rd32(rsdp + 16));
  if (x && !memcmp(x, "XSDT", 4)) {
    u32 len = rd32(x + 4);
    for (u32 o = 36; o + 8 <= len; o += 8) {
      const u8 *t = phys((uint64_t)rd32(x + o) | (uint64_t)rd32(x + o + 4) << 32);
      if (t && !memcmp(t, sig, 4)) return t;
    }
  }
  if (r && !memcmp(r, "RSDT", 4)) {
    u32 len = rd32(r + 4);
    for (u32 o = 36; o + 4 <= len; o += 4) {
      const u8 *t = phys(rd32(r + o));
      if (t && !memcmp(t, sig, 4)) return t;
    }
  }
  return 0;
}

/* an AML integer at P: ZeroOp, OneOp, BytePrefix, WordPrefix; -1 if not one */
static int aml_int(const u8 **pp) {
  const u8 *p = *pp;
  int v;
  if (*p == 0x00 || *p == 0x01) { v = *p; p += 1; }
  else if (*p == 0x0A) { v = p[1]; p += 2; }
  else if (*p == 0x0B) { v = p[1] | p[2] << 8; p += 3; }
  else return -1;
  *pp = p;
  return v;
}

/* \_S5: Name (_S5_, Package () { SLP_TYPa, SLP_TYPb, ... }) somewhere in
 * the DSDT's AML -- found by its name, as OSes without an interpreter do */
static void find_s5(const u8 *dsdt) {
  u32 len = rd32(dsdt + 4);
  for (u32 i = 36; i + 8 < len; i++) {
    const u8 *p = dsdt + i;
    if (memcmp(p, "_S5_", 4)) continue;
    /* NameOp before it (08 _S5_, or 08 \ _S5_), PackageOp after */
    if (!(p[-1] == 0x08 || (p[-1] == '\\' && p[-2] == 0x08))) continue;
    p += 4;
    if (*p != 0x12) continue;
    p++;
    p += ((*p & 0xC0) >> 6) + 1;             /* PkgLength: 1 to 4 bytes */
    p++;                                      /* NumElements */
    int a = aml_int(&p), b = aml_int(&p);
    if (a < 0) continue;
    slp_a = (u8)(a & 7); slp_b = (u8)((b < 0 ? 0 : b) & 7);
    have_s5 = 1;
    return;
  }
}

/* ACPI mode: SCI_EN in PM1a_CNT, or ACPI_ENABLE to SMI_CMD and a wait for it */
static int acpi_mode(void) {
  if (!pm1a_cnt) return 0;
  if (inw(pm1a_cnt) & 1) return 1;
  if (!smi_cmd || !acpi_enable_val) return 0;
  outb(smi_cmd, acpi_enable_val);
  for (int i = 0; i < 3000000; i++) if (inw(pm1a_cnt) & 1) return 1;
  return 0;
}

/* what the firmware's tables say; LOG gets a line */
void acpi_init(void (*log)(const char *)) {
  const u8 *rsdp = 0;
  /* the BIOS data area's word at 0x40E: the EBDA's segment (the address
   * laundered, or GCC takes a constant pointer near 0 for a null one) */
  volatile uint16_t *bda;
  uintptr_t at = 0x40E;
  __asm__("" : "=r"(bda) : "0"(at));
  u32 ebda = (u32)*bda << 4;
  if (ebda >= 0x80000 && ebda < 0xA0000) rsdp = rsdp_in(ebda, ebda + 1024);
  if (!rsdp) rsdp = rsdp_in(0xE0000, 0x100000);
  if (!rsdp) { log("c4pc: no ACPI: off and the power button by QEMU's and Bochs's ports alone\n"); return; }
  /* the processors: the MADT's local APICs (type 0) and x2APICs (type 9),
   * each enabled (flag 0) or able to be (flag 1). CPUID says how many a
   * package has: a VM of 4 processors of 2 cores each said 2 */
  const u8 *madt = table(rsdp, "APIC");
  if (madt) {
    u32 len = rd32(madt + 4);
    for (u32 o = 44; o + 2 <= len && madt[o + 1] >= 2; o += madt[o + 1]) {
      if (madt[o] == 0 && madt[o + 1] >= 8 && (rd32(madt + o + 4) & 3)) acpi_ncpu++;
      if (madt[o] == 9 && madt[o + 1] >= 16 && (rd32(madt + o + 8) & 3)) acpi_ncpu++;
    }
  }
  const u8 *f = table(rsdp, "FACP");
  if (!f) { log("c4pc: ACPI without a FADT\n"); return; }
  u32 flen = rd32(f + 4);
  smi_cmd = rd32(f + 48);
  acpi_enable_val = f[52];
  pm1a_evt = rd32(f + 56); pm1b_evt = rd32(f + 60);
  pm1a_cnt = rd32(f + 64); pm1b_cnt = rd32(f + 68);
  pm1_evt_len = f[88];
  gpe0 = rd32(f + 80); gpe1 = rd32(f + 84); gpe0_len = f[92]; gpe1_len = f[93];
  u32 sci = f[46] | (u32)f[47] << 8;
  /* RESET_REG (ACPI 2+, flag RESET_REG_SUP): an I/O port only */
  if (flen >= 129 && (rd32(f + 112) & (1u << 10)) && f[116] == 1) { reset_ok = 1; reset_port = rd32(f + 120); reset_val = f[128]; }
  const u8 *dsdt = 0;
  if (flen >= 148) dsdt = phys((uint64_t)rd32(f + 140) | (uint64_t)rd32(f + 144) << 32);
  if (!dsdt) dsdt = phys(rd32(f + 40));
  if (dsdt && !memcmp(dsdt, "DSDT", 4)) find_s5(dsdt);
  /* the power button ours, not the firmware's. (A control method button,
   * FADT flag PWR_BUTTON, bit 4, is a GPE and the AML's to say, which
   * needs an interpreter; most also set the fixed one's status.) */
  int cm = (rd32(f + 112) & (1u << 4)) != 0;
  if (acpi_mode() && pm1a_evt && pm1_evt_len >= 4) {
    /* the GPEs' enable halves cleared, then their status halves */
    for (u32 i = 0; i < gpe0_len / 2; i++) outb(gpe0 + gpe0_len / 2 + i, 0);
    for (u32 i = 0; i < gpe1_len / 2; i++) outb(gpe1 + gpe1_len / 2 + i, 0);
    for (u32 i = 0; i < gpe0_len / 2; i++) outb(gpe0 + i, 0xFF);
    for (u32 i = 0; i < gpe1_len / 2; i++) outb(gpe1 + i, 0xFF);
    /* PM1: every status cleared (written 1), the button's event alone enabled */
    outw(pm1a_evt, 0xFFFF);
    outw(pm1a_evt + pm1_evt_len / 2, 0x100);
    if (pm1b_evt) { outw(pm1b_evt, 0xFFFF); outw(pm1b_evt + pm1_evt_len / 2, 0x100); }
    button_ok = 1;
    if (sci >= 3 && sci <= 14 && sci != 4 && sci != 7 && sci != 8 && sci != 12) {
      acpi_irq = (int)sci;
      if (sci < 8) outb(0x21, (u8)(inb(0x21) & ~(1u << sci)));
      else outb(0xA1, (u8)(inb(0xA1) & ~(1u << (sci - 8))));
    } else polled = 1;
  }
  log(have_s5 ? "c4pc: ACPI: off by S5" : "c4pc: ACPI, but no \\_S5: off by QEMU's and Bochs's ports");
  log(!button_ok ? ", no power button\n" : cm ? ", the power button (a control method one: maybe not) shuts C4IX down" : ", the power button shuts C4IX down");
  if (button_ok) log(polled ? " (polled)\n" : " (by the SCI)\n");
}

/* the SCI (pc.c's isr, before the EOI): a press counted, and every status
 * cleared, or the level-triggered line stays up. Raised with nothing to
 * say, many times over: masked, and the button polled from then on */
void acpi_sci(void) {
  uint16_t s = inw(pm1a_evt);
  if (pm1b_evt) s |= inw(pm1b_evt);
  outw(pm1a_evt, s);
  if (pm1b_evt) outw(pm1b_evt, s);
  for (u32 i = 0; i < gpe0_len / 2; i++) { u8 g = inb(gpe0 + i); if (g) outb(gpe0 + i, g); }
  for (u32 i = 0; i < gpe1_len / 2; i++) { u8 g = inb(gpe1 + i); if (g) outb(gpe1 + i, g); }
  if (s & 0x100) { presses++; return; }
  if (++spurious > 10000 && acpi_irq >= 0) {
    if (acpi_irq < 8) outb(0x21, (u8)(inb(0x21) | (1u << acpi_irq)));
    else outb(0xA1, (u8)(inb(0xA1) | (1u << (acpi_irq - 8))));
    acpi_irq = -1;
    polled = 1;
  }
}

/* a press of the power button since last asked (NOW, in ms: polled, every
 * 50 ms): 1 */
int acpi_button(i64 now) {
  static i64 last;
  if (presses) { presses = 0; return 1; }
  if (!button_ok || !polled || now - last < 50) return 0;
  last = now;
  uint16_t s = inw(pm1a_evt);
  if (pm1b_evt) s |= inw(pm1b_evt);
  if (!(s & 0x100)) return 0;
  outw(pm1a_evt, 0x100);
  if (pm1b_evt) outw(pm1b_evt, 0x100);
  return 1;
}

void acpi_off(void) {
  if (!have_s5 || !pm1a_cnt) return;
  acpi_mode();
  outw(pm1a_cnt, (uint16_t)((inw(pm1a_cnt) & 0xE3FF) | slp_a << 10 | 0x2000));
  if (pm1b_cnt) outw(pm1b_cnt, (uint16_t)((inw(pm1b_cnt) & 0xE3FF) | slp_b << 10 | 0x2000));
  for (volatile int i = 0; i < 10000000; i++) ;
}

void acpi_reset(void) {
  if (reset_ok) outb(reset_port, reset_val);
  for (volatile int i = 0; i < 1000000; i++) ;
  outb(0xCF9, 0x02); for (volatile int i = 0; i < 1000; i++) ; outb(0xCF9, 0x06);
  for (volatile int i = 0; i < 1000000; i++) ;
  for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) ;
  outb(0x64, 0xFE);
  for (volatile int i = 0; i < 1000000; i++) ;
  /* a triple fault: no IDT at all, then an interrupt */
  static const struct { uint16_t lim; uintptr_t base; } __attribute__((packed)) none = { 0, 0 };
  __asm__ volatile("lidt %0; int3" : : "m"(none));
}
