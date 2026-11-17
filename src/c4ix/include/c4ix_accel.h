// c4ix_accel.h -- C4IX's accelerators, from inside: loops its programs
// hand to the host, in C4IX's own JavaScript (src/c4ix/accel.mjs), one
// instruction however much they do (libjs/accel.js, docs/accel.md 7).
//
// ANNOUNCED, NEVER PROBED, as a game's (libjs/guest/accel.h): only where
// INFO says the machine has the loader, and only while the machine's switch
// for an operating system's accelerators is on (the option osAccel, the
// page's &osaccel=0, the desktop's Settings). Ask cx_find() before each
// piece of work, not once for good: the switch may turn between two. 0
// means none, and the program runs its own C, as on every other machine
// (native c4m, c4bb, C4DOS) and in every build but c4lc's.
//
//   if ((b = cx_find())) crc = cx_crc32(b, crc, buf, len);
//   else ... the loop ...
//
// cx_os(-1) is the switch as it is: 3 on, 2 off, 0 or 1 where the machine
// has no OS accelerators (bit 1: it has); cx_os(1) and cx_os(0) turn it,
// giving it as it was.

#ifndef __C4IX_ACCEL_H
#define __C4IX_ACCEL_H 1

enum { CX_INFO = 0x100000, CX_FIND = 0x1000, CX_OS = 0x1001 };

// the module's operations, by their place in its array
enum {
    CX_CRC32 = 0,       // (crc, buf, len): libarc's arc_crc32
    CX_INFLATE = 1      // (state): libarc's codes() loop, as far as it safely goes
};

#ifdef __c4_inttypes__
#define cx_find() ((__c4_info() & CX_INFO) ? __c4_opcode("c4ix", CX_FIND) : 0)
#define cx_os(v) ((__c4_info() & CX_INFO) ? __c4_opcode((v), CX_OS) : 0)
#define cx_crc32(b, crc, buf, len) __c4_opcode((crc), (buf), (len), (b) + CX_CRC32)
#define cx_inflate(b, st) __c4_opcode((st), (b) + CX_INFLATE)
#else
#define cx_find() 0
#define cx_os(v) 0
#define cx_crc32(b, crc, buf, len) 0
#define cx_inflate(b, st) 0
#endif

#endif
