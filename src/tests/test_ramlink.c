// C4KE Test: linking inside the machine.
//
// test_selfhost proves ONE module can be compiled and run from memory.
// This proves the other half: several objects, LINKED, in memory. That
// is what C4IX needs -- it is built from twelve .c4o files plus a
// library archive, and until now c4rlink refused to write under the VM
// at all ("writing output is not supported under c4") and could not
// see a .c4o that existed only in the RAM filesystem.
//
// Compile two modules that reference each other's symbols, link them,
// run the result. If the link is real, b_add and b_message resolve and
// the program prints what test-link prints on the host.
int main (int argc, char **argv) {
	char **cargv;
	int task, len;

	if (!(cargv = malloc(6 * sizeof(char *)))) return 1;

	cargv[0] = "c4cc.c4r";
	cargv[1] = "-o";
	cargv[2] = "tla.c4o";
	cargv[3] = "src/tests/test_link_a.c";
	cargv[4] = 0;
	printf("ramlink: compiling module a...\n");
	task = kern_user_start_c4r(4, cargv, "c4cc", PRIV_USER);
	if (task <= 0) { printf("ramlink: could not start c4cc\n"); return 1; }
	await_pid(task);

	cargv[2] = "tlb.c4o";
	cargv[3] = "src/tests/test_link_b.c";
	printf("ramlink: compiling module b...\n");
	task = kern_user_start_c4r(4, cargv, "c4cc", PRIV_USER);
	if (task <= 0) { printf("ramlink: could not start c4cc\n"); return 1; }
	await_pid(task);

	len = 0;
	if (!vfs_get("tla.c4o", &len) || !len) { printf("ramlink: module a was not written\n"); return 1; }
	len = 0;
	if (!vfs_get("tlb.c4o", &len) || !len) { printf("ramlink: module b was not written\n"); return 1; }

	// The link. Both inputs exist ONLY in the RAM filesystem, and the
	// output can only go there too.
	cargv[0] = "c4rlink.c4r";
	cargv[1] = "tla.c4o";
	cargv[2] = "tlb.c4o";
	cargv[3] = "-o";
	cargv[4] = "ramlinked.c4r";
	cargv[5] = 0;
	printf("ramlink: linking...\n");
	task = kern_user_start_c4r(5, cargv, "c4rlink", PRIV_USER);
	if (task <= 0) { printf("ramlink: could not start c4rlink\n"); return 1; }
	await_pid(task);

	len = 0;
	if (!vfs_get("ramlinked.c4r", &len)) { printf("ramlink: no image produced\n"); return 1; }
	printf("ramlink: linked image is %d bytes, running it:\n", len);

	cargv[0] = "ramlinked.c4r";
	cargv[1] = 0;
	task = kern_user_start_c4r(1, cargv, "ramlinked", PRIV_USER);
	if (task <= 0) { printf("ramlink: could not run the image\n"); return 1; }
	await_pid(task);
	printf("ramlink: done\n");
	return 0;
}
