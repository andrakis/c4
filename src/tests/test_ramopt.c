// C4KE Test: the optimizer inside the OS. c4sp (the Lisp interpreter)
// runs under C4KE, optimizes a host .c4r with c4opt.lisp, writes the
// optimized image to the kernel RAM filesystem, and the kernel executes
// it from memory.
int main (int argc, char **argv) {
	char **cargv;
	int task, len;

	if (!(cargv = malloc(6 * sizeof(char *)))) return 1;
	cargv[0] = "c4sp.c4r";
	cargv[1] = "src/c4sp/lisp/c4opt-run.lisp";
	cargv[2] = "hello.c4r";
	cargv[3] = "ramopt.c4r";
	cargv[4] = 0;
	printf("ramopt: optimizing hello.c4r inside C4KE...\n");
	task = kern_user_start_c4r(4, cargv, "c4sp", PRIV_USER);
	if (task <= 0) { printf("ramopt: could not start c4sp\n"); return 1; }
	await_pid(task);
	len = 0;
	if (!vfs_get("ramopt.c4r", &len)) { printf("ramopt: no image produced\n"); return 1; }
	printf("ramopt: optimized image is %d bytes of RAM filesystem, running it:\n", len);
	cargv[0] = "ramopt.c4r";
	cargv[1] = 0;
	task = kern_user_start_c4r(1, cargv, "ramopt", PRIV_USER);
	if (task <= 0) { printf("ramopt: could not run it\n"); return 1; }
	await_pid(task);
	printf("ramopt: done\n");
	return 0;
}
