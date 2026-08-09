#include "eth.h"
#include "mem.h"
#include "cpu.h"

// See eth.h. Register offsets and semantics are ethmac.js's exactly.
// net_tx is net.c's frame sink; declared here (c4lc -c makes an
// undefined prototype an extern for c4rlink, same as jit.c does for
// dtlb_lookup).
void net_tx(char *frame, int len);

enum {
    ETH_MODER      = 0x00, ETH_INT_SOURCE = 0x04, ETH_INT_MASK = 0x08,
    ETH_IPGT       = 0x0C, ETH_IPGR1      = 0x10, ETH_IPGR2    = 0x14,
    ETH_PACKETLEN  = 0x18, ETH_COLLCONF   = 0x1C, ETH_TX_BD_NUM= 0x20,
    ETH_CTRLMODER  = 0x24, ETH_MIIMODER   = 0x28, ETH_MIICOMMAND=0x2C,
    ETH_MIIADDRESS = 0x30, ETH_MIITX_DATA = 0x34, ETH_MIIRX_DATA=0x38,
    ETH_MIISTATUS  = 0x3C, ETH_MAC_ADDR0  = 0x40, ETH_MAC_ADDR1= 0x44,
    ETH_HASH0      = 0x48, ETH_HASH1      = 0x4C, ETH_TXCTRL   = 0x50
};
enum { ETH_BD_START = 0x400, ETH_BD_END = 0x7FF };
enum { ETH_INTNO = 4 };
enum { RXBUF_MAX = 2048 };

int e_MODER, e_INT_SOURCE, e_INT_MASK, e_IPGT, e_IPGR1, e_IPGR2;
int e_PACKETLEN, e_COLLCONF, e_TX_BD_NUM, e_CTRLMODER, e_MIIMODER;
int e_MIICOMMAND, e_MIIADDRESS, e_MIITX_DATA, e_MIIRX_DATA, e_MIISTATUS;
int e_MAC_ADDR0, e_MAC_ADDR1, e_HASH0, e_HASH1, e_TXCTRL;
int e_currRX;
int e_bd[256];        // 128 64-bit descriptors: [i]=status, [i+1]=pointer
int e_mii[16];
int eth_mac[6];
char e_txframe[RXBUF_MAX];

// MII basic-mode status/id, enough for the driver to see "link up,
// autoneg complete, 10/100 available" and stop polling (ethmac.js's
// MIIregs init).
enum {
    MII_BMCR = 0x00, MII_BMSR = 0x01, MII_PHYSID1 = 0x02,
    MII_PHYSID2 = 0x03, MII_ADVERTISE = 0x04
};

// The IRQ line is a pure function of (INT_MASK & INT_SOURCE),
// reconciled once per main-loop poll -- NEVER from inside an MMIO
// access (see eth.h / net.h for why a mid-store interrupt corrupts
// pc/nextpc). Idempotent: cpu_check_for_interrupt is a no-op while
// SR_IEE is off (i.e. while an ISR runs), so re-asserting each poll
// until the guest acks INT_SOURCE never double-delivers.
void eth_poll() {
    if (e_INT_MASK & e_INT_SOURCE) cpu_raise_interrupt(ETH_INTNO);
    else cpu_clear_interrupt(ETH_INTNO);
}

void eth_reset() {
    int i;
    e_MODER = 0xA000; e_INT_SOURCE = 0; e_INT_MASK = 0;
    e_IPGT = 0x12; e_IPGR1 = 0xC; e_IPGR2 = 0x12;
    e_PACKETLEN = 0x400600; e_COLLCONF = 0xF003F; e_TX_BD_NUM = 0x40;
    e_CTRLMODER = 0; e_MIIMODER = 0x64; e_MIICOMMAND = 0;
    e_MIIADDRESS = 0; e_MIITX_DATA = 0; e_MIIRX_DATA = 0x22; e_MIISTATUS = 0;
    e_HASH0 = 0; e_HASH1 = 0; e_TXCTRL = 0;

    // A fixed, locally-administered unicast MAC 02:00:00:00:00:02
    // (jor1k randomizes; a constant keeps boots reproducible for the
    // byte-diff verification methodology). MAC_ADDR1 = high 2 bytes,
    // MAC_ADDR0 = low 4 bytes, matching the driver's read order.
    e_MAC_ADDR1 = 0x0200;
    e_MAC_ADDR0 = 0x00000002;
    eth_mac[0] = 0x02; eth_mac[1] = 0x00; eth_mac[2] = 0x00;
    eth_mac[3] = 0x00; eth_mac[4] = 0x00; eth_mac[5] = 0x02;

    for (i = 0; i < 256; ++i) e_bd[i] = 0;
    for (i = 0; i < 16; ++i) e_mii[i] = 0;
    e_mii[MII_BMCR] = 0x0100;                                    // full duplex
    e_mii[MII_BMSR] = 0x4 | 0x20 | 0x800 | 0x1000 | 0x2000 | 0x4000; // link ok, 10/100
    e_mii[MII_PHYSID1] = 0x2000; e_mii[MII_PHYSID2] = 0x5c90;
    e_mii[MII_ADVERTISE] = 0x01e1;
    e_currRX = e_TX_BD_NUM << 1;
}

// deliver a received frame (no FCS) into the current RX descriptor,
// faithful to ethmac.js Receive (minus the promiscuous/multicast/
// error paths a synthesized-reply backend never exercises: every
// frame net.c produces is addressed to eth_mac, so `match` is always
// true and RXEN is the only gate).
void eth_rx(char *frame, int len) {
    int i, ptr, j, want_irq;
    if ((e_MODER & 0x1) == 0) return;              // RXEN off
    i = e_currRX;
    if (e_bd[i] & (1 << 15)) {                     // descriptor empty/ready
        want_irq = e_bd[i] & (1 << 14);            // IRQ bit, from the ORIGINAL descriptor
        if (len > (e_PACKETLEN & 0xFFFF)) len = e_PACKETLEN & 0xFFFF;
        ptr = e_bd[i + 1];
        j = 0;
        while (j < len) { ram_sb(ptr + j, frame[j] & 0xFF); ++j; }
        // status: LEN = len+4 (CRC accounted), clear E(bit15), keep
        // WR(bit13)/IRQ(bit14) the driver set, clear error bits.
        e_bd[i] = ((len + 4) << 16) | (e_bd[i] & ((1 << 14) | (1 << 13)));
        if (want_irq) e_INT_SOURCE = e_INT_SOURCE | (1 << 2);  // RXB; eth_poll asserts the line
    } else {
        e_INT_SOURCE = e_INT_SOURCE | (1 << 4);               // BUSY
    }
    // advance RX cursor: wrap on WR(bit13) or array bound
    if ((e_bd[e_currRX] & (1 << 13)) || (e_currRX + 2) >= 256)
        e_currRX = e_TX_BD_NUM << 1;
    else
        e_currRX = e_currRX + 2;
}

void eth_transmit(int bd_num) {
    int stat, ptr, len, i;
    if ((e_MODER & (1 << 1)) == 0) return;         // TXEN off
    stat = e_bd[bd_num << 1];
    ptr = e_bd[(bd_num << 1) + 1];
    if ((stat & (1 << 15)) == 0) return;           // RD (ready) not set
    len = (stat >> 16) & 0xFFFF;
    if (len > RXBUF_MAX) len = RXBUF_MAX;
    i = 0;
    while (i < len) { e_txframe[i] = ram_lb(ptr + i) & 0xFF; ++i; }
    net_tx(e_txframe, len);                         // hand off; may inject a reply
    e_bd[bd_num << 1] = stat & ~(1 << 15);          // clear RD
    e_INT_SOURCE = e_INT_SOURCE | 1;                // TXB; eth_poll asserts the line
}

void eth_mii_command() {
    int fiad, rgad;
    fiad = e_MIIADDRESS & 0x1F;
    rgad = (e_MIIADDRESS >> 8) & 0x1F;
    if (e_MIICOMMAND == 2) {                        // read status
        if (fiad != 0) e_MIIRX_DATA = 0xFFFF;       // only PHY addr 0 exists
        else e_MIIRX_DATA = e_mii[rgad] & 0xFFFF;
    }
    // command 4 (write) is a no-op here, as in ethmac.js
}

int eth_read32(int addr) {
    switch (addr) {
    case ETH_MODER:      return e_MODER;
    case ETH_INT_SOURCE: return e_INT_SOURCE;
    case ETH_INT_MASK:   return e_INT_MASK;
    case ETH_IPGT:       return e_IPGT;
    case ETH_IPGR1:      return e_IPGR1;
    case ETH_IPGR2:      return e_IPGR2;
    case ETH_PACKETLEN:  return e_PACKETLEN;
    case ETH_COLLCONF:   return e_COLLCONF;
    case ETH_TX_BD_NUM:  return e_TX_BD_NUM;
    case ETH_CTRLMODER:  return e_CTRLMODER;
    case ETH_MIIMODER:   return e_MIIMODER;
    case ETH_MIICOMMAND: return e_MIICOMMAND;
    case ETH_MIIADDRESS: return e_MIIADDRESS;
    case ETH_MIITX_DATA: return e_MIITX_DATA;
    case ETH_MIIRX_DATA: return e_MIIRX_DATA;
    case ETH_MIISTATUS:  return e_MIISTATUS;
    case ETH_MAC_ADDR0:  return e_MAC_ADDR0;
    case ETH_MAC_ADDR1:  return e_MAC_ADDR1;
    case ETH_HASH0:      return e_HASH0;
    case ETH_HASH1:      return e_HASH1;
    case ETH_TXCTRL:     return e_TXCTRL;
    }
    if (addr >= ETH_BD_START && addr <= ETH_BD_END)
        return e_bd[(addr - ETH_BD_START) >> 2];
    return 0;
}

void eth_write32(int addr, int val) {
    int bd_num;
    switch (addr) {
    case ETH_MODER:      e_MODER = val; return;
    case ETH_INT_SOURCE: e_INT_SOURCE = e_INT_SOURCE & ~val; return;
    case ETH_INT_MASK:   e_INT_MASK = val; return;
    case ETH_IPGT:       e_IPGT = val; return;
    case ETH_IPGR1:      e_IPGR1 = val; return;
    case ETH_IPGR2:      e_IPGR2 = val; return;
    case ETH_PACKETLEN:  e_PACKETLEN = val; return;
    case ETH_COLLCONF:   e_COLLCONF = val; return;
    case ETH_TX_BD_NUM:  e_TX_BD_NUM = val; e_currRX = val << 1; return;
    case ETH_CTRLMODER:  e_CTRLMODER = val; return;
    case ETH_MIIMODER:   e_MIIMODER = val; return;
    case ETH_MIICOMMAND: e_MIICOMMAND = val; eth_mii_command(); return;
    case ETH_MIIADDRESS: e_MIIADDRESS = val; return;
    case ETH_MIITX_DATA: e_MIITX_DATA = val; return;
    case ETH_MIIRX_DATA: e_MIIRX_DATA = val; return;
    case ETH_MIISTATUS:  e_MIISTATUS = val; return;
    case ETH_MAC_ADDR0:  e_MAC_ADDR0 = val; return;
    case ETH_MAC_ADDR1:  e_MAC_ADDR1 = val; return;
    case ETH_HASH0:      e_HASH0 = val; return;
    case ETH_HASH1:      e_HASH1 = val; return;
    case ETH_TXCTRL:     e_TXCTRL = val; return;
    }
    if (addr >= ETH_BD_START && addr <= ETH_BD_END) {
        e_bd[(addr - ETH_BD_START) >> 2] = val;
        bd_num = (addr - ETH_BD_START) >> 3;
        // writing the STATUS word (not the pointer word) of a TX
        // descriptor, with the ready bit set, triggers transmit
        if (((bd_num << 3) + ETH_BD_START) == addr) {
            if ((val & (1 << 15)) != 0 && bd_num < e_TX_BD_NUM)
                eth_transmit(bd_num);
        }
    }
}
