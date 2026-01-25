#ifndef __C4M_H
#define __C4M_H 1

#ifndef C4CC
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
#define __c4_ops_list() 0
#define __c4_float()  0
#define __c4_invoke(x) 0
#define __c4_signal(x,y) 0
#endif /* ifndef __c4__ */


#endif /* ifndef __C4M_H */
