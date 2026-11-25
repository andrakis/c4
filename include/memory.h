//
// C4 Standard Library header: memory.h
//

#ifndef __MEMORY_H
#define __MEMORY_H 1

#ifndef C4CC
#ifdef __MINGW32__
#include_next <memory.h>  // MinGW (docs/windows.md) has no /usr/include
#else
#include </usr/include/memory.h>
#endif
#endif

#endif
