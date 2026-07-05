//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_launch.h -- in-OS application hand-off (the "menu.elf" launcher API).
 *
 * A launcher app (menu.elf) selects a game instance and asks the OS to swap
 * to it WITHOUT resetting the FPGA.  The kernel tears down the current app's
 * state, re-points the file system at the selected instance, re-reads its
 * os.ini, and execs the chosen ELF.
 *
 * Availability is target-dependent: MiSTer supports the in-OS relaunch; the
 * Pocket selects instances through the Analogue host (a core reload), so
 * of_relaunch() returns OF_ERR_NOT_SUPPORTED there and the caller should fall
 * back to exiting (the host menu) instead.
 *
 *   instance  Instance root, e.g. "/games/Quake".  NULL or "" = image root
 *             (legacy single-game layout).  Ignored on targets without a
 *             writable instance tree.
 *   elf       ELF to launch: a filename ("quake.elf"), a "slot:<N>" data slot,
 *             or NULL/"" to take [os] ELF from the instance's os.ini.
 */

#ifndef OF_LAUNCH_H
#define OF_LAUNCH_H

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

/* Swap to another app in-OS.  Does NOT return on success (the new app runs
 * in this process's place).  Returns a negative of_error.h code on failure
 * (e.g. OF_ERR_NOT_SUPPORTED, or the ELF could not be loaded). */
static inline int of_relaunch(const char *instance, const char *elf) {
    return of_sbi_ret_int(of_ecall2(OF_EID_BASE, OF_BASE_FID_RELAUNCH,
                                    (long)instance, (long)elf));
}

/* Register the launcher to re-enter when the running app exit()s.  With a menu
 * registered, a game's exit() relaunches the menu instead of halting.  Returns
 * 0 on success, negative on failure. */
static inline int of_launch_set_menu(const char *instance, const char *elf) {
    return of_sbi_ret_int(of_ecall2(OF_EID_BASE, OF_BASE_FID_SET_MENU,
                                    (long)instance, (long)elf));
}

/* Enumerate game instances: subdirectories of /games that contain an os.ini.
 * Writes up to `max` names into the flat buffer `names`, one every `stride`
 * bytes (NUL-terminated).  Names that don't fit `stride` are skipped, not
 * truncated.  Returns the number of names written (0..max), or 0 if there is
 * no instance tree (e.g. a single-game image, or the Pocket).  Pass the i-th
 * name to of_relaunch() as the instance argument:
 * of_relaunch("/games/<name>", NULL). */
static inline int of_instance_list(char *names, unsigned stride, unsigned max) {
    return of_sbi_ret_int(of_ecall3(OF_EID_BASE, OF_BASE_FID_LIST_INSTANCES,
                                    (long)names, (long)stride, (long)max));
}

#else /* OF_PC -- desktop shim: no in-OS relaunch */

#include "of_error.h"

static inline int of_relaunch(const char *instance, const char *elf) {
    (void)instance; (void)elf;
    return OF_ERR_NOT_SUPPORTED;
}

static inline int of_launch_set_menu(const char *instance, const char *elf) {
    (void)instance; (void)elf;
    return OF_ERR_NOT_SUPPORTED;
}

static inline int of_instance_list(char *names, unsigned stride, unsigned max) {
    (void)names; (void)stride; (void)max;
    return 0;
}

#endif /* OF_PC */

#endif /* OF_LAUNCH_H */
