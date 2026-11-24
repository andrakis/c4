/* hv.c -- c4pc as a hypervisor (docs/c4pc-hypervisor.md): with --hv the
 * machine pc.c boots is VM 0, the control C4IX, and it may make others --
 * c4 machines of their own, each with its arena, display and drives --
 * start and stop them, and attach to one, which then has the screen, the
 * keyboard and the mouse until it detaches (Ctrl+Alt+Home, or `detach` in
 * a C4IX guest) and VM 0 has them again.
 *
 * The machine manager: a window in every machine's page 0 (HV_BASE),
 * user mode's where the MMU's IOPM allows the devices (C4IX's `vm`
 * command, uioperm). A command is its arguments (A0..A3), then CMD; RES
 * answers. VM 0 alone makes, starts, stops and attaches; any machine may
 * ask its id and detach.
 *
 * The guests run on CPU 0's thread, in turn with VM 0: a slice of 4 ms of
 * each runnable guest after each of VM 0's (hv_run, and while VM 0 naps).
 * A guest naps as VM 0 does (STOP_SLEEP: its nap's end on the real clock).
 * Its console is the serial line's, a line at a time ("vm1 NAME: ..."),
 * and the PC's log (pc.c: VM 0 reads it, LOG); it is also a text screen
 * of its own (vterm.h), which an attached guest with no display ring has
 * on the PC's screen, the keyboard typing to it (hv_key), as C4IX in
 * text mode, C4KE or C4DOS's prompt are.
 * Its accelerators are off (theirs keep state a CPU at a time, which VM
 * 0's CPU 0 has already), and it has no swap of the host's.
 */
#include "../c4pc.h"
#include "../clib.h"
#include "vterm.h"

extern u32 pc_heap_free_bytes;

enum { HV_MAX = 8 };
enum { R_ID = 0x560, R_A0 = 0x564, R_A1 = 0x568, R_A2 = 0x56c, R_A3 = 0x570, R_CMD = 0x574, R_RES = 0x578, R_COUNT = 0x57c };
enum { C_NAME = 1, C_MB = 2, C_ARGS = 3, C_NEW = 4, C_START = 5, C_STOP = 6, C_ATTACH = 7, C_DETACH = 8,
       C_STATE = 9, C_INFO = 10, C_DELETE = 11, C_DISK = 12, C_ATTACHED = 13, C_CDROM = 14, C_SEND = 15,
       C_THUMB = 16, C_LOG = 17 };
enum { S_FREE, S_MADE, S_RUN, S_HALT };
static const char *STATE_NAME[] = { "free", "made", "running", "halted" };

typedef struct vm {
  int state; char name[32]; u32 mb; char args[256];
  u8 *img; u32 img_len; u8 *tar; u32 tar_len;
  machine *m; drive *drives[4];
  blkdev *disk;                        /* its hd0, a file of VM 0's (hv_disk) */
  struct xmap *cd; u32 cd_blocks;      /* its CD, an ISO file of VM 0's */
  drive *pc_cd;                        /* ... or VM 0's CD drive, shared */
  i64 nap_until; int status;
  char line[200]; int ll;              /* its console's line, being gathered */
  vterm vt;                            /* its console as a text screen */
  int mmu;                             /* 1 an MMU, 2 none, 0 as c4pc's --mmu says */
  i32 a[4], res;                       /* its own window's arguments and answer */
} vm;
static vm vms[HV_MAX];                 /* [0]: the control machine, VM 0 */
static machine *ctl;
static int attached, hv_mmu, hv_w, hv_h;
static double hv_mhz;
static char pend_name[32], pend_args[256];
static u32 pend_mb = 64;
static int pend_mmu;
void hv_out(const char *s);            /* pc.c: to the serial line alone */

static void outn(i64 v) { char b[24]; int k = 0; uint64_t x = v < 0 ? (uint64_t)-v : (uint64_t)v; do b[k++] = (char)('0' + x % 10); while (x /= 10); if (v < 0) b[k++] = '-'; char o[24]; int n = 0; while (k) o[n++] = b[--k]; o[n] = 0; hv_out(o); }

static int vm_of(machine *m) { int id = m->dv->hv_id; return id >= 0 && id < HV_MAX ? id : 0; }

/* a guest's console, a line at a time on the serial line */
static void guest_byte(machine *m, int b) {
  vm *v = &vms[vm_of(m)];
  vt_put(&v->vt, b);
  if (b == '\r') return;
  if (b != '\n' && v->ll < (int)sizeof v->line - 1) { v->line[v->ll++] = (char)b; return; }
  v->line[v->ll] = 0;
  hv_out("vm"); outn(vm_of(m)); hv_out(" "); hv_out(v->name); hv_out(": "); hv_out(v->line); hv_out("\n");
  v->ll = 0;
}

static i32 hv_rd(machine *m, u32 a);
static void hv_wr(machine *m, u32 a, i32 val);

/* a string at ADDR in VM 0's memory, into OUT (at most N - 1 bytes) */
static void ctl_string(u32 addr, char *out, u32 n) {
  u32 k = dma_cstring(ctl, addr, out, n - 1);
  out[k < n ? k : n - 1] = 0;
}
/* LEN bytes at ADDR in VM 0's memory, copied: 0 if they cannot be */
static u8 *ctl_copy(u32 addr, u32 len) {
  if (!len || len > (256u << 20) || pc_heap_free_bytes < len + (64u << 20)) return 0;
  u8 *p = xmalloc(len);
  if (dev_dma_read(ctl, addr, len, p) != len) { xfree(p); return 0; }
  return p;
}

/* ---- a guest's disk or CD: a file of VM 0's on an IDE disk, as the
 * sectors its ext2 keeps it in (C4IX's SYS_FMAP: `vm` gives them as
 * extents). Read and written on the PC's disk directly, with no turn of
 * VM 0's; VM 0 leaves the file alone while the guest runs (`vm` sees to
 * it). An extent: its first sector in the image, the disk (VM 0's hdN,
 * the PC's block device N), its first sector there, how many; a hole
 * reads zeros ---- */
struct xmap { u32 n, sectors; u32 *e; };             /* e: 4 words an extent */
static struct xmap *xmap_new(u32 table, u32 n, u32 sectors) {
  if (!n || n > (1u << 20)) return 0;
  struct xmap *x = xmalloc(sizeof *x);
  x->n = n; x->sectors = sectors;
  x->e = xmalloc((size_t)n * 16);
  if (dev_dma_read(ctl, table, n * 16, (u8 *)x->e) != n * 16) { xfree(x->e); xfree(x); return 0; }
  for (u32 k = 0; k < n; k++)
    if (x->e[4 * k + 1] >= (u32)ctl->nblk || x->e[4 * k + 2] + x->e[4 * k + 3] < x->e[4 * k + 2] ||
        x->e[4 * k + 2] + x->e[4 * k + 3] > ctl->blk[x->e[4 * k + 1]]->sectors) { xfree(x->e); xfree(x); return 0; }
  return x;
}
/* N sectors at LBA of the image: 0, or -1 */
static int xmap_rw(struct xmap *x, int write, u32 lba, u32 n, u8 *buf) {
  if (lba >= x->sectors || n > x->sectors - lba) return -1;
  while (n) {
    u32 lo = 0, hi = x->n;
    while (hi - lo > 1) { u32 mid = (lo + hi) / 2; if (x->e[4 * mid] <= lba) lo = mid; else hi = mid; }
    u32 *e = x->e + 4 * lo, k;
    if (lba >= e[0] && lba < e[0] + e[3]) {
      k = e[0] + e[3] - lba; if (k > n) k = n;
      blkdev *b = ctl->blk[e[1]];
      if (b->rw(b, write, e[2] + (lba - e[0]), k, buf)) return -1;
    } else {
      /* a hole: up to the next extent */
      u32 next = lba < e[0] ? e[0] : (lo + 1 < x->n ? x->e[4 * (lo + 1)] : x->sectors);
      k = next - lba; if (k > n) k = n;
      if (write) return -1;
      memset(buf, 0, (size_t)k * 512);
    }
    lba += k; n -= k; buf += (size_t)k * 512;
  }
  return 0;
}
typedef struct { blkdev b; struct xmap *x; } xblk;
static int xblk_rw(blkdev *b, int write, u32 lba, u32 n, u8 *buf) { return xmap_rw(((xblk *)b)->x, write, lba, n, buf); }
static blkdev *xmap_blk(struct xmap *x, const char *name) {
  xblk *d = xmalloc(sizeof *d);
  memset(d, 0, sizeof *d);
  d->b.sectors = x->sectors; d->b.flags = 0; d->b.rw = xblk_rw; d->b.ctx = x; d->x = x;
  const char *t = "a file of VM 0's: ";
  int k = 0;
  while (*t && k < 47) d->b.what[k++] = *t++;
  while (*name && k < 47) d->b.what[k++] = *name++;
  d->b.what[k] = 0;
  return &d->b;
}
/* the CD's 2048-byte blocks */
static int xmap_cd_rd(void *ctx, u32 lba, u32 n, u8 *buf) { return xmap_rw(ctx, 0, lba * 4, n * 4, buf); }

static int start(int id) {
  vm *v = &vms[id];
  if (v->state != S_MADE && v->state != S_HALT) return -1;
  if (pc_heap_free_bytes < (v->mb << 20) + (48u << 20)) { hv_out("c4pc: hv: no room for vm"); outn(id); hv_out("'s memory\n"); return -2; }
  v->drives[0] = drive_new(0);
  if (v->tar) drive_add_tar(v->drives[0], v->tar, v->tar_len);
  v->drives[0]->kind = "boot";
  int nd = 1;
  if (v->cd || v->pc_cd) {
    v->drives[1] = drive_new(0); v->drives[1]->kind = "none";
    v->drives[2] = v->pc_cd ? v->pc_cd : cdrom_image(xmap_cd_rd, v->cd, v->cd_blocks, "an ISO of VM 0's");
    nd = 3;
  }
  machine *m = machine_new(v->mb << 20, v->drives, nd, hv_mhz);
  m->on_byte = guest_byte;
  m->accel = m->osaccel = 0;
  m->dv->swap_slots = 0;
  if (v->disk) m->blk[m->nblk++] = v->disk;
  if (v->mmu == 1 || (!v->mmu && hv_mmu)) mmu_new(m);
  m->gui = gui_new(m, hv_w, hv_h);
  vt_init(&v->vt, 0);
  m->dv->hv_rd = hv_rd; m->dv->hv_wr = hv_wr; m->dv->hv_id = id;
  m->wall = 1; m->wall_base = plat_now_ms();
  /* its arguments: the image's name, then the words of ARGS */
  static char abuf[256]; static char *argv[24];
  memcpy(abuf, v->args, sizeof abuf);
  int argc = 0;
  argv[argc++] = "c4ix.c4r";
  for (char *p = abuf; *p && argc < 23;) {
    while (*p == ' ') *p++ = 0;
    if (!*p) break;
    argv[argc++] = p;
    while (*p && *p != ' ') p++;
  }
  argv[argc] = 0;
  if (machine_boot(m, v->img, v->img_len, argc, argv)) { hv_out("c4pc: hv: vm"); outn(id); hv_out(" did not boot\n"); return -3; }
  v->m = m; v->state = S_RUN; v->nap_until = 0; v->ll = 0;
  hv_out("c4pc: hv: vm"); outn(id); hv_out(" "); hv_out(v->name); hv_out(" started, "); outn(v->mb); hv_out(" MB\n");
  return 0;
}

void hv_attach(int id) {
  if (id < 0 || id >= HV_MAX) return;
  if (id && (vms[id].state != S_RUN || !vms[id].m || !vms[id].m->gui)) return;
  if (attached == id) return;
  attached = id;
  machine *m = id ? vms[id].m : ctl;
  extern void screen_full(void);
  if (m->gui) gui_redraw_all(m->gui);
  screen_full();
  hv_out("c4pc: hv: the screen is vm"); outn(id); hv_out("'s"); if (id) { hv_out(" ("); hv_out(vms[id].name); hv_out(")"); }
  hv_out(id ? "; Ctrl+Alt+Home comes back\n" : "\n");
}
/* the machine the screen, keyboard and mouse are: VM 0's or an attached guest's */
machine *hv_shown(machine *m0) {
  if (attached && vms[attached].state == S_RUN && vms[attached].m && vms[attached].m->gui) return vms[attached].m;
  return m0;
}

/* does guest V show its text screen: no program has its display (no ring,
 * nothing flipped) */
int gui_ring(gui *g);
u32 gui_flips(gui *g);
static int text_shown(vm *v) { return v->m && v->m->gui && !gui_ring(v->m->gui) && !gui_flips(v->m->gui); }
const u16 *hv_text(machine *m, int *cur) {
  int id = vm_of(m);
  if (!id || !ctl || m == ctl || vms[id].state != S_RUN || !text_shown(&vms[id])) return 0;
  *cur = vt_cursor(&vms[id].vt);
  return vms[id].vt.cells;
}
/* a key from the PC's keyboard, for the attached guest if it shows its text
 * screen: given to it as a terminal would (echoed, backspace taken back,
 * ^C its SIGINT, ^D its end of input), unless it reads the keys raw */
int hv_key(int c) {
  if (!attached || vms[attached].state != S_RUN || !text_shown(&vms[attached])) return 0;
  vm *v = &vms[attached];
  machine *g = v->m;
  v->nap_until = 0;
  if (g->raw_kbd > 0) { kbd_push(g, c); return 1; }
  if (c == 4) { g->rx_eof = 1; return 1; }
  if (c == 3) { if (g->sigh[2]) g->pending_signal = 2; return 1; }
  if (c == 127 || c == 8) {
    if (g->rx_tail > g->rx_head && g->rx[g->rx_tail - 1] != 10) { g->rx_tail--; vt_put(&v->vt, '\b'); vt_put(&v->vt, ' '); vt_put(&v->vt, '\b'); }
    return 1;
  }
  kbd_push(g, c);
  /* echoed, but not an arrow's or Home's escape sequence (ESC [ ... a letter or ~) */
  static int esc;
  if (c == 27) esc = 1;
  else if (esc && ((c >= 64 && c <= 126 && c != 91) || esc > 8)) esc = 0;
  else if (esc) esc++;
  else vt_put(&v->vt, c);
  return 1;
}

static i32 info(int id, u32 addr, u32 len) {
  if (id < 0 || id >= HV_MAX) return -1;
  vm *v = &vms[id];
  char b[200]; int n = 0;
#define PUT(s) do { const char *q_ = (s); while (*q_ && n < (int)sizeof b - 1) b[n++] = *q_++; } while (0)
#define NUM(x) do { char t_[24]; int k_ = 0; uint64_t x_ = (uint64_t)(x); do t_[k_++] = (char)('0' + x_ % 10); while (x_ /= 10); while (k_ && n < (int)sizeof b - 1) b[n++] = t_[--k_]; } while (0)
  PUT("id "); NUM(id); PUT("\nname "); PUT(id ? v->name : "control"); PUT("\nstate "); PUT(id ? STATE_NAME[v->state] : "running");
  PUT("\nmb "); NUM(id ? v->mb : ctl->size >> 20);
  if (id && v->m) { PUT("\ncycles "); NUM((uint64_t)v->m->cycle); }
  if (id && v->state == S_HALT) { PUT("\nstatus "); NUM((u32)v->status); }
  if (id) { PUT("\nmmu "); PUT(v->mmu == 1 || (!v->mmu && hv_mmu) ? "yes" : "no"); }
  if (id && v->state == S_RUN) { PUT("\nscreen "); PUT(text_shown(v) ? "text" : "display"); }
  PUT("\nattached "); PUT(attached == id ? "yes" : "no"); PUT("\n");
#undef PUT
#undef NUM
  b[n] = 0;
  if ((u32)n + 1 > len) n = (int)len - 1;
  if (n < 0) return -1;
  dev_dma_write(ctl, addr, (const u8 *)b, (u32)n + 1);
  return n;
}

static i32 command(machine *m, i32 c) {
  int me = vm_of(m);
  vm *v = &vms[me];
  i32 *a = v->a;
  if (c == C_DETACH) { hv_attach(0); return 0; }
  if (c == C_ATTACHED) return attached;
  if (me != 0) return -1;              /* the rest are the control machine's */
  switch (c) {
    case C_NAME: ctl_string((u32)a[0], pend_name, sizeof pend_name); return 0;
    /* A0 MB of memory; A1 its MMU: 1 one, 2 none, 0 as c4pc's --mmu */
    case C_MB: pend_mb = a[0] < 4 ? 4 : (u32)a[0] > 1024 ? 1024 : (u32)a[0]; pend_mmu = a[1] == 1 || a[1] == 2 ? a[1] : 0; return 0;
    case C_ARGS: ctl_string((u32)a[0], pend_args, sizeof pend_args); return 0;
    case C_NEW: {
      int id = 1;
      while (id < HV_MAX && vms[id].state != S_FREE) id++;
      if (id == HV_MAX) return -1;
      vm *g = &vms[id];
      memset(g, 0, sizeof *g);
      if (!(g->img = ctl_copy((u32)a[0], (u32)a[1]))) return -2;
      if (a[3] && !(g->tar = ctl_copy((u32)a[2], (u32)a[3]))) { xfree(g->img); g->img = 0; return -2; }
      g->img_len = (u32)a[1]; g->tar_len = (u32)a[3];
      memcpy(g->name, pend_name[0] ? pend_name : "guest", sizeof g->name);
      g->name[sizeof g->name - 1] = 0;
      memcpy(g->args, pend_args, sizeof g->args);
      g->mb = pend_mb; g->mmu = pend_mmu;
      g->state = S_MADE;
      pend_name[0] = 0; pend_args[0] = 0; pend_mb = 64; pend_mmu = 0;
      hv_out("c4pc: hv: vm"); outn(id); hv_out(" "); hv_out(g->name); hv_out(" made\n");
      return id;
    }
    case C_START: return a[0] > 0 && a[0] < HV_MAX ? start(a[0]) : -1;
    /* bytes typed at a running guest's console (`vm send`) */
    case C_SEND: {
      if (a[0] <= 0 || a[0] >= HV_MAX || vms[a[0]].state != S_RUN || a[2] < 0 || a[2] > 4096) return -1;
      char b[4096];
      if (dev_dma_read(ctl, (u32)a[1], (u32)a[2], (u8 *)b) != (u32)a[2]) return -1;
      for (i32 k = 0; k < a[2]; k++) kbd_push(vms[a[0]].m, (u8)b[k]);
      vms[a[0]].nap_until = 0;
      return a[2];
    }
    case C_STOP: {
      if (a[0] <= 0 || a[0] >= HV_MAX || vms[a[0]].state != S_RUN) return -1;
      machine *g = vms[a[0]].m;
      /* SIGTERM, as the power button: C4IX shuts down; or at once (A1) */
      if (a[1] || !g->sigh[15]) { g->halted = 1; g->status = 0; }
      else g->pending_signal = 15;
      return 0;
    }
    case C_ATTACH: hv_attach(a[0]); return attached == a[0] ? 0 : -1;
    case C_STATE: return a[0] >= 0 && a[0] < HV_MAX ? (a[0] ? vms[a[0]].state : S_RUN) : -1;
    case C_INFO: return info(a[0], (u32)a[1], (u32)a[2]);
    /* a running guest's screen scaled to A2 x A3, a word a pixel at A1 in
     * VM 0's memory (C4IX's Machines shows it) */
    case C_THUMB: {
      if (a[0] <= 0 || a[0] >= HV_MAX || vms[a[0]].state != S_RUN || !vms[a[0]].m || !vms[a[0]].m->gui) return -1;
      if (a[2] <= 0 || a[3] <= 0 || a[2] > 640 || a[3] > 480) return -1;
      u32 n = (u32)a[2] * (u32)a[3];
      u32 *px = xmalloc((size_t)n * 4);
      if (text_shown(&vms[a[0]])) {
        /* its text screen: drawn at 640 x 400, then scaled as gui_thumb scales */
        extern void screen_text_pixels(const u16 *cells, int cur, u32 *out);
        u32 *t = xmalloc((size_t)640 * 400 * 4);
        screen_text_pixels(vms[a[0]].vt.cells, vt_cursor(&vms[a[0]].vt), t);
        for (i32 y = 0; y < a[3]; y++) for (i32 x = 0; x < a[2]; x++)
          px[(u32)y * (u32)a[2] + (u32)x] = t[(u32)(y * 400 / a[3]) * 640 + (u32)(x * 640 / a[2])];
        xfree(t);
      } else if (gui_thumb(vms[a[0]].m->gui, px, a[2], a[3]) < 0) { xfree(px); return -1; }
#if WB == 4
      u32 got = dev_dma_write(ctl, (u32)a[1], (const u8 *)px, n * 4);
#else
      u8 *wb = xmalloc((size_t)n * WB);
      memset(wb, 0, (size_t)n * WB);
      for (u32 k = 0; k < n; k++) memcpy(wb + (size_t)k * WB, &px[k], 4);
      u32 got = dev_dma_write(ctl, (u32)a[1], wb, n * WB) / (WB / 4);
      xfree(wb);
#endif
      xfree(px);
      return got == n * 4 ? 0 : -1;
    }
    /* the PC's log (pc.c), its last A1 bytes at most, at A0 in VM 0's
     * memory, ended by a 0: how many */
    case C_LOG: {
      extern u32 pc_log_copy(char *out, u32 max);
      if (a[1] < 2) return -1;
      u32 max = (u32)a[1] - 1 > 32768 ? 32768 : (u32)a[1] - 1;
      char *b = xmalloc(max + 1);
      u32 n = pc_log_copy(b, max);
      b[n] = 0;
      dev_dma_write(ctl, (u32)a[0], (const u8 *)b, n + 1);
      xfree(b);
      return (i32)n;
    }
    case C_DELETE: {
      if (a[0] <= 0 || a[0] >= HV_MAX || vms[a[0]].state == S_RUN) return -1;
      vm *g = &vms[a[0]];
      xfree(g->img); xfree(g->tar);
      g->img = g->tar = 0; g->m = 0;     /* (its machine, halted, is not freed: the engine has no free) */
      g->state = S_FREE;
      return 0;
    }
    case C_DISK: case C_CDROM: {
      if (a[0] <= 0 || a[0] >= HV_MAX || vms[a[0]].state == S_RUN || vms[a[0]].state == S_FREE) return -1;
      if (c == C_CDROM && !a[2]) {
        /* VM 0's first CD drive (`vm cdrom ID pc`): both read the disc */
        for (int k = 0; k < ctl->dv->ndrives; k++)
          if (ctl->dv->drives[k]->rescan)   /* a CD drive (cdrom.c: the only drives that look again) */ { vms[a[0]].pc_cd = ctl->dv->drives[k]; vms[a[0]].cd = 0; return 0; }
        return -2;
      }
      struct xmap *x = xmap_new((u32)a[1], (u32)a[2], (u32)a[3]);
      if (!x) return -2;
      if (c == C_DISK) vms[a[0]].disk = xmap_blk(x, vms[a[0]].name);
      else { vms[a[0]].cd = x; vms[a[0]].cd_blocks = (u32)a[3] / 4; }
      return 0;
    }
  }
  return -1;
}

static i32 hv_rd(machine *m, u32 a) {
  vm *v = &vms[vm_of(m)];
  switch (a) {
    case R_ID: return vm_of(m);
    case R_A0: return v->a[0]; case R_A1: return v->a[1]; case R_A2: return v->a[2]; case R_A3: return v->a[3];
    case R_RES: return v->res;
    case R_COUNT: return HV_MAX;
  }
  return 0;
}
static void hv_wr(machine *m, u32 a, i32 val) {
  vm *v = &vms[vm_of(m)];
  switch (a) {
    case R_A0: v->a[0] = val; return; case R_A1: v->a[1] = val; return;
    case R_A2: v->a[2] = val; return; case R_A3: v->a[3] = val; return;
    case R_CMD: v->res = command(m, val); return;
  }
}

/* --hv: M is VM 0, the control machine */
void hv_init(machine *m, double mhz, int mmu_on, int w, int h) {
  ctl = m; hv_mhz = mhz; hv_mmu = mmu_on; hv_w = w; hv_h = h;
  m->dv->hv_rd = hv_rd; m->dv->hv_wr = hv_wr; m->dv->hv_id = 0;
  vms[0].state = S_RUN;
  hv_out("c4pc: hv: the hypervisor: this machine is VM 0, the control; `vm` makes others\n");
}

/* a turn for each runnable guest: 1 if any ran */
int hv_run(void) {
  if (!ctl) return 0;
  int busy = 0;
  i64 now = plat_now_ms();
  for (int id = 1; id < HV_MAX; id++) {
    vm *v = &vms[id];
    if (v->state != S_RUN) continue;
    machine *m = v->m;
    if (!m->halted && v->nap_until > now) continue;
    int r = m->halted ? STOP_HALT : machine_run(m, (i64)m->cycles_per_ms * 4);
    busy = 1;
    if (m->gui) gui_tick(m->gui);
    if (r == STOP_HALT || m->halted) {
      v->state = S_HALT; v->status = m->status;
      if (v->ll) guest_byte(m, '\n');
      hv_out("c4pc: hv: vm"); outn(id); hv_out(" "); hv_out(v->name); hv_out(" halted, status "); outn(v->status); hv_out("\n");
      if (attached == id) hv_attach(0);
      continue;
    }
    if (r == STOP_SLEEP) v->nap_until = m->wall_base + m->nap_until;
    else if (r == STOP_INPUT) v->nap_until = now + 10;
  }
  return busy;
}
/* the soonest a napping guest wakes (ms, the real clock), or -1 */
i64 hv_next_wake(void) {
  i64 best = -1;
  for (int id = 1; id < HV_MAX; id++)
    if (vms[id].state == S_RUN && (best < 0 || vms[id].nap_until < best)) best = vms[id].nap_until;
  return best;
}
int hv_on(void) { return ctl != 0; }
