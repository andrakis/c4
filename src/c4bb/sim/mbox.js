// mbox.js - the host's side of the mailbox (devices.js MBOX_*).
//
// Two rings of i32 frames in guest RAM, read and written straight
// through the arena's typed array: no copy, no opcode. The guest's side
// is include/c4bb_mbox.h, and the two must agree word for word:
//
//   ring  [cap][head][tail][data...]     head/tail monotonic word counters
//   frame [len][type][seq][payload...]   len counts the header; never wraps
//   -1 at head%cap                        skip to the next multiple of cap
//
// Host->guest is the first half of the region, guest->host the second.
// Pure JS, no DOM, no Node: works in a page, a worker, and node alike.

const CAP = 0, HEAD = 1, TAIL = 2, DATA = 3, HDR = 3;

export class HostMailbox {
  // region = { base, len } from loader.boot(...).mbox
  constructor(arena, region) {
    this.arena = arena;
    this.base = region.base;
    this.len = region.len;
    const half = this.len >> 1;
    this.toGuest = { at: region.base >> 2, cap: (half >> 2) - DATA };
    this.toHost = { at: (region.base + half) >> 2, cap: (half >> 2) - DATA };
    this.seq = 0;
    this.layout();
  }

  /** write both ring headers; the rings are empty afterwards */
  layout() {
    const w = this.arena.i32;
    for (const r of [this.toGuest, this.toHost]) {
      w[r.at + CAP] = r.cap; w[r.at + HEAD] = 0; w[r.at + TAIL] = 0;
    }
  }

  /** words free in the guest's inbox */
  free() {
    const w = this.arena.i32, r = this.toGuest;
    return r.cap - (w[r.at + HEAD] - w[r.at + TAIL]);
  }

  /** queue a frame for the guest; false when the inbox is full */
  send(type, payload = []) {
    const w = this.arena.i32, r = this.toGuest;
    const len = HDR + payload.length;
    let head = w[r.at + HEAD];
    const tail = w[r.at + TAIL];
    let at = head % r.cap;
    let free = r.cap - (head - tail);
    if (at + len > r.cap) {
      if (free < (r.cap - at) + len) return false;
      w[r.at + DATA + at] = -1;
      head += r.cap - at;
      at = 0;
      free = r.cap - (head - tail);
    }
    if (free < len) return false;
    const f = r.at + DATA + at;
    w[f + 1] = type | 0;
    w[f + 2] = this.seq++;
    for (let i = 0; i < payload.length; i++) w[f + HDR + i] = payload[i] | 0;
    w[f] = len;
    w[r.at + HEAD] = head + len;
    return true;
  }

  /** take every frame the guest has written: [{type, seq, payload: Int32Array}] */
  drain() {
    const w = this.arena.i32, r = this.toHost;
    const out = [];
    const head = w[r.at + HEAD];
    let tail = w[r.at + TAIL];
    while (tail !== head) {
      let at = tail % r.cap;
      if (w[r.at + DATA + at] === -1) {
        tail += r.cap - at;
        if (tail === head) break;
        at = 0;
      }
      const f = r.at + DATA + at;
      const len = w[f];
      if (len < HDR || at + len > r.cap) throw new Error(`mbox: corrupt frame at word ${at} (len ${len})`);
      out.push({ type: w[f + 1], seq: w[f + 2], payload: w.slice(f + HDR, f + len) });
      tail += len;
    }
    w[r.at + TAIL] = tail;
    return out;
  }

  /** true when the guest has consumed every frame the host queued */
  inboxEmpty() {
    const w = this.arena.i32, r = this.toGuest;
    return w[r.at + HEAD] === w[r.at + TAIL];
  }

  /** words the guest has queued and the host has not drained */
  pending() {
    const w = this.arena.i32, r = this.toHost;
    return w[r.at + HEAD] - w[r.at + TAIL];
  }
}
