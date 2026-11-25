// A guarded header whose #endif is the LAST line -- which is every guarded
// header there is. Its line number is what used to collide. Keep #endif on
// line 6: c4lc_ppinc.c includes the next header from ITS line 6.
#ifndef C4LC_PPINC_H
#define C4LC_PPINC_H
enum { PPINC_OK = 7 };
#endif
