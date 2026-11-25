// net.h -- the c4or1k network backend contract (M15).
//
// eth.c is a faithful ethmac controller but has nothing to talk to.
// net.c is the thing on the other end of the wire. It is deliberately
// NOT a host socket: it synthesizes the replies a minimal peer would
// send -- ARP, ICMP echo, and DHCP -- in pure computation, so the
// entire path (guest driver -> BD ring -> device -> backend -> device
// -> BD ring -> guest driver -> kernel IP stack) is exercised end to
// end with zero host networking and zero new VM primitives. That is
// the point: it proves the emulator can carry a network, in the
// native build (fast) AND the hosted builds (c4m/c4mp), before any
// real-connectivity primitive is added to c4mp.
//
// Reentrancy: net_tx is called from deep inside a guest MMIO store
// (eth_transmit); it must NOT inject into the RX ring synchronously,
// because eth_rx raises an interrupt and a mid-instruction IRQ would
// corrupt the pc/nextpc the store is about to advance. So net_tx only
// ENQUEUES replies; net_poll (called from main.c's loop, between
// instructions, on the same cadence as con_poll/tick) drains the
// queue into eth_rx. Same discipline virtio/uart use for host input.

void net_reset();                    // main.c init: MAC/gateway config + queue
void net_tx(char *frame, int len);   // device -> backend: the guest sent a frame
void net_poll();                     // main loop -> backend: deliver queued replies
