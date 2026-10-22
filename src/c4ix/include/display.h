// display.h -- libjs's whole display, taken from the desktop and given
// back, as a DOS game took the screen from Windows 3 to 98.
//
// libjs's display (libjs/guest/gui.h) serves one program at a time, and
// the desktop is usually that program. A game that draws on the display
// itself (Wolfenstein 3D, or raycast -G) asks the kernel for it first;
// the desktop gives way within a frame or two -- its windows' programs
// run on, unseen -- and has it back, and draws everything again, when
// the game gives it up or ends:
//
//   if (!udisplay_take(0)) { printf("the display is in use\n"); return 1; }
//   gui_attach(ring, 65536, 640, 480);
//   ...
//   gui_detach();
//   udisplay_give();
//
// udisplay_take answers 1 when the display is the caller's, and 0 when a
// program that does not give way has it. DISP_YIELDS says the caller
// will give way in turn, as the desktop does: it then watches
// udisplay(DISP_STATE, 0), which is 1 when another asks, yields with
// udisplay(DISP_YIELD, 0), and is 2 until it has the display back (0).
// src/c4ix/sys.c, "The display", has the kernel's half.
//
// C4IX only: a program for the bare machine as well must know where it
// is before it asks (Wolfenstein 3D's plat-c4ix.c does).

// libjs's display, taken from the desktop and given back (include/display.h)
#ifndef __C4IX_DISPLAY_H
#define __C4IX_DISPLAY_H 1
enum { DISP_TAKE = 0, DISP_STATE = 1, DISP_YIELD = 2, DISP_GIVE = 3, DISP_YIELDS = 1 };
int  udisplay(int op, int flags);
int  udisplay_take(int flags);
void udisplay_give();
#endif
