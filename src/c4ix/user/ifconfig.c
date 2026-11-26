// C4IX userland: ifconfig -- the machine's network (docs/c4pc-network.md).
//
//   ifconfig                      net0, the c4net card, and lo
//   ifconfig net0 A.B.C.D[/BITS]  net0's address (a mask of BITS, 24 if not said)
//
// The card is c4pc's: each machine of the hypervisor's has one on its
// switch, 10.4.0.ID+1/24 (VM 0, the control, is 10.4.0.1).

#include "c4ix_user.h"
#include "c4ix_net.h"

static int bits_of(int mask) {
    int n;
    n = 0;
    while (n < 32 && (mask >> (31 - n)) & 1) ++n;
    return n;
}

int main(int argc, char **argv) {
    int v[NI_WORDS], ip, bits;
    char a[20], b[20], c[20];
    if (argc == 3 && !ustrcmp(argv[1], "net0")) {
        if (!(ip = uparseip(argv[2], &bits))) { ufprintf(STDERR, "ifconfig: %s: not an address\n", argv[2]); return 2; }
        if (bits < 0) bits = 24;
        if (unetset(ip, bits) < 0) { ufprintf(STDERR, "ifconfig: /%d: not a mask C4IX takes (8 to 30)\n", bits); return 1; }
        return 0;
    }
    if (argc != 1) { ufprintf(STDERR, "usage: ifconfig [net0 A.B.C.D[/BITS]]\n"); return 2; }
    if (unetinfo(v) < NI_WORDS) { ufprintf(STDERR, "ifconfig: this kernel has no network\n"); return 1; }
    if (v[NI_UP]) {
        uprintf("net0: flags=UP,BROADCAST,RUNNING  mtu 1500  (c4net, the hypervisor's switch)\n");
        uprintf("        inet %s  netmask %s  broadcast %s\n", uipstr(v[NI_IP], a), uipstr(v[NI_MASK], b),
            uipstr(v[NI_IP] | (v[NI_MASK] ^ NET_ALL), c));
        uprintf("        ether %02x:%02x:%02x:%02x:%02x:%02x\n", v[NI_MAC], v[NI_MAC + 1], v[NI_MAC + 2],
            v[NI_MAC + 3], v[NI_MAC + 4], v[NI_MAC + 5]);
        uprintf("        RX packets %d  bytes %d\n", v[NI_RX], v[NI_RXB]);
        uprintf("        TX packets %d  bytes %d\n", v[NI_TX], v[NI_TXB]);
        uprintf("        dropped %d  malformed %d  to no port %d\n", v[NI_DROP], v[NI_BAD], v[NI_NOPORT]);
    } else uprintf("net0: none (c4pc's hypervisor gives each machine one: GRUB's \"C4PC hypervisor\" entries)\n");
    uprintf("lo: flags=UP,LOOPBACK,RUNNING  mtu 1500\n");
    uprintf("        inet 127.0.0.1  netmask 255.0.0.0\n");
    uprintf("        packets %d\n", v[NI_LO]);
    uprintf("sockets open: %d (/%d)\n", v[NI_SOCKS], bits_of(v[NI_MASK]));
    return 0;
}
