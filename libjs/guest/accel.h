// accel.h -- the accelerators, from inside the machine: a game's heaviest
// loops run by the host, in the game's own JavaScript (games/NAME/c4/
// accel.mjs), one instruction however much they do (libjs/accel.js,
// docs/accel.md).
//
// ANNOUNCED, NEVER PROBED: ask accel_find() for a game's by name, and use
// them only if it gave a number. On any other machine -- native c4m, c4bb,
// C4DOS, libjs with accel off -- INFO says nothing of them, accel_find()
// gives 0, and a program keeps its own loops and draws with those. Only
// c4lc's builds can ask (__c4_inttypes__): built by another compiler,
// accel_find() is 0.
//
//   base = accel_find("quake");
//   if (base)
//       __c4_opcode(args..., base + QA_SPANS8);    // the module's op 0
//   else
//       ... the loop ...
//
// The game's own header names its operations' numbers and arguments.

#ifndef __C4_ACCEL_H
#define __C4_ACCEL_H 1

enum { C4I_ACCEL = 0x100000, ACC_FIND = 0x1000 };

#ifdef __c4_inttypes__
#define accel_present() ((__c4_info() & C4I_ACCEL) != 0)
#define accel_find(name) (accel_present() ? __c4_opcode((name), ACC_FIND) : 0)
#else
#define accel_present() 0
#define accel_find(name) 0
#endif

#endif
