/* net.c -- c4net (docs/c4pc-network.md): a network card for a c4
 * machine, and the switch the hypervisor's machines are plugged into.
 *
 * The card is a window of registers in the machine's page 0 (NET_BASE),
 * the kernel's alone (the MMU's IOPM gives user mode none of it, as the
 * disks' BLK window): Ethernet frames, copied a command at a time between
 * the machine's memory (physical) and the card, as a disk's sectors are.
 *
 *   0x580 STATUS  r: 1 a port (the link), 2 frames waiting
 *   0x584 MAC0    r: the MAC's bytes 0-3, the first in the low byte
 *   0x588 MAC1    r: its bytes 4-5
 *   0x58c IP      r: the address the switch suggests, 0 none
 *   0x590 ADDR    w: where in memory (physical) the next command reads or writes
 *   0x594 LEN     w: how many bytes
 *   0x598 CMD     w: 1 send the frame at ADDR, LEN bytes; 2 the next
 *                    frame received into ADDR, LEN bytes at most; 3 the
 *                    address of the machine named at ADDR (a C string:
 *                    its name, vmN or control); r: the result -- bytes
 *                    sent or received (0: none waiting), the address (0:
 *                    no such machine), or -1
 *   0x59c RXQ     r: frames waiting
 *
 * The switch: a port a machine, by its hypervisor id (VM 0 the control,
 * pc/hv.c), with its MAC 52:54:00:c4:00:ID and address 10.4.0.ID+1/24. A
 * frame goes to the port whose MAC it names, or to every other port when
 * it is a broadcast or multicast; each port keeps 64 frames for its
 * machine, and drops (counted) when they are not taken. Delivery wakes the
 * machine if it naps (net_kick_fn: hv.c ends a guest's nap; pc.c's nap
 * loop for VM 0 looks at net_waiting). The machines run on processors of
 * their own: the switch has its own lock, never held while anything else
 * is taken.
 */
#include "c4pc.h"
#include "clib.h"

enum { NQ = 64, FMAX = 1536, NPORT = 16 };
enum { R_STATUS = 0x580, R_MAC0 = 0x584, R_MAC1 = 0x588, R_IP = 0x58c, R_ADDR = 0x590, R_LEN = 0x594, R_CMD = 0x598, R_RXQ = 0x59c };
enum { C_SEND = 1, C_RECV = 2, C_RESOLVE = 3 };

struct netport {
  int used, id; char name[32];
  u8 mac[6]; u32 ip;
  machine *m;                          /* its machine, 0 while it has none (halted) */
  u8 *q; u16 qlen[NQ]; u32 head, tail; /* frames for it: written at head, taken at tail */
  u32 addr, len; i32 result;           /* the window's */
  unsigned long long rx, tx, rxb, txb, drops;
};
static struct netport ports[NPORT];
static volatile int sw_lk;
void (*net_kick_fn)(int id);           /* hv.c: a frame for machine ID, napping perhaps */
/* The uplink (N2): the switch's way out, c4pc-host's --net udp:... (host.c),
 * a frame a datagram. It is given every frame a machine sends that no port
 * here is named by, broadcasts too; net_inject puts one from outside onto
 * the switch, as a machine's would be but for the uplink itself. */
void (*net_uplink_fn)(const u8 *f, u32 len);
unsigned long long net_up_counts[4];   /* frames out, in; bytes out, in */

static void sw_lock(void) { while (__atomic_exchange_n(&sw_lk, 1, __ATOMIC_ACQUIRE)) __builtin_ia32_pause(); }
static void sw_unlock(void) { __atomic_store_n(&sw_lk, 0, __ATOMIC_RELEASE); }

/* machine M plugged into the switch as ID, called NAME: its card */
void net_attach(machine *m, int id, const char *name) {
  struct netport *p = 0;
  sw_lock();
  for (int k = 0; k < NPORT && !p; k++) if (ports[k].used && ports[k].id == id) p = &ports[k];
  for (int k = 0; k < NPORT && !p; k++) if (!ports[k].used) p = &ports[k];
  if (!p) { sw_unlock(); return; }
  if (!p->used) {
    memset(p, 0, sizeof *p);
    p->used = 1; p->id = id;
    p->mac[0] = 0x52; p->mac[1] = 0x54; p->mac[2] = 0x00; p->mac[3] = 0xc4; p->mac[4] = (u8)(id >> 8); p->mac[5] = (u8)id;
    p->ip = 0x0a040001u + (u32)id;
  }
  if (!p->q) p->q = xmalloc((size_t)NQ * FMAX);
  int k = 0;
  while (name && *name && k < (int)sizeof p->name - 1) p->name[k++] = *name++;
  p->name[k] = 0;
  p->head = p->tail = 0;
  p->m = m;
  m->dv->net = p;
  sw_unlock();
}
/* M has stopped: its card out of the switch (its port kept, for its id) */
void net_detach(machine *m) {
  struct netport *p = m->dv->net;
  if (!p) return;
  sw_lock();
  p->m = 0; p->head = p->tail = 0;
  m->dv->net = 0;
  sw_unlock();
}
int net_waiting(machine *m) {
  struct netport *p = m->dv->net;
  return p && p->head != p->tail;
}
/* port P's counts: "rx R tx T rxbytes RB txbytes TB drops D" (INFO, later) */
void net_counts(machine *m, unsigned long long *c) {
  struct netport *p = m->dv->net;
  for (int k = 0; k < 5; k++) c[k] = 0;
  if (!p) return;
  c[0] = p->rx; c[1] = p->tx; c[2] = p->rxb; c[3] = p->txb; c[4] = p->drops;
}

/* a frame from port SRC through the switch (0: from the uplink) */
static void sw_send(struct netport *src, const u8 *f, u32 len) {
  int kick[NPORT], nk = 0, mine = 0;
  int bcast = f[0] & 1;
  sw_lock();
  if (src) { src->tx++; src->txb += len; }
  for (int k = 0; k < NPORT; k++) {
    struct netport *p = &ports[k];
    if (!p->used || !p->m || p == src) continue;
    if (!bcast && memcmp(f, p->mac, 6)) continue;
    mine = 1;
    if (p->head - p->tail >= NQ) { p->drops++; continue; }
    u32 at = p->head % NQ;
    memcpy(p->q + (size_t)at * FMAX, f, len);
    p->qlen[at] = (u16)len;
    __atomic_store_n(&p->head, p->head + 1, __ATOMIC_RELEASE);
    p->rx++; p->rxb += len;
    kick[nk++] = p->id;
  }
  int up = src && net_uplink_fn && (bcast || !mine);
  if (up) { net_up_counts[0]++; net_up_counts[2] += len; }
  else if (!src) { net_up_counts[1]++; net_up_counts[3] += len; }
  sw_unlock();
  if (net_kick_fn) for (int k = 0; k < nk; k++) net_kick_fn(kick[k]);
  if (up) net_uplink_fn(f, len);
}

/* a frame from outside (the uplink) onto the switch */
void net_inject(const u8 *f, u32 len) {
  if (len >= 14 && len <= FMAX) sw_send(0, f, len);
}

static u32 resolve(const char *s) {
  u32 ip = 0;
  sw_lock();
  if (s[0] == 'v' && s[1] == 'm' && s[2] >= '0' && s[2] <= '9') {
    int id = 0;
    const char *q = s + 2;
    while (*q >= '0' && *q <= '9') id = id * 10 + (*q++ - '0');
    if (!*q) for (int k = 0; k < NPORT; k++) if (ports[k].used && ports[k].id == id) ip = ports[k].ip;
    /* beyond the uplink, a machine N is 10.4.0.N+1 all the same */
    if (!*q && !ip && net_uplink_fn && id < 254) ip = 0x0a040001u + (u32)id;
  }
  for (int k = 0; k < NPORT && !ip; k++) {
    if (!ports[k].used) continue;
    const char *a = ports[k].name, *b = s;
    while (*a && *a == *b) { a++; b++; }
    if (!*a && !*b) ip = ports[k].ip;
  }
  sw_unlock();
  return ip;
}

i32 net_rd(machine *m, u32 a) {
  struct netport *p = m->dv->net;
  if (!p) return 0;
  switch (a) {
    case R_STATUS: return 1 | (p->head != p->tail ? 2 : 0);
    case R_MAC0: return (i32)((u32)p->mac[0] | (u32)p->mac[1] << 8 | (u32)p->mac[2] << 16 | (u32)p->mac[3] << 24);
    case R_MAC1: return (i32)((u32)p->mac[4] | (u32)p->mac[5] << 8);
    case R_IP: return (i32)p->ip;
    case R_ADDR: return (i32)p->addr;
    case R_LEN: return (i32)p->len;
    case R_CMD: return p->result;
    case R_RXQ: return (i32)(__atomic_load_n(&p->head, __ATOMIC_ACQUIRE) - p->tail);
  }
  return 0;
}
void net_wr(machine *m, u32 a, i32 v) {
  struct netport *p = m->dv->net;
  if (!p) return;
  switch (a) {
    case R_ADDR: p->addr = (u32)v; return;
    case R_LEN: p->len = (u32)v; return;
    case R_CMD: break;
    default: return;
  }
  p->result = -1;
  if (v == C_SEND) {
    u8 f[FMAX];
    if (p->len < 14 || p->len > FMAX) return;
    if (dev_dma_read(m, p->addr, p->len, f) != p->len) return;
    sw_send(p, f, p->len);
    p->result = (i32)p->len;
  } else if (v == C_RECV) {
    if (__atomic_load_n(&p->head, __ATOMIC_ACQUIRE) == p->tail) { p->result = 0; return; }
    u32 at = p->tail % NQ, n = p->qlen[at];
    if (n > p->len) n = p->len;
    if (dev_dma_write(m, p->addr, p->q + (size_t)at * FMAX, n) != n) return;
    __atomic_store_n(&p->tail, p->tail + 1, __ATOMIC_RELEASE);
    p->result = (i32)p->qlen[at];
  } else if (v == C_RESOLVE) {
    char s[40];
    u32 k = dma_cstring(m, p->addr, s, sizeof s - 1);
    s[k < sizeof s ? k : sizeof s - 1] = 0;
    p->result = (i32)resolve(s);
  }
}
