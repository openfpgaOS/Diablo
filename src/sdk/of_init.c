//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_init.c -- SDK runtime init
 *
 * Auto-linked into every openfpgaOS app via sdk.mk. Anything that
 * needs to run unconditionally before main() lives here.
 *
 * Responsibilities:
 *
 *   1. Capture the openfpgaOS capability descriptor and services-table
 *      pointers from the kernel-supplied auxv vector. The kernel ELF
 *      loader pushes them as AT_OF_CAPS / AT_OF_SVC; we look them up
 *      via musl's getauxval() and stash them in the globals that
 *      of_caps.h / of_services.h expose to the rest of the SDK.
 *      This replaces the previous mechanism of hardcoding 0x7800 /
 *      0x7A00 BRAM addresses in the API headers, so the same app .elf
 *      can run on any target whose kernel honors the boot ABI.
 *
 *   2. Force stdout to unbuffered. musl's default leaves stdout fully
 *      buffered (BUFSIZ ~= 4KB) when isatty() fails, and even with the
 *      OS's ioctl stub it stays line buffered -- neither is what apps
 *      drawing direct cursor-positioned UI want, since printf calls
 *      without a trailing '\n' never reach the terminal until the
 *      buffer fills.
 *
 * Both are wired through __attribute__((constructor)) at priority 101,
 * which guarantees they run before any user-defined constructor (the
 * default priority bucket starts at 65535) and before main(). The caps
 * lookup must run first because other SDK code may legitimately use
 * of_get_caps() / OF_SVC from a higher-priority constructor.
 */

#include <stdio.h>
#include <sys/auxv.h>

#include "of_app_abi.h"
#include "of_caps.h"
#include "of_input_types.h"
#include "of_services.h"

/* Definitions for the externs declared in of_caps.h / of_services.h.
 * Initially NULL; populated by the constructor below. Reading either
 * before the constructor runs is a bug -- the constructor priority
 * (101) ensures it runs before any other openfpgaOS code. */
const struct of_capabilities *_of_caps_ptr = NULL;
const struct of_services_table *_of_svc_ptr = NULL;
of_input_state_t _of_input_p0;
of_input_state_t _of_input_p1;

__attribute__((constructor(101)))
static void of_runtime_init(void) {
    /* musl's __init_libc has already populated libc.auxv from the
     * stack vector the kernel pushed in elf_exec(). getauxval() walks
     * that array and returns 0 for missing entries -- which would
     * indicate a kernel/SDK ABI mismatch, but we deliberately do not
     * crash here so an app can still attempt to limp along on broken
     * boots. The first OF_SVC->... call will fault clearly enough. */
    _of_caps_ptr = (const struct of_capabilities *)getauxval(AT_OF_CAPS);
    _of_svc_ptr  = (const struct of_services_table *)getauxval(AT_OF_SVC);

    setbuf(stdout, NULL);
}
