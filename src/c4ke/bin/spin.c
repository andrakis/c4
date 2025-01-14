// C4 Test program: spin
//
// Run forever doing nothing.

#include <u0.h>

int cont;

void sigint () {
	cont = 0;
	printf("spin: SIGINT, cont == %d\n", cont);
	exit(0);
}

int main (int argc, char **argv) {
	cont = 1;
	// Set kernel focus to this task so that ctrl+c kills it
	c4ke_set_focus(pid());
	signal(SIGINT, (int *)&sigint);
	printf("spin: running forever, CTRL+C to abort\n");
	if (argc > 1)
		printf("spin: argument provided, initiating non-restful spinning\n");
	// TODO: cont not getting updated?
	while(cont) {
		if (argc <= 1)
			sleep(100);
		//printf("spin: cont == %d\n", cont);
	}
	return 0;
}
