// C4KE Test: the self-hosting loop. Compile a program with c4cc running
// under C4KE, write the image to the kernel RAM filesystem (the VM has
// no write syscall, so the host filesystem is unreachable), and execute
// the result straight from memory -- the loader checks the RAM
// filesystem before the host.
int main (int argc, char **argv) {
	char **cargv;
	int task, len;

	if (!(cargv = malloc(5 * sizeof(char *)))) return 1;
	cargv[0] = "c4cc.c4r";
	cargv[1] = "-o";
	cargv[2] = "selfhosted.c4r";
	cargv[3] = "src/tests/hello.c";
	cargv[4] = 0;
	printf("selfhost: compiling %s inside C4KE...\n", cargv[3]);
	task = kern_user_start_c4r(4, cargv, "c4cc", PRIV_USER);
	if (task <= 0) { printf("selfhost: could not start c4cc\n"); return 1; }
	await_pid(task);
	len = 0;
	if (!vfs_get("selfhosted.c4r", &len)) {
		printf("selfhost: no image produced\n");
		return 1;
	}
	printf("selfhost: image is %d bytes of RAM filesystem, running it:\n", len);
	cargv[0] = "selfhosted.c4r";
	cargv[1] = 0;
	task = kern_user_start_c4r(1, cargv, "selfhosted", PRIV_USER);
	if (task <= 0) { printf("selfhost: could not run the image\n"); return 1; }
	await_pid(task);
	printf("selfhost: done\n");
	return 0;
}
