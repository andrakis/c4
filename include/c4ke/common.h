//
// C4KE Standard Library: c4ke/common.h
// Common definitions used by C4KE user-mode programs.
// Includes stream control (read-only for now)

#ifndef __C4KE_COMMON_H
#define __C4KE_COMMON_H

#ifndef DO_WHILE_0
#ifdef __c4cc__
#define DO_WHILE_0(body) if (1) body
#else  /* ifndef __c4cc */
#define DO_WHILE_0(body) do body while(0)
#endif /* ifndef __c4cc */
#else  /* ifdef __c4cc__ */
// GCC compatibility
#define c4ke_opcode(o)          0
#endif /* ifdef __c4cc__ */

#include <c4ke/opcodes.h>

static int OP_STREAM_DEST, OP_STREAM_UPDATE;

#if NO_INLINE
static int __c4ke_stream_dest(int *stream, int *available) { return __c4_opcode(available, stream, OP_STREAM_DEST); }
static int __c4ke_stream_update(int *stream) { return __c4_opcode(stream, OP_STREAM_DEST); }
#else /* if NO_INLINE */
#define __c4ke_stream_dest(stream, available) __c4_opcode(available, stream, OP_STREAM_DEST);
#define __c4ke_stream_update(stream)          __c4_opcode(stream, OP_STREAM_DEST);
#endif /* if NO_INLINE */

#if STDLIB_EXPERIMENTAL
static int __attribute__((constructor)) __c4ke_common_init (int *c4r) {
	OP_STREAM_DEST = c4ke_opcode("OP_STREAM_DEST");
	OP_STREAM_UPDATE = c4ke_opcode("OP_STREAM_UPDATE");
	return 0;
}
#endif

#endif /* ifndef __C4KE_COMMON_H */

