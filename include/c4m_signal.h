// c4m_signal.h -- a guest's signal handlers, for a c4m that is itself a
// C4KE program (c4m.c built by a compiler with a preprocessor, with u0:
// /bin/c4m in C4IX, which runs it through its C4KE compatibility layer).
//
// Native c4m catches a host signal with signal(2) and traps its guest
// with it (include/c4.h). Built for the VM, c4m used to hand a guest's
// SIGH straight to the machine under it -- the outer SIGH, which knows
// nothing of tasks: it replaced the kernel's own handler with an address
// that meant something only inside c4m's guest, and c4m itself had no
// handler, so Ctrl-C in a Command Prompt ended c4m instead of reaching
// C4KE. Here the kernel c4m runs on is asked the C4KE way, u0's
// signal(), for a handler of c4m's own that marks the signal pending;
// c4m's loop then traps the guest with it, as native c4m does.
//
// Included by c4m.c after u0.h, and only by a build that has both, so
// plain c4 (which skips '#' lines and would compile this) never sees it.
#ifndef __C4M_SIGNAL_H
#define __C4M_SIGNAL_H 1

enum { C4M_NSIG = 64 };

// A handler is entered through a trap frame, C4KE's and C4IX's alike
// (C4KE's own signal_forwarder): its arguments are the trap's.
static void c4m_sig_forward (int type, int sig, int mode, int a, int *bp, int *sp, int *returnpc) {
	if (sig > 0 && sig < C4M_NSIG) pending_signal = sig;
}

// The guest's SIGH: remember its handler and answer the one it replaces.
int *c4m_guest_signal (int sig, int *handler) {
	int *old;
	if (sig <= 0 || sig >= C4M_NSIG) return 0;
	if (!signal_handlers) {
		if (!(signal_handlers = (int *)malloc(C4M_NSIG * sizeof(int)))) return 0;
		memset(signal_handlers, 0, C4M_NSIG * sizeof(int));
	}
	old = (int *)signal_handlers[sig];
	signal_handlers[sig] = (int)handler;
	signal(sig, (int *)&c4m_sig_forward);
	return old;
}

#endif
