// C4KE Extension: Plus
// Adds support for the advanced features c4plus supports.

#include "c4ke_extension.h"

static int plus_enabled;

static int plus_init () {
	if (kernel_verbosity >= VERB_MAX)
		printf("c4ke: plus module error: loaded, but interpreter doesn't provide plus features.\n");
	plus_enabled = 0;
	return KXERR_NONE; // KXERR_FAIL;
}

static int plus_start () {
	if (kernel_verbosity >= VERB_MED) {
		if (plus_enabled)
			printf("c4ke: plus services enabled\n");
		else
			printf("c4ke: plus services unavailable (requires c4plus)\n");
	}
	return KXERR_NONE;
}

static int plus_shutdown () {
	if (kernel_verbosity >= VERB_MAX)
		printf("c4ke: plus module shutdown\n");
	return KXERR_NONE;
}

static int __attribute__((constructor)) plus_constructor () {
	kext_register("plus", (int *)&plus_init, (int *)&plus_start, (int *)&plus_shutdown);
}
