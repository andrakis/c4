#ifndef __C4_H
#define __C4_H

#ifndef __c4__
#include <stdio.h>
#include <stdlib.h>
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
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wformat-security"
#pragma GCC diagnostic ignored "-Wunused-result"

// Support signal handlers
#include <signal.h>
static __INTPTR_TYPE__ *signal_handlers;
static __INTPTR_TYPE__  pending_signal;
void c4_sig_handler (int sig) {
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

// Please define this for your architecture if required.
#define int __INTPTR_TYPE__

static int __c4_sigint () { return SIGINT; }
static int __c4_usleep (int useconds) {
	return usleep(useconds);
}

#endif // ifndef __c4__

#endif
