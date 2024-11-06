//
// C4KE Standard Library header: signal.h
//

#ifndef __SIGNAL_H
#define __SIGNAL_H 1

#define SIGHANDLER(name, signame) \
	static void name (int signame, int __ignored1, int __ignored2, int __ignored3, int __ignored4)

// Signals
// Stop regular compilers from caring about these signals
#undef SIGHUP
#undef SIGINT
#undef SIGQUIT
#undef SIGILL
#undef SIGTRAP
#undef SIGABRT
#undef SIGBUS
#undef SIGFPE
#undef SIGKILL
#undef SIGUSR1
#undef SIGSEGV
#undef SIGUSR2
#undef SIGPIPE
#undef SIGALRM
#undef SIGTERM
#undef SIGSTKFLT
#undef SIGCHLD
#undef SIGCONT
#undef SIGSTOP
#undef SIGTSTP
#undef SIGTTIN
#undef SIGTTOU
#undef SIGURG
#undef SIGXCPU
#undef SIGXFSZ
#undef SIGVTALRM
#undef SIGPROF
#undef SIGWINCH
#undef SIGIO
#undef SIGPWR
#undef SIGSYS
#undef SIGRTMIN
#undef SIGRTMAX
enum {
  SIGHUP = 1  ,SIGINT      ,SIGQUIT     ,SIGILL      ,SIGTRAP,
  SIGABRT     ,SIGBUS      ,SIGFPE      ,SIGKILL     ,SIGUSR1,
  SIGSEGV     ,SIGUSR2     ,SIGPIPE     ,SIGALRM     ,SIGTERM,
  SIGSTKFLT   ,SIGCHLD     ,SIGCONT     ,SIGSTOP     ,SIGTSTP,
  SIGTTIN     ,SIGTTOU     ,SIGURG      ,SIGXCPU     ,SIGXFSZ,
  SIGVTALRM   ,SIGPROF     ,SIGWINCH    ,SIGIO       ,SIGPWR,
  SIGSYS      ,SIGRTMIN = 32,SIGRTMAX = 63, SIGMAX = 64
};

static int __signal_OP_USER_SIGNAL;
SIGHANDLER(__signal_default_sighandler, sig) {
	printf("signal.h: received unhandled signal %d\n", sig);
	exit(-1);
}

static int __attribute__((constructor)) __signal_initialize () {
	int sig;

	__signal_OP_USER_SIGNAL = __c4_opcode("OP_USER_SIGNAL", OP_REQUEST_SYMBOL);
	// Install default signal handlers
    if (U0_DEBUG) printf("u0: signal setup start\n");

	sig = SIGMIN;
	while (sig < SIGMAX) {
		signal(sig++,  (int *)&__signal_default_sighandler);
	}
}
#if C4CC
static int signal (int sig, int *handler) {
	return __c4_opcode(handler, sig, __signal_OP_USER_SIGNAL);
}
#else
#define signal __signal
#endif

#endif
