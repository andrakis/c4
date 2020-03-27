#ifndef __C4_H

#define _CRT_SECURE_NO_WARNINGS 1

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>

#ifdef __GNUC__
	#include <errno.h>
	#include <unistd.h>

	#define _O_CREAT O_CREAT
	#define _S_IREAD 0
	#define _S_IWRITE 0
#else

	#include <io.h>
	#include <sys\types.h>
	#include <sys\stat.h>

	#define open(p,m) _open(p, m, _S_IREAD | _S_IWRITE)
	#define read _read
	#define write _write
	#define close _close

	#if _WIN64
		#define __INTPTR_TYPE__ long long
	#elif _WIN32
		#define __INTPTR_TYPE__ int
	#endif // if _WIN64
#endif // ifdef __GNUC__

#include <fcntl.h>
#define int __INTPTR_TYPE__


#endif
