//
// C4KE Standard Library: unistd.h
//

#ifndef __UNISTD_H
#define __UNISTD_H

#ifndef __c4cc__
#include_next <unistd.h>
#else /* ifndef __c4cc__ */

#ifndef C4KE
#include <c4ke/common.h>

static int OP_UNISTD_OPEN, OP_UNISTD_READ, OP_UNISTD_CLOSE;

#if STDLIB_EXPERIMENTAL
#if NO_INLINE
int open (char *pathname, int flags)    { return __c4_opcode(flags, pathname, OP_UNISTD_OPEN); }
int read (int fd, void *buf, int count) { return __c4_opcode(count, buf, fd, OP_UNISTD_READ); }
int close (int fd)                      { return __c4_opcode(fd, OP_UNISTD_CLOSE); }
#else /* if NO_INLINE */
#define open(pathname, flags)                    __c4_opcode(flags, pathname, OP_UNISTD_OPEN)
#define read(fd, buf, count)                     __c4_opcode(count, buf, fd, OP_UNISTD_READ)
#define close(fd)                                __c4_opcode(fd, OP_UNISTD_CLOSE)
#endif /* if NO_INLINE */

static int __attribute__((constructor)) __unistd_init (int *c4r) {
	OP_UNISTD_OPEN  = c4ke_opcode("OP_UNISTD_OPEN");
	OP_UNISTD_READ  = c4ke_opcode("OP_UNISTD_READ");
	OP_UNISTD_CLOSE = c4ke_opcode("OP_UNISTD_CLOSE");
	return 0;
}
#endif /* #if STDLIB_EXPERIMENTAL */

#endif /* ifndef C4KE */
#endif /* ifndef/else __c4cc__ */

#endif /* ifndef __UNISTD_H */

