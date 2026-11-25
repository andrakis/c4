// kboot -- boot another kernel from inside C4KE, without a reset.
//
//   kboot c4ix.c4r [arguments...]
//
// C4KE shuts down as it does when init exits, and then runs the image,
// from its RAM filesystem (what b4ke just linked) or the disk, passing
// the arguments after the name. When that kernel shuts down, C4KE
// returns to whatever loaded it. extensions/c4ke_boot.c does the work.

int main (int argc, char **argv) {
	int op;
	if (argc < 2) {
		printf("usage: kboot <image.c4r> [arguments...]\n");
		printf("  stops C4KE and boots the image, passing it the arguments\n");
		return 1;
	}
	if (!(op = __c4_opcode("OP_KBOOT", OP_REQUEST_SYMBOL))) {
		printf("kboot: this kernel cannot boot another (no OP_KBOOT)\n");
		return 1;
	}
	// arguments reversed: the kernel reads sp[1] (argc) first (u0.h)
	return __c4_opcode(argv + 1, argc - 1, op) ? 1 : 0;
}
