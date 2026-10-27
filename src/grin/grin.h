// grin.h -- what a grin application adds to the window kit (winkit.h)
// (docs/grin-plan.md 3.4).
//
// A grin application is a C4DOS program written against the window kit,
// the same functions the C4IX desktop's tools call, linked with
// src/grin/import.c in the kit's place and compiled with -D WK_IMPORT.
// Its main opens its windows and returns GRIN_RESIDENT; grin keeps the
// image loaded while it has a window, and runs the windows' callbacks
// from its loop. A second launch calls main again on the same image, so
// what belongs to a window goes in its state (wk_state), not in globals.
// When the last window closes, grin unloads the image, and its memory
// goes with it. An exit() inside a callback closes the application's
// windows and unloads it, and grin carries on.
//
//   #include "winkit.h"
//   #include "grin.h"
//   int main(int argc, char **argv) {
//       if (!grin_present()) { printf("Run CLOCK from grin.\n"); return 1; }
//       if (clock_open() < 0) return 1;
//       return GRIN_RESIDENT;
//   }
//
// Callbacks never block: each returns promptly, as Windows 3.x's did.

#ifndef GRIN_H
#define GRIN_H 1

// main's answer for "keep me loaded": 'G','R','E','S', written out
enum { GRIN_RESIDENT = 1196574035 };

int grin_present();                   // 1 when grin is running and published its table

#endif
