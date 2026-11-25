// c4or1k ethernet -- OpenCores ethmac controller (M15), ported from
// jor1k/js/worker/dev/ethmac.js op-for-op. MMIO base 0x92000000
// (top byte 0x92, routed from mmio.c), IRQ line 4. This is the device
// the guest kernel's `ethoc` driver already probes for every boot --
// the boot log's `libphy: ethoc-mdio: probed` and the failed
// `mmio_read32 ... 0x92000040` (a MAC_ADDR0 read) were it looking for
// exactly this.
//
// TX: the driver arms a transmit buffer descriptor (sets the READY
// bit); eth_write32 reads the frame out of guest RAM and hands it to
// net_tx (net.c). RX: net.c calls eth_rx with a frame; it lands in
// the next empty receive BD in guest RAM and raises IRQ 4. No host
// networking is involved -- net.c synthesizes replies (ARP/DHCP/ICMP)
// in pure computation, so this works identically in the native and
// hosted builds. Real external connectivity is a separate step that
// would need a host socket primitive (see docs' M15 section).

void eth_reset();
int  eth_read32(int addr);         // addr already masked to the 0x1000 window
void eth_write32(int addr, int val);

// net.c -> device: deliver a received frame (no FCS) into the RX ring.
// MUST be called only from the main-loop poll path (net_poll), never
// synchronously from an MMIO store: it sets the RX interrupt-source
// bit, and eth_poll() then asserts the IRQ line -- doing that from
// inside a guest store would corrupt the pc/nextpc that store is about
// to advance (see net.h).
void eth_rx(char *frame, int len);

// Reconcile the IRQ line from (INT_MASK & INT_SOURCE). Called from the
// main loop (net_poll), which is the sanctioned place to raise/clear
// interrupts -- the same place con_poll/cpu_tick_check do. The device
// itself NEVER touches the IRQ line during an MMIO access; it only
// updates INT_SOURCE, and this reconciles the level here.
void eth_poll();

// the guest's own MAC, as the six bytes net.c must address replies to.
extern int eth_mac[6];
