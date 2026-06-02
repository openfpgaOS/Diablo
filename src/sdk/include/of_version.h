//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_version.h -- API versioning for openfpgaOS
 */

#ifndef OF_VERSION_H
#define OF_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define OF_API_VERSION_MAJOR  1
#define OF_API_VERSION_MINOR  0
#define OF_API_VERSION_PATCH  0

/* Packed version: major(8).minor(8).patch(8) in bits [23:0] */
#define OF_VERSION(maj, min, pat) \
    (((uint32_t)(maj) << 16) | ((uint32_t)(min) << 8) | (uint32_t)(pat))

#define OF_API_VERSION \
    OF_VERSION(OF_API_VERSION_MAJOR, OF_API_VERSION_MINOR, OF_API_VERSION_PATCH)

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

static inline uint32_t of_get_version(void) {
    return (uint32_t)of_ecall0(OF_EID_BASE, OF_BASE_FID_GET_VERSION).value;
}

#else

static inline uint32_t of_get_version(void) {
    return OF_API_VERSION;
}

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_VERSION_H */
