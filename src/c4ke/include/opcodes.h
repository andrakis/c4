//
// C4KE Standard Library: c4ke/opcodes.h
// Provides the interface to C4KE opcodes.

#ifndef __C4KE_OPCODES_H
#define __C4KE_OPCODES_H

#ifdef __c4cc__
#ifdef C4KE
// C4KE-specific version of opcodes interface
#else
// User-mode specific version of opcodes interface

// C4KE opcode: int request_opcode(char *name)
enum { OP_REQUEST_SYMBOL = 128 };

static int c4ke_opcode(char *opcode) {
	return __c4_opcode(opcode, OP_REQUEST_SYMBOL);
}

#endif /* ifdef/else C4KE */

#else /* ifndef/else __c4cc__ */
// GCC compatibility
#define c4ke_opcode(o)          0
#endif /* ifdef __c4cc__ */

#endif /* ifndef __C4KE_OPCODES_H */

