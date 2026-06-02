//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_mount.h -- Mounted read-only filesystems for openfpgaOS
 *
 * Mounting an ISO exposes it through the normal POSIX APIs:
 *
 *   of_iso_mount("data.iso", "/cd");
 *   FILE *f = fopen("/cd/README.TXT", "rb");
 *   DIR *d = opendir("/cd");
 */

#ifndef OF_MOUNT_H
#define OF_MOUNT_H

#include <stdint.h>

#define OF_MOUNT_RDONLY 1u

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

static inline int of_mount(const char *source, const char *target,
                           const char *fstype, uint32_t flags) {
    struct of_sbiret r = of_ecall4(OF_EID_FILE, OF_FILE_FID_MOUNT,
                                   (long)source, (long)target,
                                   (long)fstype, (long)flags);
    return r.error < 0 ? (int)r.error : 0;
}

static inline int of_umount(const char *target) {
    struct of_sbiret r = of_ecall1(OF_EID_FILE, OF_FILE_FID_UMOUNT,
                                   (long)target);
    return r.error < 0 ? (int)r.error : 0;
}

static inline int of_iso_mount(const char *source, const char *target) {
    return of_mount(source, target, "iso9660", OF_MOUNT_RDONLY);
}

#else

int of_mount(const char *source, const char *target,
             const char *fstype, uint32_t flags);
int of_umount(const char *target);
int of_iso_mount(const char *source, const char *target);

#endif /* OF_PC */

#endif /* OF_MOUNT_H */
