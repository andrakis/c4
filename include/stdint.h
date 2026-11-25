// C4KE override header: stdint.h
//

#ifndef __STDINT_H
#define __STDINT_H 1

#ifndef C4CC
#ifdef __MINGW32__
#include_next <stdint.h>  // MinGW (docs/windows.md) has no /usr/include
#else
#include </usr/include/stdint.h>
#endif
#endif

#endif
