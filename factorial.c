// C4 test program: factorial
//
// Offers a few ways to calculate testing various styles

#include <stdio.h>

#define int long long

int factorial_recursive_impl (int n, int acc) {
	if (n <= 1) return acc;
	return factorial_recursive_impl(n - 1, n * acc);
}
int factorial_recursive (int n) { return factorial_recursive_impl(n, 1); }

int factorial_while (int n) {
	int acc;
	acc = 1;
	while(n > 1)
		acc = acc * n--;
	return acc;
}

int main (int argc, char **argv) {
	int n;

	n = 10;

	printf("Factorial: %lld\n", factorial_recursive(n));
	printf("Factorial: %lld\n", factorial_while(n));
	
	return 0;
}
