// C4KE extension: the C4DOS handover.
//
// C4DOS can build a kernel. Until now it could not hand one over: the
// player types BUILD, cpp and c4cc produce c4ke.c4r and init.c4r on
// the DOS RAM disk, RUN starts the kernel -- and the kernel boots the
// PREBUILT init off the floppy, because its loader only knows the host
// filesystem. Everything the machine just compiled is invisible to it.
//
// This closes that. C4DOS already writes the address of its API table
// into any transient that exports __c4dos_api (c4dos.c's inject_api),
// so a kernel loaded by DOS is holding a live handle to DOS's own
// routines. At KEXT_START -- after the OP_VFS_* opcodes exist and
// before the init task is created -- this walks the RAM disk and
// ramfs_put()s every entry. task_loadc4r already checks the RAM
// filesystem BEFORE the host for every program including init, so from
// that moment the freshly built init is what runs.
//
// It is the initrd handover, in the same shape as every other one: the
// thing that loaded you hands you a filesystem, you copy it in, it goes
// away.
//
// The extension is INERT unless C4DOS loaded us. Under gcc, under c4m
// straight off the host, under c4bb booting the floppy -- __c4dos_api
// is 0 and start() returns immediately.

#ifndef __C4KE_DOS_C
#define __C4KE_DOS_C 1

#include <c4ke/extension.h>

// The API table C4DOS wrote into us. NOT static, and NOT renamed:
// inject_api scans the image's EXPORTED symbol section for this exact
// name and requires class Glo (131). A static here would be silently
// skipped and the whole path would look like "no DOS present".
int *__c4dos_api;

// The table's layout. Spelled out rather than #included from
// include/c4dos.h, because that header is strict-c4 and carries the
// transient-side invoke stub -- a function called with varying
// argument counts, which gcc will not compile. The kernel only needs
// the numbers.
enum {
	DOSAPI_MAGIC   = 0,   // ('C' << 16) + ('4' << 8) + 'D'
	DOSAPI_VERSION = 5,
	DOSAPI_COUNT   = 9,   // count() -> entries
	DOSAPI_ENTNAME = 10,  // entname(i) -> char *
	DOSAPI_ENTSIZE = 11,  // entsize(i) -> bytes
	DOSAPI_ENTDATA = 12,  // entdata(i) -> char *
	DOSAPI_TRIM    = 13,  // trim() -> bytes released
	DOSAPI_RELEASE = 14   // release() -> bytes released
};
enum { DOSAPI_MAGIC_VALUE = 4404292 };   // 'C','4','D' packed, as c4dos.c builds it
enum { DOSAPI_MIN_VERSION = 2 };         // enumeration arrived in v2

// Calling through an int * needs c4 syntax, which gcc rejects; the
// macro is named after the VARIABLE so the same line means an indirect
// call under both. load-c4r.c:997-1019 is the pattern, and the proof
// it works under c4cc, c4lc and gcc alike. The pointer is a LOCAL, so
// c4cc emits JSRS -- already in C4KE's opcode set -- rather than the
// JSRI a global would need.
#ifndef __c4cc__
#define dosfn0()  ((int (*)())dosfn0)()
#define dosfn1(a) ((int (*)(int))dosfn1)(a)
#endif

static int dos_kext_call0 (int *dosfn0) { return dosfn0(); }
static int dos_kext_call1 (int *dosfn1, int a) { return dosfn1(a); }

// 1 when C4DOS loaded us AND its table is one we understand.
static int dos_kext_usable () {
	if (!__c4dos_api)
		return 0;
	if (__c4dos_api[DOSAPI_MAGIC] != DOSAPI_MAGIC_VALUE) {
		printf("c4ke: __c4dos_api set but the magic is wrong (0x%x), ignoring it\n",
		       __c4dos_api[DOSAPI_MAGIC]);
		return 0;
	}
	if (__c4dos_api[DOSAPI_VERSION] < DOSAPI_MIN_VERSION) {
		printf("c4ke: C4DOS API v%d has no enumeration, nothing seeded\n",
		       __c4dos_api[DOSAPI_VERSION]);
		return 0;
	}
	// Slots 9-12 are only advertised when CONFIG.SYS installed a RAM
	// disk. No disk is not an error: a DOS booted without one simply
	// has nothing to hand over.
	return __c4dos_api[DOSAPI_COUNT] != 0;
}

static int dos_kext_init () {
	return KXERR_NONE;
}

static int dos_kext_start () {
	int n, i, len, seeded, failed, freed;
	char *name, *data;

	if (!dos_kext_usable())
		return KXERR_NONE;

	n = dos_kext_call0((int *)__c4dos_api[DOSAPI_COUNT]);
	if (!n) {
		if (kernel_verbosity >= VERB_MED)
			printf("c4ke: C4DOS RAM disk is empty, nothing seeded\n");
		return KXERR_NONE;
	}

	seeded = 0;
	failed = 0;
	i = 0;
	while (i < n) {
		name = (char *)dos_kext_call1((int *)__c4dos_api[DOSAPI_ENTNAME], i);
		len  =         dos_kext_call1((int *)__c4dos_api[DOSAPI_ENTSIZE], i);
		data = (char *)dos_kext_call1((int *)__c4dos_api[DOSAPI_ENTDATA], i);
		// ramfs_put COPIES, so DOS's buffer can be passed straight in
		// and released below. Peak cost is 2x the RAM disk, briefly.
		if (!name || !data || len < 0 || ramfs_put(name, data, len) < 0) {
			// Name the file. A silent shortfall here surfaces much
			// later as "init not found", which is a miserable way to
			// discover that RAMFS_MAX was reached.
			printf("c4ke: could not seed '%s' (%d bytes)\n", name ? name : "?", len);
			++failed;
		} else {
			++seeded;
			if (kernel_verbosity >= VERB_MAX)
				printf("c4ke:   seeded %s, %d bytes\n", name, len);
		}
		++i;
	}
	printf("c4ke: seeded %d/%d file(s) from the C4DOS RAM disk\n", seeded, n);

	// Take the memory back. DOS is not going to run again -- we have
	// the machine now -- and a kernel about to compile C4IX wants every
	// byte. RELEASE first: every pointer above is dead after it.
	if (__c4dos_api[DOSAPI_RELEASE]) {
		freed = dos_kext_call0((int *)__c4dos_api[DOSAPI_RELEASE]);
		if (kernel_verbosity >= VERB_MED)
			printf("c4ke: reclaimed %d bytes of C4DOS RAM disk\n", freed);
	}
	if (__c4dos_api[DOSAPI_TRIM]) {
		freed = dos_kext_call0((int *)__c4dos_api[DOSAPI_TRIM]);
		if (kernel_verbosity >= VERB_MED)
			printf("c4ke: reclaimed %d bytes of C4DOS scratch\n", freed);
	}

	return failed ? KXERR_FAIL : KXERR_NONE;
}

static int dos_kext_shutdown () {
	return KXERR_NONE;
}

static int __attribute__((constructor)) dos_kext_constructor () {
	kext_register("dos", (int *)&dos_kext_init, (int *)&dos_kext_start,
	              (int *)&dos_kext_shutdown);
}

#endif // ifndef __C4KE_DOS_C
