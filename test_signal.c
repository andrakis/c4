#include <stdio.h>

#include "u0.c"

int g_sig;
// TODO: this signature is needed to get access to the signal number, but it's ugly.
void generic_signal_handler (int sig, int a, int b, int c, int d) {
	printf("generic_signal_handler: signal %d\n", sig);
	g_sig = sig;
}

int main (int argc, char **argv) {
	signal(SIGUSR1, (int *)&generic_signal_handler);
	signal(SIGUSR2, (int *)&generic_signal_handler);
	g_sig = 0;
	kill(pid(), SIGUSR1);
	while (g_sig == 0) sleep(1);
	g_sig = 0;
	kill(pid(), SIGUSR2);
	while (g_sig == 0) sleep(1);
	printf("test finished\n");
	return 0;
}
