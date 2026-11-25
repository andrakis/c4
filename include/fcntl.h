//
// C4PRE override file: fcntl.h
//

#ifndef __FCNTL_H
#define __FCNTL_H

#ifndef C4CC
#ifdef __MINGW32__
#include_next <fcntl.h>  // MinGW (docs/windows.md) has no /usr/include
#else
#include </usr/include/fcntl.h>
#endif
#endif

#endif
