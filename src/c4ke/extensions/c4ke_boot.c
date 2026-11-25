// C4KE extension: boot another kernel, without a reset.
//
// The ladder's last step on a machine with no BIOS. C4DOS builds C4KE
// and hands over to it with dosload: load the image, give memory back,
// jump. C4KE then builds C4IX (b4ke -f c4ix.b4k), and on the breadboard
// the way up from there is a medium and a reboot (kinstall C:, reboot
// 1). libjs has no BIOS to reboot into, and the climb should not need
// one: this is dosload one rung up.
//
//   kboot c4ix.c4r [arguments...]
//
// bin/kboot.c raises OP_KBOOT. The handler checks the image is there
// (the RAM filesystem first, then the host, as task_loadc4r looks),
// keeps its command line, and ends init, which is what stops C4KE: its
// shutdown runs as for any other init that exits, every task and
// handler goes, the timer stops. Then main's last act is
// kernel_handoff (c4ke.c), which is boot_handoff below: it loads the
// image, gives the RAM filesystem back, and runs it as C4KE was run,
// constructors, main and destructors. When that kernel shuts down,
// C4KE returns too, to what loaded it: under dosload, the A> prompt.

#ifndef __C4KE_BOOT_C
#define __C4KE_BOOT_C 1

#include <c4ke/extension.h>

enum { BOOT_OP = 246 };          // below the mailbox's 248-253 (c4ke_mbox.c)
enum { BOOT_ARGS = 16, BOOT_BUF = 1024 };

static int    boot_argc;
static char **boot_argv;         // BOOT_ARGS pointers into boot_buf
static char  *boot_buf;

// Is `name` there to load? The RAM filesystem, with and without .c4r,
// then the host.
static int boot_found (char *name) {
	int fd, len;
	if (ramfs_get(name, &len)) return 1;
	fd = open(name, 0);
	if (fd >= 0) { close(fd); return 1; }
	return 0;
}

// Give the RAM filesystem back: the kernel booted next does not read it,
// and it is the biggest thing C4KE still holds.
static void boot_ramfs_free () {
	int i;
	if (!ramfs_names) return;
	i = 0;
	while (i < ramfs_nfiles) { free(ramfs_names[i]); free(ramfs_bufs[i]); ++i; }
	free(ramfs_names); free(ramfs_bufs); free(ramfs_lens);
	ramfs_names = 0; ramfs_bufs = 0; ramfs_lens = 0;
	ramfs_nfiles = 0;
}

// kernel_handoff: C4KE has stopped. Load the image, run it, come back.
static int boot_handoff (int status) {
	int *module, len, r;
	char *data, *name;

	name = boot_argv[0];
	module = 0;
	if ((data = ramfs_get(name, &len)))
		module = c4r_load_mem(name, data, len, kernel_loadc4r_mode | C4ROPT_RUN);
	else
		module = c4r_load_opt(name, kernel_loadc4r_mode | C4ROPT_RUN);
	if (!module || !module[C4R_LOADCOMPLETE]) {
		printf("kboot: could not load %s\n", name);
		return status;
	}
	boot_ramfs_free();
	printf("c4ke: booting %s\n\n", name);
	r = loadc4r_execute(module, boot_argc, boot_argv);
	c4r_free(module);
	printf("c4ke: %s returned %d\n", name, r);
	return r;
}

// OP_KBOOT: sp[1] = argc, sp[2] = argv, the image's own name first.
// Returns 0, or -1 when the image is not there or the line is too long.
static void op_kboot (int trap, int ins, int mode, int a, int *bp, int *sp, int *returnpc) {
	int argc, i, n;
	char **argv, *s, *d;

	argc = sp[1];
	argv = (char **)sp[2];
	a = -1;
	if (!boot_buf || argc < 1 || argc > BOOT_ARGS) {
		printf("kboot: give an image and at most %d arguments\n", BOOT_ARGS - 1);
	} else if (!boot_found(argv[0])) {
		printf("kboot: %s is not in the RAM filesystem or on the disk\n", argv[0]);
	} else {
		d = boot_buf;
		n = 0;
		i = 0;
		while (i < argc && n >= 0) {
			boot_argv[i] = d;
			s = argv[i];
			while (*s && n < BOOT_BUF - 1) { *d++ = *s++; ++n; }
			if (*s) n = -1;
			else { *d++ = 0; ++n; }
			++i;
		}
		if (n < 0) {
			printf("kboot: the command line is longer than %d bytes\n", BOOT_BUF);
		} else {
			boot_argc = argc;
			kernel_handoff = (int *)&boot_handoff;
			printf("kboot: C4KE stops, then %s boots\n", boot_argv[0]);
			// Ending init is what stops the kernel (main's await_pid).
			// Quietly: SIGKILL's own path prints the task table.
			kernel_task_finish(kernel_init_task);
			kernel_init_task[TASK_STATE] = STATE_ZOMBIE;
			++kernel_tasks_zombie;
			a = 0;
		}
	}
	trap_exit();
}

static int boot_init () {
	kext_register_symbol("OP_KBOOT", BOOT_OP);
	return KXERR_NONE;
}

static int boot_start () {
	if (!(boot_buf = malloc(BOOT_BUF)) || !(boot_argv = (char **)malloc(BOOT_ARGS * sizeof(char *))))
		return KXERR_FAIL;
	install_custom_opcode(BOOT_OP, (int *)&op_kboot);
	return KXERR_NONE;
}

static int boot_shutdown () { return KXERR_NONE; }

static int __attribute__((constructor)) boot_constructor () {
	kext_register("boot", (int *)&boot_init, (int *)&boot_start, (int *)&boot_shutdown);
}

#endif // ifndef __C4KE_BOOT_C
