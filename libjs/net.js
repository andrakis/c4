// net.js - the machine's network card (docs/c4pc-network.md, N2): c4pc's
// NET window (src/c4pc/net.c), the same registers in page 0 that the
// kernel alone reaches, so the same C4IX drives it (src/c4ix/net.c):
//
//   0x580 STATUS  r: 1 a port (the link), 2 frames waiting
//   0x584 MAC0    r: the MAC's bytes 0-3, the first in the low byte
//   0x588 MAC1    r: its bytes 4-5
//   0x58c IP      r: the address it is to have, 10.4.0.ID+1
//   0x590 ADDR    w: where in memory (physical) the next command reads or writes
//   0x594 LEN     w: how many bytes
//   0x598 CMD     w: 1 send the frame at ADDR, LEN bytes; 2 the next frame
//                    received into ADDR, LEN bytes at most; 3 the address of
//                    the machine named at ADDR (a C string: vmN, or its own
//                    name); r: the result -- bytes sent or received (0: none
//                    waiting), the address (0: none), or -1
//   0x59c RXQ     r: frames waiting
//
// Its frames go to and come from a port: { send(frame), onframe } -- a
// UDP socket to another machine (cli.js --net udp:..., to c4pc-host or
// another cli.js: the same frames, a datagram each), or a BroadcastChannel
// between a page's tabs (ChannelPort). The card is machine ID's on a
// cable or a hub: its MAC 52:54:00:c4:00:ID, its address 10.4.0.ID+1/24, as
// the hypervisor's switch gives them, so machines of both kinds can share
// one network. It keeps 64 frames for the kernel, takes only those for its
// MAC or broadcast, and drops (counted) what it has no room for. Nothing
// here is Node's: a page fits one.

export const NET_BASE = 0x580, NET_END = 0x5a0;
const STATUS = 0x580, MAC0 = 0x584, MAC1 = 0x588, IP = 0x58c, ADDR = 0x590, LEN = 0x594, CMD = 0x598, RXQ = 0x59c;
const C_SEND = 1, C_RECV = 2, C_RESOLVE = 3;
const NQ = 64, FMAX = 1536;

export class NetCard {
  constructor(port, id = 1, name = '') {
    this.id = id | 0;
    this.name = name || `vm${this.id}`;
    this.mac = new Uint8Array([0x52, 0x54, 0x00, 0xc4, (this.id >> 8) & 255, this.id & 255]);
    this.ip = (0x0a040001 + this.id) >>> 0;
    this.q = [];
    this.addr = 0; this.len = 0; this.result = 0;
    this.rx = 0; this.tx = 0; this.rxb = 0; this.txb = 0; this.drops = 0;
    this.arena = null;                  // bus.js
    this.onwake = null;                 // a frame came: the host ends a nap (cli.js, worker.js)
    this.port = port;
    port.onframe = f => this.receive(f);
  }
  receive(f) {
    if (!(f instanceof Uint8Array)) f = new Uint8Array(f);
    if (f.length < 14 || f.length > FMAX) return;
    // its own MAC, or a broadcast or multicast: a hub's frames are everyone's
    if (!(f[0] & 1)) for (let k = 0; k < 6; k++) if (f[k] !== this.mac[k]) return;
    if (this.q.length >= NQ) { this.drops++; return; }
    this.q.push(f);
    this.rx++; this.rxb += f.length;
    if (this.onwake) this.onwake();
  }
  // a name to an address: vmN is 10.4.0.N+1, as beyond c4pc's switch
  resolve(s) {
    const m = /^vm(\d+)$/.exec(s);
    if (m && +m[1] < 254) return (0x0a040001 + +m[1]) | 0;
    if (s === this.name) return this.ip | 0;
    return 0;
  }
  read32(a) {
    switch (a) {
      case STATUS: return 1 | (this.q.length ? 2 : 0);
      case MAC0: return (this.mac[0] | this.mac[1] << 8 | this.mac[2] << 16 | this.mac[3] << 24) | 0;
      case MAC1: return this.mac[4] | this.mac[5] << 8;
      case IP: return this.ip | 0;
      case ADDR: return this.addr | 0;
      case LEN: return this.len | 0;
      case CMD: return this.result | 0;
      case RXQ: return this.q.length;
    }
    return 0;
  }
  write32(a, v) {
    switch (a) {
      case ADDR: this.addr = v >>> 0; return;
      case LEN: this.len = v >>> 0; return;
      case CMD: break;
      default: return;
    }
    const u8 = this.arena.u8, at = this.addr, n = this.len;
    this.result = -1;
    if (v === C_SEND) {
      if (n < 14 || n > FMAX || at + n > u8.length) return;
      const f = u8.slice(at, at + n);
      this.tx++; this.txb += n;
      try { this.port.send(f); } catch { /* dropped, as a cable would */ }
      this.result = n;
    } else if (v === C_RECV) {
      if (!this.q.length) { this.result = 0; return; }
      const f = this.q[0];
      const m = Math.min(f.length, n);
      if (at + m > u8.length) return;
      u8.set(m < f.length ? f.subarray(0, m) : f, at);
      this.q.shift();
      this.result = f.length;
    } else if (v === C_RESOLVE) {
      let s = '';
      for (let k = 0; k < 39 && at + k < u8.length && u8[at + k]; k++) s += String.fromCharCode(u8[at + k]);
      this.result = this.resolve(s);
    }
  }
  // frames in and out, bytes in and out, dropped (c4pc's net_counts)
  counts() { return [this.rx, this.tx, this.rxb, this.txb, this.drops]; }
}

// A page's tabs as a hub: every frame to every other tab that has one
// (BroadcastChannel NAME), each card keeping its own MAC's and broadcasts
export class ChannelPort {
  constructor(name = 'c4net') {
    this.onframe = null;
    this.bc = new BroadcastChannel(name);
    this.bc.onmessage = e => { if (this.onframe) this.onframe(new Uint8Array(e.data)); };
  }
  send(f) { this.bc.postMessage(f); }
  close() { this.bc.close(); }
}
