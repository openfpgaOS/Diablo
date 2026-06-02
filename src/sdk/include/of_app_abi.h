//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_app_abi.h -- openfpgaOS app boot ABI
 *
 * Defines the contract between the kernel ELF loader and an SDK app's
 * runtime startup. Apps shouldn't include this directly -- it's the
 * shared definition between of_init.c (SDK side) and loader.c
 * (kernel side).
 *
 * Boot sequence
 * -------------
 * The kernel's elf_exec() builds a standard SysV initial stack
 * (argc / argv / envp / auxv) and jumps to the app's _start. musl's
 * crt1.o reads the stack and calls __libc_start_main, which walks
 * auxv into libc.auxv before invoking constructors.
 *
 * To find the openfpgaOS capability descriptor and services table,
 * the SDK init constructor calls getauxval() on the two vendor tags
 * defined here. This replaces the previous mechanism of reading from
 * fixed BRAM addresses (0x7800 / 0x7A00), which forced every app to
 * compile in those literals and prevented the same .elf from running
 * on a target with a different BRAM layout.
 *
 * The vendor tags use the openfpgaOS namespace prefix 0xC0DE0000,
 * matching the SBI vendor extension range in of_syscall_numbers.h.
 * They are well above any Linux AT_* value (Linux uses 0..51), so
 * a stock musl getauxval() walking the kernel-pushed auxv vector
 * will find them without ambiguity.
 */

#ifndef OF_APP_ABI_H
#define OF_APP_ABI_H

/* Vendor auxv tags. Values must fit in `unsigned long` for getauxval().
 * Picked in the openfpgaOS vendor namespace (0xC0DE0000+) so they cannot
 * collide with current or future Linux AT_* assignments. */
#define AT_OF_CAPS  0xC0DE0001UL  /* pointer to struct of_capabilities */
#define AT_OF_SVC   0xC0DE0002UL  /* pointer to struct of_services_table */

#endif /* OF_APP_ABI_H */
