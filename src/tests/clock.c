// C4 Test: does the clock move?
//
// Reads __time(), then reads it again until the answer changes or
// 100000 tries have gone by. Under plain c4, c4m.c's clock is the file
// /proc/uptime (c4_time_host), so
//
//     c4 c4m.c clock.c
//
// says whether a kernel or a machine gives c4m.c one it can read: C4IX's
// VFS, and c4bb's and libjs's disk device under C4DOS and C4KE
// (libjs/tests/test-libjs.sh). Without the file c4m.c says "unable to
// open uptime file" and the clock stands at 0.
int main () {
	int t0, t1, n;
	t0 = __time();
	n = 0;
	while ((t1 = __time()) == t0 && n < 100000) ++n;
	if (t1 == t0) {
		printf("clock: stopped at %d\n", t0);
		return 1;
	}
	printf("clock: moved %d ms\n", t1 - t0);
	return 0;
}
