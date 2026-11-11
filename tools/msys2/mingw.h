/* Included first in every `gcc -m32` build on Windows, by tools/msys2/gcc:
 * what MinGW's i686 gcc lacks of what these programs expect of Linux.
 * It is not a POSIX layer, only the few things the 32-bit host builds
 * use, and docs/windows.md says why they are MinGW builds at all.
 *
 * It includes nothing: every name a header declares here is one a
 * program cannot define for itself (advent has a read(), Quake a
 * filelength()), and MinGW's headers declare many that Linux's do not. */
#ifndef C4_MSYS2_MINGW_H
#define C4_MSYS2_MINGW_H 1

/* printf and scanf as C99 has them, and glibc: msvcrt's print %e with
 * three exponent digits, and round otherwise. */
#ifndef __USE_MINGW_ANSI_STDIO
#define __USE_MINGW_ANSI_STDIO 1
#endif

/* Bytes in, bytes out. MinGW opens every file, and stdin, stdout and
 * stderr, in text mode, which turns \n into \r\n and stops reading at
 * a ^Z. These programs write images and compare output byte for byte.
 * (io.h's and stdlib.h's names, declared by hand: see above.) */
int __cdecl _setmode(int, int);
extern int *__cdecl __p__fmode(void);
static void __attribute__((constructor)) c4_msys2_binary (void) {
	*__p__fmode() = 0x8000;            /* _O_BINARY */
	_setmode(0, 0x8000);
	_setmode(1, 0x8000);
	_setmode(2, 0x8000);
}

#define SIGRTMAX 64
#define S_IRUSR 0400                   /* _S_IREAD, _S_IWRITE, _S_IEXEC */
#define S_IWUSR 0200
#define S_IXUSR 0100
#endif
