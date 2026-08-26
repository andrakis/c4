// C4KE Test: the C4IX build path, in the machine.
//
// test_ramlink proves c4cc and c4rlink can compile, link and run
// entirely in the RAM filesystem. This proves the OTHER toolchain --
// the one C4IX is actually built with -- can do the same: c4lc,
// running on c4sp, compiles a real C4IX kernel module to an object in
// memory, and c4rlink reads that object BACK out of memory and writes
// a library from it.
//
// One module, not twelve: a module takes about a minute and a half
// under the VM, and what is in question is whether the path works at
// all, not whether it works twelve times. Nothing here boots C4IX --
// its userland links against libc4ix and runs under C4IX, not under
// C4KE.
//
// -c 400000 is the measured arena floor doubled (docs/homeward-ladder.md,
// M5). The 8000000 the build scripts used to pass is ~168MB of cells
// and would not fit in the machine this is meant to run in.
int main (int argc, char **argv) {
	char **cargv;
	int task, len;

	if (!(cargv = malloc(12 * sizeof(char *)))) return 1;

	cargv[0] = "c4sp.c4r";
	cargv[1] = "-c";
	cargv[2] = "400000";
	// -R: the recursive evaluator, which the host build rules have used
	// since A1.2 and this one never did. c4lc uses neither call/cc nor
	// first-class environments, so CEK buys it nothing; on c4bb the
	// same module costs 4.66 G instructions without -R and 1.65 G with
	// it (docs/compiler-on-the-board.md).
	cargv[3] = "-R";
	cargv[4] = "src/c4sp/lisp/c4lc.lisp";
	cargv[5] = "-O";
	cargv[6] = "-c";
	cargv[7] = "-I";
	cargv[8] = "src/c4ix";
	cargv[9] = "src/c4ix/boot.c";
	cargv[10] = "ixboot.c4o";
	cargv[11] = 0;
	printf("ixbuild: c4lc is compiling src/c4ix/boot.c (slow)...\n");
	task = kern_user_start_c4r(11, cargv, "c4sp", PRIV_USER);
	if (task <= 0) { printf("ixbuild: could not start c4sp\n"); return 1; }
	await_pid(task);

	len = 0;
	if (!vfs_get("ixboot.c4o", &len) || !len) {
		printf("ixbuild: no object in the RAM filesystem\n");
		return 1;
	}
	printf("ixbuild: object is %d bytes of RAM filesystem\n", len);

	// Library mode: one C4IX module on its own has unresolved externs
	// by design, and -r is how you say so. What matters is that
	// c4rlink READ an object that exists only in memory and WROTE one
	// back there -- twelve of these plus a link is the C4IX kernel.
	cargv[0] = "c4rlink.c4r";
	cargv[1] = "-r";
	cargv[2] = "ixboot.c4o";
	cargv[3] = "-o";
	cargv[4] = "ixboot.c4l";
	cargv[5] = 0;
	printf("ixbuild: c4rlink is reading it back...\n");
	task = kern_user_start_c4r(5, cargv, "c4rlink", PRIV_USER);
	if (task <= 0) { printf("ixbuild: could not start c4rlink\n"); return 1; }
	await_pid(task);

	len = 0;
	if (!vfs_get("ixboot.c4l", &len) || !len) {
		printf("ixbuild: no library produced\n");
		return 1;
	}
	printf("ixbuild: library is %d bytes, ok\n", len);
	return 0;
}
