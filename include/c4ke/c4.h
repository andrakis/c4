#ifndef __C4_H
#define __C4_H 1

// Determine whether this is compiled natively or not.
#if __c4cc__
#define NATIVE 0
#elif __GNUC__ // #if __c4cc_
#define NATIVE 1
#else          // #elif __GNUC__
// TODO: MSVC detection
#define NATIVE 1
#endif         // #if __c4cc__

// Make it easier for C4PRE
#if NATIVE
#define NOT_NATIVE 0
#else // #if NATIVE
#define NOT_NATIVE 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <fcntl.h>

#ifdef __GNUC__
#include <unistd.h>
#else
#if _WIN64
#define __INTPTR_TYPE__ long long
#elif _WIN32
#define __INTPTR_TYPE__ int
#endif // if _WIN64
#endif // ifdef __GNUC__

// For windows, remove __attribute__
#ifndef __GNUC__
#define __attribute__(x)
// No SIGRTMAX, define our own
#define SIGRTMAX 64
#endif

#ifdef C4M_SIGNALS
// On C4CC, handle signals differently.
// TODO: Not handled at all right now, but under c4m could work.
#ifdef C4CC
int __c4_signal_init () { return 0; }
int pending_signal; // if a signal is pending
int *signal_handlers;
void __c4_signal_shutdown () { }

// On C4CC, use the direct time and sleep functions supported by c4m.
#define c4m_time() __time()
#define usleep(x)  __c4_usleep(x)
#else // #ifdef C4CC
// On all other platforms, try to implement signal handling.

#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wformat-security"
#pragma GCC diagnostic ignored "-Wunused-result"

// Support signal handlers
#include <signal.h>
#include <stdlib.h>
static __INTPTR_TYPE__ *signal_handlers;
static __INTPTR_TYPE__  pending_signal;
static void c4_sig_handler (int sig) {
	// printf("c4m: sig handler %d\n", sig);
	pending_signal = sig;
}
static int __c4_signal_init () {
	__INTPTR_TYPE__ t;
	if (signal_handlers == 0) {
		// First initialization
		if (!(signal_handlers = malloc(t = sizeof(int) * SIGRTMAX))) {
			printf("c4m: signal handler allocation failure\n");
			return 1;
		}
		// Initialize handlers to 0
		memset(signal_handlers, 0, t);
	}
	return 0;
}
static __INTPTR_TYPE__ *__c4_signal (__INTPTR_TYPE__ sig, __INTPTR_TYPE__ *handler) {
	__INTPTR_TYPE__ *old;
	old = 0;
	// printf("c4m: installing signal handler for signal %d @ 0x%x\n", sig, handler);
	//return (__INTPTR_TYPE__) signal(sig, handler);
	if (signal_handlers[sig]) {
		// printf("c4m: overwriting signal handler\n");
		old = (__INTPTR_TYPE__ *)signal_handlers[sig];
	}
	signal_handlers[sig] = (__INTPTR_TYPE__)handler;
	signal(sig, c4_sig_handler);
	return old;
}
static void __c4_signal_shutdown () {
	if (signal_handlers) {
		free(signal_handlers);
		signal_handlers = 0;
	}
}

static int __c4_sigint () { return SIGINT; }

#ifdef __c4_usleep
// TODO: why is this causing issues?
#undef __c4_usleep
#undef __opcode
static int __c4_usleep (int useconds) {
	return usleep(useconds);
}
#endif // ifdef __c4_usleep

#endif // #ifndef C4CC

#endif // #ifdef C4M_SIGNALS

#ifndef C4CC
// Please define this for your architecture if required.
#define int __INTPTR_TYPE__
#endif

#endif // ifndef __C4_H
