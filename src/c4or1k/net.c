#include "net.h"
#include "eth.h"

// See net.h. All multi-byte network fields are big-endian ("network
// order"); the get/put helpers below are explicit about it. Under
// c4lc's 64-bit `int` (and the native build's `#define int long`)
// every byte is masked, same discipline as mem.c.

// ---- our synthetic LAN peer -----------------------------------------
enum { GW_IP = 0x0A000001 };      // 10.0.0.1  gateway + DHCP server + DNS
enum { OFFER_IP = 0x0A000002 };   // 10.0.0.2  leased to the guest
enum { NETMASK = 0xFFFFFF00 };    // 255.255.255.0
enum { LEASE = 86400 };           // seconds
int gw_mac[6];                     // 02:00:00:00:00:01

char nbuf[2048];
int  nc;                           // build cursor into nbuf

// Reply queue. net_tx (called from inside a guest store) must not
// touch the RX ring or the IRQ line -- see net.h. So a finished reply
// is COPIED here; net_poll (main loop, between instructions) drains it
// into eth_rx and then reconciles the interrupt via eth_poll.
enum { NQ_SLOTS = 16, NQ_SZ = 2048 };
char *nq;                          // malloc'd (NQ_SLOTS*NQ_SZ = 32KB): a
                                   // static array this size blows c4lc's
                                   // 256KB data-segment budget, same reason
                                   // mem.c's `ram` and jit.c's arena are
                                   // malloc'd rather than static.
int  nq_len[NQ_SLOTS];
int  nq_head, nq_tail, nq_count;

void nq_push() {
    int i;
    if (nq_count >= NQ_SLOTS) return;              // full: drop (never happens at this scale)
    i = 0;
    while (i < nc) { nq[nq_tail * NQ_SZ + i] = nbuf[i]; ++i; }
    nq_len[nq_tail] = nc;
    nq_tail = (nq_tail + 1) % NQ_SLOTS;
    ++nq_count;
}

void net_poll() {
    while (nq_count > 0) {
        eth_rx(nq + nq_head * NQ_SZ, nq_len[nq_head]);
        nq_head = (nq_head + 1) % NQ_SLOTS;
        --nq_count;
    }
    eth_poll();                                    // reconcile the IRQ line here, not mid-store
}

// ---- byte helpers ----------------------------------------------------
int g8(char *f, int o)  { return f[o] & 0xFF; }
int g16(char *f, int o) { return (g8(f, o) << 8) | g8(f, o + 1); }
int g32(char *f, int o) { return (g16(f, o) << 16) | g16(f, o + 2); }

void p8(int v)  { nbuf[nc] = v & 0xFF; ++nc; }
void p16(int v) { p8(v >> 8); p8(v); }
void p32(int v) { p16(v >> 16); p16(v); }
void pmac(int *m) { int i; i = 0; while (i < 6) { p8(m[i]); ++i; } }
void pbytes(char *s, int off, int n) { int i; i = 0; while (i < n) { p8(s[off + i]); ++i; } }

// ones-complement Internet checksum over nbuf[off .. off+len)
int inet_csum(int off, int len) {
    int sum, i, w;
    sum = 0;
    i = 0;
    while (i + 1 < len) { sum = sum + ((g8(nbuf, off + i) << 8) | g8(nbuf, off + i + 1)); i = i + 2; }
    if (i < len) sum = sum + (g8(nbuf, off + i) << 8);   // odd trailing byte
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (~sum) & 0xFFFF;
}
// overwrite a 16-bit field already emitted at nbuf[off]
void patch16(int off, int v) { nbuf[off] = (v >> 8) & 0xFF; nbuf[off + 1] = v & 0xFF; }

// ---- ARP ------------------------------------------------------------
// f is the guest's ARP request. Reply only to "who has GW_IP".
void handle_arp(char *f) {
    int op, tpa, i;
    op = g16(f, 20);
    tpa = g32(f, 38);
    if (op != 1 || tpa != GW_IP) return;

    nc = 0;
    pbytes(f, 6, 6);                 // eth dst = requester's MAC (its src)
    pmac(gw_mac);                    // eth src = gateway MAC
    p16(0x0806);                     // ethertype ARP
    p16(1); p16(0x0800); p8(6); p8(4); p16(2);   // htype/ptype/hlen/plen/op=reply
    pmac(gw_mac); p32(GW_IP);        // sender = gateway
    pbytes(f, 6, 6); p32(g32(f, 28));// target = requester
    nq_push();
}

// ---- IPv4 framing shared by DHCP and ICMP replies -------------------
// emit Ethernet + IPv4 header; returns the offset of the IP total-
// length field (patched later) via globals; caller appends payload
// then calls ip_finish. src/dst are host-order IPs.
int ip_len_off, ip_hdr_off;
void ip_begin(int *dst_mac, int src_ip, int dst_ip, int proto) {
    nc = 0;
    pmac(dst_mac); pmac(gw_mac); p16(0x0800);    // Ethernet
    ip_hdr_off = nc;
    p8(0x45); p8(0);                              // ver/IHL, DSCP
    ip_len_off = nc; p16(0);                      // total length (patched)
    p16(0); p16(0x4000); p8(64); p8(proto);       // id, flags(DF), ttl, proto
    p16(0);                                       // header checksum (patched)
    p32(src_ip); p32(dst_ip);
}
void ip_finish() {
    int total;
    total = nc - ip_hdr_off;
    patch16(ip_len_off, total);
    patch16(ip_hdr_off + 10, 0);
    patch16(ip_hdr_off + 10, inet_csum(ip_hdr_off, 20));
    nq_push();
}

// ---- DHCP -----------------------------------------------------------
// scan DHCP options (from dopt) for message-type (53); returns 1
// DISCOVER / 3 REQUEST / 0 none.
int dhcp_msgtype(char *f, int dopt, int flen) {
    int o;
    o = dopt;
    while (o + 1 < flen) {
        int code, len;
        code = g8(f, o);
        if (code == 255) return 0;                // end
        if (code == 0) { o = o + 1; continue; }   // pad
        len = g8(f, o + 1);
        if (code == 53) return g8(f, o + 2);
        o = o + 2 + len;
    }
    return 0;
}

void handle_dhcp(char *f, int ihl, int udp) {
    int dhcp, mtype, reply_type, i, udp_off, udp_len;
    dhcp = udp + 8;                               // DHCP starts after UDP header
    if (g32(f, dhcp + 236) != 0x63825363) return; // DHCP magic cookie
    mtype = dhcp_msgtype(f, dhcp + 240, 2048);
    if (mtype == 1) reply_type = 2;               // DISCOVER -> OFFER
    else if (mtype == 3) reply_type = 5;          // REQUEST  -> ACK
    else return;

    ip_begin(eth_mac, GW_IP, 0xFFFFFFFF, 17);     // to the guest, IP broadcast
    udp_off = nc;
    p16(67); p16(68);                             // UDP sport/dport (BOOTPS->BOOTPC)
    p16(0); p16(0);                               // length (patched), checksum 0 = unused
    // DHCP fixed area
    p8(2); p8(1); p8(6); p8(0);                   // op=reply, htype=eth, hlen=6, hops
    p32(g32(f, dhcp + 4));                        // xid (echo)
    p16(0); p16(g16(f, dhcp + 10));               // secs=0, flags (echo)
    p32(0);                                       // ciaddr
    p32(OFFER_IP);                                // yiaddr
    p32(GW_IP);                                   // siaddr (next server)
    p32(0);                                       // giaddr
    pbytes(f, dhcp + 28, 6); i = 0; while (i < 10) { p8(0); ++i; }  // chaddr (16)
    i = 0; while (i < 192) { p8(0); ++i; }        // sname(64) + file(128)
    p32(0x63825363);                              // magic cookie
    // options
    p8(53); p8(1); p8(reply_type);                // message type
    p8(54); p8(4); p32(GW_IP);                    // server identifier
    p8(51); p8(4); p32(LEASE);                    // lease time
    p8(1);  p8(4); p32(NETMASK);                  // subnet mask
    p8(3);  p8(4); p32(GW_IP);                    // router
    p8(6);  p8(4); p32(GW_IP);                    // DNS
    p8(255);                                      // end
    while ((nc - udp_off) < 300) p8(0);           // pad BOOTP min payload
    udp_len = nc - udp_off;
    patch16(udp_off + 4, udp_len);                // UDP length
    ip_finish();
}

// ---- ICMP echo ------------------------------------------------------
void handle_icmp(char *f, int ihl, int icmp, int iptot) {
    int payload, plen, src_ip, i, icmp_off;
    if (g8(f, icmp) != 8) return;                 // only echo request
    src_ip = g32(f, 26);
    plen = iptot - ihl - 8;                        // ICMP data length (after 8-byte hdr)
    if (plen < 0) plen = 0;

    ip_begin(eth_mac, GW_IP, src_ip, 1);           // reply src=gw, dst=pinger
    icmp_off = nc;
    p8(0); p8(0);                                  // type=echo reply, code
    p16(0);                                        // checksum (patched)
    p16(g16(f, icmp + 4)); p16(g16(f, icmp + 6));  // id, seq (echo)
    pbytes(f, icmp + 8, plen);                     // echo the payload
    patch16(icmp_off + 2, inet_csum(icmp_off, 8 + plen));
    ip_finish();
}

// ---- DNS ------------------------------------------------------------
// A synthetic resolver. There is no real upstream (the whole peer is
// pure C, no host sockets -- see net.h), so every name is answered from
// a deterministic, made-up address instead of a real lookup: the QNAME
// bytes are hashed into an off-subnet 11.x.x.x address. Off-subnet
// matters -- the guest then routes to it via the gateway (whose ARP and
// ICMP echo this peer already answers), so `ping <name>` resolves AND
// gets replies, where before name resolution failed outright. Only A
// (IPv4) queries get an answer; anything else (AAAA, ...) gets a
// well-formed NOERROR/0-answer response so the resolver moves on rather
// than hanging. Real name resolution needs the c4mp socket bridge
// (docs/c4or1k-design.md's M15/future-work + M18 plan).
void handle_dns(char *f, int l4) {
    int dns, id, qdcount, qtype, qname_off, o, hash, synth_ip, sport, src_ip, qend, qlen, i;
    dns = l4 + 8;                                  // DNS starts after the UDP header
    id = g16(f, dns);
    qdcount = g16(f, dns + 4);
    if (qdcount < 1) return;

    // Walk the QNAME labels (bounded) to find its terminating 0, then
    // QTYPE/QCLASS follow. Hash the name bytes for the synthetic answer.
    qname_off = dns + 12;
    o = qname_off;
    hash = 0x811c9dc5;                             // FNV-1a offset basis
    i = 0;
    while (i < 255 && (g8(f, o) != 0)) {
        hash = (hash ^ g8(f, o)) * 16777619;
        ++o; ++i;
    }
    qend = o;                                      // the 0 length-octet
    qtype = g16(f, qend + 1);
    qlen = (qend + 1) - qname_off;                 // QNAME length incl. the 0 octet
    src_ip = g32(f, 26);
    sport = g16(f, l4);                            // guest's query source port

    // 11.x.x.x from the low 24 bits of the hash (never .0 in the last
    // octet, to avoid a network address).
    synth_ip = 0x0B000000 | (hash & 0x00FFFFFF);
    if ((synth_ip & 0xFF) == 0) synth_ip = synth_ip | 1;

    ip_begin(eth_mac, GW_IP, src_ip, 17);          // reply src = the DNS server, dst = guest
    { int udp_off, udp_len, ancount;
      ancount = (qtype == 1) ? 1 : 0;              // answer only A queries
      udp_off = nc;
      p16(53); p16(sport);                         // UDP sport=53, dport=guest
      p16(0); p16(0);                              // length (patched), checksum 0 (unused)
      p16(id);                                     // DNS id (echo)
      p16(0x8180);                                 // flags: response, RD, RA, NOERROR
      p16(1); p16(ancount); p16(0); p16(0);        // QD=1, AN, NS=0, AR=0
      pbytes(f, qname_off, qlen);                  // echo the QNAME
      p16(qtype); p16(1);                          // QTYPE, QCLASS=IN
      if (ancount) {
          p16(0xC00C);                             // NAME: pointer to the QNAME at offset 12
          p16(1); p16(1);                          // TYPE=A, CLASS=IN
          p32(60);                                 // TTL
          p16(4); p32(synth_ip);                   // RDLENGTH=4, RDATA=synthetic IPv4
      }
      udp_len = nc - udp_off;
      patch16(udp_off + 4, udp_len);               // UDP length
    }
    ip_finish();
}

// ---- entry ----------------------------------------------------------
void net_tx(char *f, int len) {
    int ethertype, proto, ihl, l4, iptot;
    if (len < 14) return;
    ethertype = g16(f, 12);

    if (ethertype == 0x0806) { handle_arp(f); return; }
    if (ethertype != 0x0800) return;               // only ARP and IPv4

    ihl = (g8(f, 14) & 0x0F) * 4;
    proto = g8(f, 23);
    iptot = g16(f, 16);
    l4 = 14 + ihl;                                 // transport header offset
    if (proto == 17 && g16(f, l4 + 2) == 67) handle_dhcp(f, ihl, l4);
    else if (proto == 17 && g16(f, l4 + 2) == 53) handle_dns(f, l4);
    else if (proto == 1) handle_icmp(f, ihl, l4, iptot);
}

void net_reset() {
    gw_mac[0] = 0x02; gw_mac[1] = 0x00; gw_mac[2] = 0x00;
    gw_mac[3] = 0x00; gw_mac[4] = 0x00; gw_mac[5] = 0x01;
    nq_head = 0; nq_tail = 0; nq_count = 0;
    nq = malloc(NQ_SLOTS * NQ_SZ);
}
