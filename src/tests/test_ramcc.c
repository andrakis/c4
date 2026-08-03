// C4KE Test: the compiler inside the OS. c4sp (the Lisp interpreter)
// runs c4lc.lisp under C4KE, compiles a C source file, writes the
// image to the kernel RAM filesystem, and the kernel executes the
// fresh binary from memory. No write ever touches the host
// filesystem: the VM has no write syscall, only the RAM-FS opcodes.
int main (int argc, char **argv) {
	char **cargv;
	int task, len;

	if (!(cargv = malloc(8 * sizeof(char *)))) return 1;
	cargv[0] = "c4sp.c4r";
	cargv[1] = "-c";
	cargv[2] = "500000";
	cargv[3] = "src/c4sp/lisp/c4lc.lisp";
	cargv[4] = "src/tests/hello.c";
	cargv[5] = "ramcc.c4r";
	cargv[6] = 0;
	printf("ramcc: compiling hello.c inside C4KE...\n");
	task = kern_user_start_c4r(6, cargv, "c4sp", PRIV_USER);
	if (task <= 0) { printf("ramcc: could not start c4sp\n"); return 1; }
	await_pid(task);
	len = 0;
	if (!vfs_get("ramcc.c4r", &len)) { printf("ramcc: no image produced\n"); return 1; }
	printf("ramcc: compiled image is %d bytes of RAM filesystem, running it:\n", len);
	cargv[0] = "ramcc.c4r";
	cargv[1] = 0;
	task = kern_user_start_c4r(1, cargv, "ramcc", PRIV_USER);
	if (task <= 0) { printf("ramcc: could not run it\n"); return 1; }
	await_pid(task);
	printf("ramcc: done\n");
	return 0;
}
