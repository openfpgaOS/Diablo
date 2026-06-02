//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_interact.h -- Platform menu/settings API
 *
 * Read option values set by the platform menu (Pocket menu button,
 * MiSTer OSD, etc.). Variables are defined in dist/interact.json
 * and surfaced to the app via the of_interact_get() ecall below.
 *
 * Apps only need to know the variable index (0..OF_INTERACT_MAX_VARS-1).
 * The bridge address that interact.json points each variable at is a
 * Pocket-specific authoring detail; it lives in the per-target schema
 * helper (e.g. targets/pocket/include/pocket_interact_addrs.h) and is
 * intentionally NOT part of the SDK API.
 */

#ifndef OF_INTERACT_H
#define OF_INTERACT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define OF_INTERACT_MAX_VARS    64

/* Read an interact variable by index (0-63).
 * Returns the 32-bit value set by the platform menu. */
#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

static inline uint32_t of_interact_get(int index) {
    return (uint32_t)of_ecall1(OF_EID_INTERACT,
                               OF_INTERACT_FID_GET, index).value;
}

#else /* OF_PC */

static inline uint32_t of_interact_get(int index) {
    (void)index;
    return 0;
}

#endif

#ifdef __cplusplus
}
#endif

#endif /* OF_INTERACT_H */
