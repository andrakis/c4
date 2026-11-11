// C4KE override header: time.h
//

#ifndef __TIME_H
#define __TIME_H 1

#ifndef C4CC
#ifdef __MINGW32__
#include_next <time.h>  // MinGW (docs/windows.md) has no /usr/include
#else
#include </usr/include/time.h>
#endif
#endif

#endif

