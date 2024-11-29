#include <stdio.h>
#include <stdlib.h>

#define int long

int *fun_tbl;

int fun_a (int a, int b) { return a + b; }
int fun_b (int a, int b) { return a - b; }
int fun_c (int a, int b) { return a / b; }
int fun_d (int a, int b) { return a * b; }
int fun_e (int a, int b) { return a % b; }
int fun_f (int a, int b) { return !a && !b; }

#if C4
int CALL2 (int *fun, int a, int b) {
	return fun(a, b);
}
#else
#define CALL2(fun,a,b) ((int(*)(int,int))fun)(a,b)
#endif

int main (int argc, char **argv) {
	int ix, iy, *ip, ir;

	fun_tbl = malloc(sizeof(int*) * 7);
	fun_tbl[0] = (int)&fun_a;
	fun_tbl[1] = (int)&fun_b;
	fun_tbl[2] = (int)&fun_c;
	fun_tbl[3] = (int)&fun_d;
	fun_tbl[4] = (int)&fun_e;
	fun_tbl[5] = (int)&fun_f;
	fun_tbl[0] = 0;

	ix = iy = 0;
	while(++ix < 10) {
		while(++iy < 6) {
			ip = (int*)fun_tbl[iy];
			ir = CALL2(ip, ix, iy);
			printf("ix=%ld,iy=%ld,ip=%p,ir=%ld\n", ix, iy, ip, ir);
		}
		iy = 0;
	}

	free(fun_tbl);
	return 0;
}
