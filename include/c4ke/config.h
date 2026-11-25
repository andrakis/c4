/**
 * c4ke/include/config.h: Compile-time configuration of the kernel.
 *
 * One day this might be modified by a configuration tool.
 */

#ifndef __C4KE_CONFIG_H
#define __C4KE_CONFIG_H 1

/**
 * Option: Kernel Events
 * Notes:
 *  - Precursor to protected mode, obsolete.
 */
#define CONFIG_KEVENTS 0

/**
 * Option: Enable Protected Mode
 * Requires: c4m compiled with protected mode.
 * Notes:
 *   - Protected mode was long shelved as unstable and slow. As of
 *     docs/task-memory.md it is neither refused nor the default: with
 *     it on, a task's malloc and free reach the kernel, which is the
 *     only way the kernel can give a task's memory back when the task
 *     ends -- see kernel_task_malloc in c4ke.c. `make test-task-mem`
 *     pins that. It costs a trap per syscall.
 *   - Overridable from the command line, which is how the PM kernels
 *     the test builds are made:  $(PREPROC) -DCONFIG_ENABLE_PM=1 ...
 */
#ifndef CONFIG_ENABLE_PM
#define CONFIG_ENABLE_PM 0
#endif

#endif
