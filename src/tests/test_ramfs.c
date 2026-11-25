// C4KE Test: the kernel RAM filesystem (OP_VFS_*).
// Writes a file, reads it back, replaces it, lists, unlinks.
int main (int argc, char **argv) {
	char *buf, *got;
	int len, i;

	buf = "hello from the ram filesystem";
	if (vfs_put("test.txt", buf, 29)) { printf("ramfs: put failed\n"); return 1; }
	len = 0;
	if (!(got = vfs_get("test.txt", &len))) { printf("ramfs: get failed\n"); return 1; }
	printf("ramfs: got %d bytes: %s\n", len, got);
	if (vfs_put("test.txt", "shorter", 7)) { printf("ramfs: replace failed\n"); return 1; }
	got = vfs_get("test.txt", &len);
	printf("ramfs: now %d bytes: %s\n", len, got);
	vfs_put("second.txt", "two", 3);
	i = 0;
	while (i < vfs_count()) {
		printf("ramfs: file %d: %s\n", i, vfs_name(i));
		i = i + 1;
	}
	if (vfs_unlink("test.txt")) { printf("ramfs: unlink failed\n"); return 1; }
	printf("ramfs: %d file(s) after unlink\n", vfs_count());
	if (vfs_get("test.txt", &len)) { printf("ramfs: still there!\n"); return 1; }
	printf("ramfs: ok\n");
	return 0;
}
