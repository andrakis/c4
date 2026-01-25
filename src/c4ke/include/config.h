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
 *   - Protected mode proved very unstable, and very slow.
 *   - User-mode services such as memory allocation, IO, etc, will be
 *    implemented in u0.h
 */
#define CONFIG_ENABLE_PM 0

#endif
