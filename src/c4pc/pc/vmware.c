/* vmware.c -- VMware's guest integration, without VMware Tools: what
 * open-vm-tools and Linux's vmmouse driver do, over VMware's "backdoor"
 * I/O port (open-vm-tools' backdoor_def.h, guest_msg_def.h, message.c,
 * rpcout.c, hgfsProto.h, hgfsBd.c; Linux's drivers/input/mouse/vmmouse.c;
 * docs/c4pc.md, "VMware").
 *
 *   - the backdoor: IN on port 0x5658 with EAX the magic 'VMXh', ECX the
 *     command; VMware answers in the registers. Not there (a real PC, a
 *     VM of another make), EBX is not given back the magic.
 *   - the mouse, absolute (vmmouse): the pointer goes where the host's
 *     is, and leaves the VM's window at its edge with no grab, no
 *     Ctrl+Alt. A PS/2 packet still comes to say there is something; the
 *     positions are read from the backdoor (ABSPOINTER_*), 0..65535 across
 *     the screen. A program that locks the pointer (a game) has relative
 *     motion while it does.
 *   - copy and paste, VMware's first way (copy/paste version 1): the
 *     host's text is GETSELLENGTH, then GETNEXTPIECE four bytes at a time;
 *     the guest's is SETSELLENGTH, then SETNEXTPIECE.
 *   - GuestRPC: a channel (BDOOR_CMD_MESSAGE) of messages, a size, then
 *     the bytes four at a time, or at once by the high-bandwidth port
 *     (0x5659, REP OUTSB/INSB) where VMware has it. RPCI asks VMware
 *     things ("1 " or "0 " before its answer); TCLO is VMware asking us:
 *     "ping", "reset", "OS_Halt" -- answered so that VMware takes Tools to
 *     be running (its copy and paste, its shared folders, its Shut Down
 *     Guest).
 *   - shared folders (HGFS, protocol version 1, packed structures): a
 *     request is RPCI's "f " and the packet. A drive (kind vmware) has
 *     them: its hgfs/SHARE/... are the host's folders, listed and read when
 *     wanted, and written whole when a file written there is closed. Its
 *     file `clipboard` is the host's text, and a file `clipboard.set`
 *     written is the guest's, for the desktop (src/c4ix/user/desktop).
 */
#include "../c4pc.h"
#include "../clib.h"

enum { BDOOR_MAGIC = 0x564D5868, BDOOR_PORT = 0x5658, BDOORHB_PORT = 0x5659 };
enum { CMD_GETSELLENGTH = 6, CMD_GETNEXTPIECE = 7, CMD_SETSELLENGTH = 8, CMD_SETNEXTPIECE = 9,
       CMD_GETVERSION = 10, CMD_MESSAGE = 30, CMD_ABS_DATA = 39, CMD_ABS_STATUS = 40, CMD_ABS_COMMAND = 41 };
enum { MSG_OPEN, MSG_SENDSIZE, MSG_SENDPAYLOAD, MSG_RECVSIZE, MSG_RECVPAYLOAD, MSG_RECVSTATUS, MSG_CLOSE };
enum { ST_SUCCESS = 1, ST_DORECV = 2, ST_CPT = 0x10, ST_HB = 0x80 };
#define PROTO_RPCI 0x49435052u
#define PROTO_TCLO 0x4f4c4354u
#define FLAG_COOKIE 0x80000000u

typedef struct { u32 ax, bx, cx, dx, si, di; } regs;

static int present, use_hb = 1;

/* a low-bandwidth backdoor call: R in, R out */
static void bdoor(regs *r) {
  r->ax = BDOOR_MAGIC;
  r->dx = (r->dx & 0xffff0000u) | BDOOR_PORT;
  __asm__ volatile("inl %%dx, %%eax"
                   : "+a"(r->ax), "+b"(r->bx), "+c"(r->cx), "+d"(r->dx), "+S"(r->si), "+D"(r->di) : : "memory");
}
/* the high-bandwidth port: N bytes out of P (rep outsb) or into P (rep insb).
 * EBP holds a cookie half, so it is saved around the call. EBX's high half
 * is the status out */
static u32 hb_bp;                              /* EBP's value, read after EBP is saved */
static u32 hb_out(u32 id, u32 bpv, u32 di, const u8 *p, u32 n) {
  hb_bp = bpv;
  u32 a = BDOOR_MAGIC, b = (u32)ST_SUCCESS << 16, c = n, d = BDOORHB_PORT | id << 16;
  uintptr_t s = (uintptr_t)p, dd = di;
#ifdef __x86_64__
  __asm__ volatile("push %%rbp; mov %6, %%ebp; cld; rep outsb; pop %%rbp"
                   : "+a"(a), "+b"(b), "+c"(c), "+d"(d), "+S"(s), "+D"(dd) : "m"(hb_bp) : "memory");
#else
  __asm__ volatile("push %%ebp; mov %6, %%ebp; cld; rep outsb; pop %%ebp"
                   : "+a"(a), "+b"(b), "+c"(c), "+d"(d), "+S"(s), "+D"(dd) : "m"(hb_bp) : "memory");
#endif
  return b >> 16;
}
static u32 hb_in(u32 id, u32 si, u32 bpv, u8 *p, u32 n) {
  hb_bp = bpv;
  u32 a = BDOOR_MAGIC, b = (u32)ST_SUCCESS << 16, c = n, d = BDOORHB_PORT | id << 16;
  uintptr_t s = si, dd = (uintptr_t)p;
#ifdef __x86_64__
  __asm__ volatile("push %%rbp; mov %6, %%ebp; cld; rep insb; pop %%rbp"
                   : "+a"(a), "+b"(b), "+c"(c), "+d"(d), "+S"(s), "+D"(dd) : "m"(hb_bp) : "memory");
#else
  __asm__ volatile("push %%ebp; mov %6, %%ebp; cld; rep insb; pop %%ebp"
                   : "+a"(a), "+b"(b), "+c"(c), "+d"(d), "+S"(s), "+D"(dd) : "m"(hb_bp) : "memory");
#endif
  return b >> 16;
}

int vmw_present(void) { return present; }
/* the host's time, microseconds since 1970 (BDOOR_CMD_GETTIMEFULL, 46:
 * seconds in ESI:EDX, microseconds in EBX, EAX the magic): 1, or 0 where
 * the host gives none. VMware's TSC is the guest's "apparent" time, run
 * fast to catch up after the VM fell behind: Quake played the first
 * seconds of its demo fast (pc.c's clock_vmw) */
u32 vmw_time_regs[4];                   /* the last time call's EAX EBX EDX ESI, for the boot's diagnosis */
static int time_cmd = 46;               /* GETTIMEFULL, else GETTIME (23: QEMU's vmport has only it) */
int vmw_host_us(uint64_t *us) {
  if (!present || !time_cmd) return 0;
  if (time_cmd == 46) {
    regs r = { 0, 0, 46, 0, 0, 0 };
    bdoor(&r);
    vmw_time_regs[0] = r.ax; vmw_time_regs[1] = r.bx; vmw_time_regs[2] = r.dx; vmw_time_regs[3] = r.si;
    if (r.ax == BDOOR_MAGIC && !(r.si == 0 && r.dx < 1000000000u) && r.bx < 1000000u) {
      *us = ((uint64_t)r.si << 32 | r.dx) * 1000000u + r.bx;
      return 1;
    }
    time_cmd = 23;
  }
  /* GETTIME: seconds in EAX, microseconds in EBX */
  regs r = { 0, 0, 23, 0, 0, 0 };
  bdoor(&r);
  if (r.ax == 0xffffffffu || r.ax == BDOOR_MAGIC || r.ax < 1000000000u || r.bx >= 1000000u) { time_cmd = 0; return 0; }
  *us = (uint64_t)r.ax * 1000000u + r.bx;
  return 1;
}
/* is this VMware's (or QEMU's vmport): 1 */
int vmw_detect(int no_hb) {
  regs r = { 0, ~(u32)BDOOR_MAGIC, CMD_GETVERSION, 0, 0, 0 };
  bdoor(&r);
  present = r.bx == BDOOR_MAGIC && r.ax != 0xffffffffu;
  use_hb = !no_hb;
  return present;
}

/* ---- the mouse ---- */
enum { ABS_ENABLE = 0x45414552u, ABS_DISABLE = 0xf5, ABS_RELATIVE = 0x4c455252u, ABS_ABSOLUTE = 0x53424152u,
       ABS_VERSION = 0x3442554au, ABS_ERROR = 0xffff0000u, ABS_REL_PACKET = 0x10000 };
static int mouse_on, mouse_rel;
extern int mouse_debug;                 /* --debug-mouse (screen.c) */
void pc_log(const char *s);
void pc_logn(i64 v);
static u32 call1(u32 cmd, u32 arg) { regs r = { 0, arg, cmd, 0, 0, 0 }; bdoor(&r); return r.ax; }
/* the absolute pointer, where VMware (or QEMU's vmmouse) has it: 1 */
int vmw_mouse_init(void) {
  if (!present) return 0;
  call1(CMD_ABS_COMMAND, ABS_ENABLE);
  u32 st = call1(CMD_ABS_STATUS, 0);
  if ((st & 0xffff) == 0 || (st & ABS_ERROR) == ABS_ERROR) return 0;
  if (call1(CMD_ABS_DATA, 1) != ABS_VERSION) { call1(CMD_ABS_COMMAND, ABS_DISABLE); return 0; }
  call1(CMD_ABS_COMMAND, ABS_ABSOLUTE);
  mouse_on = 1; mouse_rel = 0;
  return 1;
}
int vmw_mouse(void) { return mouse_on; }
/* what the pointer did, to the display: LOCKED, a program has the pointer
 * (relative motion wanted). 0 if the channel went wrong (the PS/2 mouse's
 * packets are used again) */
int vmw_mouse_poll(gui *g, int locked) {
  if (!mouse_on) return 0;
  if (locked != mouse_rel) { call1(CMD_ABS_COMMAND, locked ? ABS_RELATIVE : ABS_ABSOLUTE); mouse_rel = locked; }
  for (int k = 0; k < 64; k++) {
    u32 st = call1(CMD_ABS_STATUS, 0);
    if ((st & ABS_ERROR) == ABS_ERROR) { mouse_on = 0; return 0; }
    u32 q = st & 0xffff;
    if (!q) break;
    if (q % 4) { mouse_on = 0; return 0; }
    regs r = { 0, 4, CMD_ABS_DATA, 0, 0, 0 };
    bdoor(&r);
    /* left, right, middle to the display's left, middle, right */
    int b = ((r.ax & 0x20) ? 1 : 0) | ((r.ax & 0x08) ? 2 : 0) | ((r.ax & 0x10) ? 4 : 0);
    int z = (signed char)(u8)r.dx;
    if (mouse_debug) {
      pc_log("vmmouse "); pc_logn((i64)r.ax); pc_log(" "); pc_logn((i64)r.bx); pc_log(" ");
      pc_logn((i64)r.cx); pc_log(" "); pc_logn((i64)r.dx); pc_log("\n");
    }
    if (r.ax & ABS_REL_PACKET) gui_mouse(g, (int)r.bx * 256, -(int)r.cx * 256, b, z);   /* its y is up */
    else {
      int w, h; gui_size(g, &w, &h);
      int x = (int)((uint64_t)(r.bx & 0xffff) * (u32)w / 65536), y = (int)((uint64_t)(r.cx & 0xffff) * (u32)h / 65536);
      gui_mouse_at(g, x, y, b);
      if (z) gui_mouse(g, 0, 0, b, z);
    }
  }
  return 1;
}

/* ---- copy and paste, version 1 ---- */
/* the host's text into a new blob ("" where there is none) */
static blob *clip_host(void) {
  regs r = { 0, 0, CMD_GETSELLENGTH, 0, 0, 0 };
  bdoor(&r);
  i32 n = (i32)r.ax;
  if (n < 0 || n > (1 << 20)) n = 0;
  blob *b = blob_new((u32)n + 4);
  for (i32 k = 0; k < n; k += 4) {
    regs p = { 0, 0, CMD_GETNEXTPIECE, 0, 0, 0 };
    bdoor(&p);
    memcpy(b->bytes + k, &p.ax, 4);
  }
  b->len = (u32)n;
  return b;
}
static void clip_guest(const u8 *s, u32 n) {
  regs r = { 0, n, CMD_SETSELLENGTH, 0, 0, 0 };
  bdoor(&r);
  for (u32 k = 0; k < n; k += 4) {
    u32 v = 0;
    memcpy(&v, s + k, n - k < 4 ? n - k : 4);
    regs p = { 0, v, CMD_SETNEXTPIECE, 0, 0, 0 };
    bdoor(&p);
  }
}

/* ---- GuestRPC channels ---- */
typedef struct { int open; u32 proto, id, ckhi, cklo; } chan;
static chan rpci, tclo;
static u8 *rbuf; static u32 rcap;

static int ch_open(chan *c, u32 proto) {
  u32 flags = FLAG_COOKIE;
  for (;;) {
    regs r = { 0, proto | flags, CMD_MESSAGE | MSG_OPEN << 16, 0, 0, 0 };
    bdoor(&r);
    if ((r.cx >> 16) & ST_SUCCESS) { c->id = r.dx >> 16; c->ckhi = r.si; c->cklo = r.di; c->open = 1; c->proto = proto; return 0; }
    if (!flags) return -1;
    flags = 0;                                 /* no cookies: an old VMware */
  }
}
static void ch_close(chan *c) {
  if (!c->open) return;
  regs r = { 0, 0, CMD_MESSAGE | MSG_CLOSE << 16, c->id << 16, c->ckhi, c->cklo };
  bdoor(&r);
  c->open = 0;
}
static int ch_send(chan *c, const u8 *p, u32 n) {
  for (int tries = 0; tries < 4; tries++) {
    regs r = { 0, n, CMD_MESSAGE | MSG_SENDSIZE << 16, c->id << 16, c->ckhi, c->cklo };
    bdoor(&r);
    u32 st = r.cx >> 16;
    if (!(st & ST_SUCCESS)) return -1;
    if ((st & ST_HB) && use_hb) {
      if (!n) return 0;
      u32 hs = hb_out(c->id, c->ckhi, c->cklo, p, n);
      if (hs & ST_SUCCESS) return 0;
      if (hs & ST_CPT) continue;               /* a checkpoint: again */
      return -1;
    }
    u32 k = 0;
    int again = 0;
    while (k < n) {
      u32 v = 0;
      memcpy(&v, p + k, n - k < 4 ? n - k : 4);
      regs q = { 0, v, CMD_MESSAGE | MSG_SENDPAYLOAD << 16, c->id << 16, c->ckhi, c->cklo };
      bdoor(&q);
      u32 qs = q.cx >> 16;
      if (!(qs & ST_SUCCESS)) { if (qs & ST_CPT) { again = 1; break; } return -1; }
      k += 4;
    }
    if (!again) return 0;
  }
  return -1;
}
/* a message from VMware into rbuf: its length (0: none), or -1 */
static i32 ch_recv(chan *c) {
  for (int tries = 0; tries < 4; tries++) {
    regs r = { 0, 0, CMD_MESSAGE | MSG_RECVSIZE << 16, c->id << 16, c->ckhi, c->cklo };
    bdoor(&r);
    u32 st = r.cx >> 16;
    if (!(st & ST_SUCCESS)) return -1;
    if (!(st & ST_DORECV)) return 0;
    if ((r.dx >> 16) != MSG_SENDSIZE) return -1;
    u32 n = r.bx;
    if (n > (1u << 20)) return -1;
    if (n + 5 > rcap) { rcap = n + 5; rbuf = xrealloc(rbuf, rcap); }
    int again = 0;
    if ((st & ST_HB) && use_hb) {
      if (n) {
        u32 hs = hb_in(c->id, c->ckhi, c->cklo, rbuf, n);
        if (!(hs & ST_SUCCESS)) { if (hs & ST_CPT) continue; return -1; }
      }
    } else {
      for (u32 k = 0; k < n && !again; k += 4) {
        regs q = { 0, ST_SUCCESS, CMD_MESSAGE | MSG_RECVPAYLOAD << 16, c->id << 16, c->ckhi, c->cklo };
        bdoor(&q);
        u32 qs = q.cx >> 16;
        if (!(qs & ST_SUCCESS)) { if (qs & ST_CPT) { again = 1; break; } return -1; }
        if ((q.dx >> 16) != MSG_SENDPAYLOAD) return -1;
        memcpy(rbuf + k, &q.bx, n - k < 4 ? n - k : 4);
      }
      if (again) continue;
    }
    rbuf[n] = 0;
    regs s = { 0, ST_SUCCESS, CMD_MESSAGE | MSG_RECVSTATUS << 16, c->id << 16, c->ckhi, c->cklo };
    bdoor(&s);
    u32 ss = s.cx >> 16;
    if (!(ss & ST_SUCCESS)) { if (ss & ST_CPT) continue; return -1; }
    return (i32)n;
  }
  return -1;
}
/* an RPCI request, N bytes at P: VMware's answer after its "1 " (OK) or
 * "0 " in rbuf + 2, its length; -1 when the channel failed, -2 for "0 " */
static i32 rpci_call(const u8 *p, u32 n) {
  for (int tries = 0; tries < 2; tries++) {
    if (!rpci.open && ch_open(&rpci, PROTO_RPCI)) return -1;
    i32 got;
    if (ch_send(&rpci, p, n) == 0 && (got = ch_recv(&rpci)) >= 2) {
      if (rbuf[0] == '1' && rbuf[1] == ' ') return got - 2;
      if (rbuf[0] == '0' && rbuf[1] == ' ') return -2;
      return -1;
    }
    ch_close(&rpci);                           /* again, on a fresh channel */
  }
  return -1;
}
static i32 rpci_str(const char *s) { return rpci_call((const u8 *)s, (u32)strlen(s)); }

/* ---- TCLO: VMware's requests, answered ----
 * As vmtoolsd does (rpcin.c): send the last answer (at first, none), then
 * take the next request, a poll at a time (pc.c, vmw_poll). */
static char tclo_reply[64];
static u32 tclo_rlen;
static int tclo_started, halt_asked, tclo_fails;
static int starts(const u8 *s, i32 n, const char *w) { i32 k = (i32)strlen(w); return n >= k && !memcmp(s, w, (size_t)k); }
static void tclo_answer(const char *s) { tclo_rlen = (u32)strlen(s); memcpy(tclo_reply, s, tclo_rlen); }
static void tclo_poll(void) {
  if (!tclo.open) {
    if (tclo_fails > 20) return;                /* no GuestRPC here (QEMU's vmport) */
    if (ch_open(&tclo, PROTO_TCLO)) { tclo_fails++; return; }
    tclo_fails = 0;
    tclo_rlen = 0;
    if (!tclo_started) {
      /* a version of Tools that VMware does not manage (open-vm-tools' own number) */
      rpci_str("tools.set.version 2147483647");
      tclo_started = 1;
    }
  }
  for (int k = 0; k < 8; k++) {
    if (ch_send(&tclo, (const u8 *)tclo_reply, tclo_rlen)) { ch_close(&tclo); return; }
    tclo_rlen = 0;
    i32 n = ch_recv(&tclo);
    if (n < 0) { ch_close(&tclo); return; }
    if (n == 0) return;                        /* nothing asked: the empty answer next time */
    const u8 *q = rbuf;
    if (starts(q, n, "reset")) tclo_answer("OK ATR toolbox");
    else if (starts(q, n, "ping")) tclo_answer("OK ");
    else if (starts(q, n, "OS_Halt") || starts(q, n, "OS_PowerOff")) { halt_asked = 1; tclo_answer("OK "); }
    else if (starts(q, n, "OS_Reboot")) { halt_asked = 2; tclo_answer("OK "); }
    else if (starts(q, n, "Capabilities_Register") || starts(q, n, "Set_Option") || starts(q, n, "Time_Synchronize") ||
             starts(q, n, "vmsvc.") || starts(q, n, "OS_"))
      tclo_answer("OK ");
    else tclo_answer("ERROR Unknown Command");
  }
}

/* ---- shared folders (HGFS version 1) ---- */
enum { HG_OPEN, HG_READ, HG_WRITE, HG_CLOSE, HG_SEARCH_OPEN, HG_SEARCH_READ, HG_SEARCH_CLOSE, HG_GETATTR,
       HG_SETATTR, HG_CREATE_DIR, HG_DELETE_FILE, HG_DELETE_DIR };
enum { HGFS_MAX = 6144, HGFS_IO = 4096 };
static u8 pkt[2 + HGFS_MAX];                    /* "f " and the packet */
static u32 hgfs_id;
static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32 get32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
/* the header of a request of OP: the packet's next free byte */
static u8 *req(u32 op) { pkt[0] = 'f'; pkt[1] = ' '; put32(pkt + 2, ++hgfs_id); put32(pkt + 6, op); return pkt + 10; }
/* a path ("share/dir/file") as HGFS's name: its parts with NULs between; the end */
static u8 *name(u8 *p, const char *path) {
  u8 *len = p; p += 4;
  u32 n = 0;
  while (*path == '/') path++;
  for (; *path && p + 1 < pkt + sizeof pkt; path++) {
    if (*path == '/') { while (path[1] == '/') path++; if (!path[1]) break; *p++ = 0; }
    else *p++ = (u8)*path;
    n++;
  }
  put32(len, n);
  return p;
}
/* the request to P sent: the reply's status (0 success), its body at *body; -1 */
static i32 hgfs_do(u8 *end, const u8 **body, i32 *blen) {
  i32 n = rpci_call(pkt, (u32)(end - pkt));
  if (n < 8) return -1;
  if (get32(rbuf + 2) != hgfs_id) return -1;
  if (body) { *body = rbuf + 2 + 8; *blen = n - 8; }
  return (i32)get32(rbuf + 2 + 4);
}

/* where a file is read from: its path, kept a while (the drive's hooks'
 * WHERE is its index here) */
enum { PATHS = 64 };
static char *paths[PATHS]; static u32 next_path;
static u32 open_where = 0xffffffffu, open_handle;

static int hgfs_getattr(const char *path, u32 *type, uint64_t *size) {
  const u8 *b; i32 bl;
  u8 *p = name(req(HG_GETATTR), path);
  if (hgfs_do(p, &b, &bl) != 0 || bl < 45) return -1;
  *type = get32(b);
  *size = get32(b + 4) | (uint64_t)get32(b + 8) << 32;
  return 0;
}
static void hgfs_close_open(void) {
  if (open_where == 0xffffffffu) return;
  u8 *p = req(HG_CLOSE); put32(p, open_handle); p += 4;
  hgfs_do(p, 0, 0);
  open_where = 0xffffffffu;
}

/* the drive's listing of DIR ("hgfs/share/dir"), as C4IX's .c4ix-dir:
 * "SIZE NAME" or "d NAME" a line */
static blob *hgfs_list(const char *dir) {
  const u8 *b; i32 bl;
  u8 *p = name(req(HG_SEARCH_OPEN), dir);
  if (hgfs_do(p, &b, &bl) != 0 || bl < 4) return 0;
  u32 search = get32(b);
  obuf o = {0, 0, 0};
  for (u32 off = 0; off < 100000; off++) {
    p = req(HG_SEARCH_READ); put32(p, search); put32(p + 4, off); p += 8;
    if (hgfs_do(p, &b, &bl) != 0 || bl < 49) break;
    u32 type = get32(b), nl = get32(b + 45);
    if (!nl || bl < 49 + (i32)nl) break;
    const char *nm = (const char *)b + 49;
    if ((nl == 1 && nm[0] == '.') || (nl == 2 && nm[0] == '.' && nm[1] == '.')) continue;
    if (type == 1) ob_put(&o, 'd');
    else { char t[24]; int k = 0; uint64_t v = get32(b + 4) | (uint64_t)get32(b + 8) << 32;
           if (v > 0xffffffffu) v = 0xffffffffu;
           do t[k++] = (char)('0' + v % 10); while (v /= 10); while (k) ob_put(&o, t[--k]); }
    ob_put(&o, ' ');
    for (u32 i = 0; i < nl; i++) ob_put(&o, nm[i]);
    ob_put(&o, '\n');
  }
  p = req(HG_SEARCH_CLOSE); put32(p, search); p += 4;
  hgfs_do(p, 0, 0);
  if (!o.b) ob_puts(&o, "");
  blob *r = blob_new(o.n);
  memcpy(r->bytes, o.b, o.n);
  xfree(o.b);
  return r;
}

/* ---- the drive ---- */
static const char *under_hgfs(const char *name) {
  while (*name == '/') name++;
  if (!strncmp(name, "hgfs", 4) && (name[4] == '/' || !name[4])) return name + 4;
  return 0;
}
static blob *vfetch(drive *d, const char *name) {
  (void)d;
  size_t n = strlen(name);
  obuf o = {0, 0, 0};
  if (!strcmp(name, ".c4pc-drive")) {
    /* the shared folders are hgfs/ (VMware's, where they are turned on) */
    ob_puts(&o, "kind vmware\nlabel VMware\nsize 0\nused 0\nfree 0\n");
  } else if (!strcmp(name, "clipboard")) return clip_host();
  else if (n >= 9 && !strcmp(name + n - 9, ".c4ix-dir")) {
    char dir[1024];
    if (n - 9 >= sizeof dir) return 0;
    memcpy(dir, name, n - 9); dir[n - 9] = 0;
    while (n > 9 && dir[strlen(dir) - 1] == '/') dir[strlen(dir) - 1] = 0;
    const char *h = under_hgfs(dir);
    if (h) return hgfs_list(h);
    /* the drive's top: hgfs/, and the clipboard */
    const char *t = dir; while (*t == '/') t++;
    if (*t) return 0;
    ob_puts(&o, "d hgfs\n0 clipboard\n");
  } else return 0;
  blob *b = blob_new(o.n);
  memcpy(b->bytes, o.b, o.n);
  xfree(o.b);
  return b;
}
static int vfind(drive *d, const char *name, u32 *size, u32 *where) {
  (void)d;
  const char *h = under_hgfs(name);
  u32 type; uint64_t sz;
  if (!h || hgfs_getattr(h, &type, &sz) || type == 1) return -1;
  u32 w = next_path++ % PATHS;
  if (open_where == w) hgfs_close_open();
  xfree(paths[w]);
  paths[w] = xmalloc(strlen(h) + 1);
  memcpy(paths[w], h, strlen(h) + 1);
  *size = sz > 0xffffffffu ? 0xffffffffu : (u32)sz;
  *where = w;
  return 0;
}
static u32 vpread(drive *d, u32 where, u32 pos, u8 *buf, u32 n) {
  (void)d;
  if (where >= PATHS || !paths[where]) return 0;
  if (open_where != where) {
    hgfs_close_open();
    const u8 *b; i32 bl;
    u8 *p = req(HG_OPEN); put32(p, 0); put32(p + 4, 0); p[8] = 0; p = name(p + 9, paths[where]);
    if (hgfs_do(p, &b, &bl) != 0 || bl < 4) return 0;
    open_handle = get32(b); open_where = where;
  }
  u32 done = 0;
  while (done < n) {
    u32 k = n - done < HGFS_IO ? n - done : HGFS_IO;
    const u8 *b; i32 bl;
    u8 *p = req(HG_READ); put32(p, open_handle); put32(p + 4, pos + done); put32(p + 8, 0); put32(p + 12, k); p += 16;
    if (hgfs_do(p, &b, &bl) != 0 || bl < 4) break;
    u32 got = get32(b);
    if (got > k || (i32)(4 + got) > bl) break;
    memcpy(buf + done, b + 4, got);
    done += got;
    if (got < k) break;
  }
  return done;
}
/* a file written to the drive and closed: the clipboard's, or the host's
 * file written whole (made, or emptied first) */
static void vsink(drive *d, const char *nm, blob *b) {
  const char *h = under_hgfs(nm);
  if (!strcmp(nm, "clipboard.set")) clip_guest(b->bytes, b->len);
  else if (h) {
    const u8 *r; i32 rl;
    hgfs_close_open();
    u8 *p = req(HG_OPEN); put32(p, 1); put32(p + 4, 4); p[8] = 6; p = name(p + 9, h);   /* write only, create or empty, rw- */
    if (hgfs_do(p, &r, &rl) == 0 && rl >= 4) {
      u32 fh = get32(r);
      for (u32 off = 0; off < b->len; ) {
        u32 k = b->len - off < HGFS_IO ? b->len - off : HGFS_IO;
        p = req(HG_WRITE); put32(p, fh); p[4] = 0; put32(p + 5, off); put32(p + 9, 0); put32(p + 13, k);
        memcpy(p + 17, b->bytes + off, k);
        if (hgfs_do(p + 17 + k, &r, &rl) != 0 || rl < 4 || !get32(r)) break;
        off += get32(r);
      }
      p = req(HG_CLOSE); put32(p, fh); p += 4;
      hgfs_do(p, 0, 0);
    }
  }
  /* the bytes are the host's now, not the drive's */
  blob *old = smap_del(d->files, nm);
  if (old) blob_unref(old);
}
static void vremove(drive *d, const char *nm) {
  (void)d;
  const char *h = under_hgfs(nm);
  if (!h) return;
  hgfs_close_open();
  u8 *p = name(req(HG_DELETE_FILE), h);
  hgfs_do(p, 0, 0);
}

drive *vmw_drive(void) {
  if (!present) return 0;
  drive *d = drive_new(1);
  d->kind = "vmware";
  d->fetch = vfetch; d->find = vfind; d->pread = vpread;
  d->sink = vsink; d->remove = vremove;
  return d;
}

/* ---- the run loop's turn (pc.c), every 100 ms or so: TCLO's requests.
 * 1 when VMware asked the guest to halt, 2 to restart (taken once) */
int vmw_poll(void) {
  if (!present) return 0;
  tclo_poll();
  int h = halt_asked; halt_asked = 0;
  return h;
}
