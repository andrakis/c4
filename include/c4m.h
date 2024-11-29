#ifndef __C4M_H
#define __C4M_H

#ifndef __c4__
// Dummy these out so that gcc can syntax check
#define __c4_tlev()
#define __c4_trap(x)
#define __c4_opcode(x,...) 0
#define __c4_jmp(x)
#define __c4_adjust(x)
#define __opcode(x) 0
#define install_trap_handler(x) 0
#define __c4_configure(...) 0
#define __c4_cycles() 0
#define __time() c4m_time()
#define __c4_usleep(x) 0
#define __c4_info()    0
#include "c4m_util.c"
#endif /* __c4__ */

#endif /* __C4M_H */
