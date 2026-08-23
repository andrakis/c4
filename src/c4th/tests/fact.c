// The program src/c4th/forth/fact.f hand-assembles, so that c4th's
// assembler can be checked against c4cc's code generator for the same
// source. No string literals: an IMM of a string address would be a
// relocated operand, and the comparison is about the instruction
// sequence, not about where a data segment happened to land.
int fact (int n) {
	if (n < 2) return 1;
	return n * fact(n - 1);
}

int main () {
	return fact(10);
}
