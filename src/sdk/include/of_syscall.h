//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_syscall.h -- openfpgaOS syscall ABI (SBI-style)
 *
 * Modeled on the RISC-V Supervisor Binary Interface (SBI) specification:
 *   https://github.com/riscv-non-isa/riscv-sbi-doc
 *
 * ============================================================================
 * Calling convention
 * ============================================================================
 *
 *   On ecall:
 *     a7 = EID  (Extension ID -- the subsystem, e.g. OF_EID_MIXER)
 *     a6 = FID  (Function ID  -- the call within the subsystem)
 *     a0..a5    six 32-bit argument slots
 *
 *   On return:
 *     a0 = sbiret.error  (0 = success, < 0 = error code from of_error.h)
 *     a1 = sbiret.value  (the actual return value, when no error)
 *
 * Linux-compatible syscalls (used by musl/POSIX) keep their historic
 * convention -- a7 = syscall number, return value in a0 alone -- and
 * are routed by the kernel based on EID range:
 *
 *     EID < OF_EID_BASE  ->  Linux compat (a6 ignored, return in a0)
 *     EID >= OF_EID_BASE ->  openfpgaOS SBI vendor extension
 *
 * The vendor base 0xC0DE0000 has the high bit set, so it cannot collide
 * with the Linux RISC-V syscall numbering (which tops out around 450)
 * or with future standard SBI extensions (assigned by the SBI working
 * group in the low 0x10..0x2F range).
 *
 * ============================================================================
 * Adding a new openfpgaOS syscall
 * ============================================================================
 *
 *   1. Pick the right EID in of_syscall_numbers.h (or add a new one).
 *   2. Append a FID to the corresponding `enum of_<subsys>_fid` -- never
 *      reuse retired numbers, always grow at the end.
 *   3. Add a case to the matching `case OF_EID_<SUBSYS>:` in
 *      kernel/syscall.c::syscall_dispatch().
 *   4. Add the inline wrapper in the SDK header using of_ecallN() so
 *      callers don't have to know the EID/FID values.
 *
 * Adding a brand-new subsystem: append the EID at the end of the list,
 * never reuse a retired EID, never insert in the middle.
 *
 * See docs/syscall-abi.md for the complete specification.
 */

#ifndef OF_SYSCALL_H
#define OF_SYSCALL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#ifndef OF_PC

/* SBI return: { error, value } pair, packed into a0/a1 by the kernel.
 * RISC-V ABI returns 2-word structs in a0+a1, so this is zero-cost. */
struct of_sbiret {
    long error;
    long value;
};

/* ----------------------------------------------------------------------------
 * Vendor SBI ecall wrappers -- use these for all openfpgaOS syscalls.
 * ----------------------------------------------------------------------------
 *
 * Each wrapper places the EID in a7, the FID in a6, the call arguments
 * in a0..a5, executes ecall, and returns the {error, value} pair from
 * a0/a1. Inlined in the caller, so cost is one ecall + a few register
 * moves.
 */

static inline struct of_sbiret of_ecall6(long eid, long fid,
                                          long arg0, long arg1, long arg2,
                                          long arg3, long arg4, long arg5) {
    register long a0 __asm__("a0") = arg0;
    register long a1 __asm__("a1") = arg1;
    register long a2 __asm__("a2") = arg2;
    register long a3 __asm__("a3") = arg3;
    register long a4 __asm__("a4") = arg4;
    register long a5 __asm__("a5") = arg5;
    register long a6 __asm__("a6") = fid;
    register long a7 __asm__("a7") = eid;
    __asm__ volatile ("ecall"
        : "+r"(a0), "+r"(a1)
        : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7)
        : "memory");
    return (struct of_sbiret){ .error = a0, .value = a1 };
}

static inline struct of_sbiret of_ecall0(long eid, long fid) {
    return of_ecall6(eid, fid, 0, 0, 0, 0, 0, 0);
}
static inline struct of_sbiret of_ecall1(long eid, long fid, long a0) {
    return of_ecall6(eid, fid, a0, 0, 0, 0, 0, 0);
}
static inline struct of_sbiret of_ecall2(long eid, long fid,
                                          long a0, long a1) {
    return of_ecall6(eid, fid, a0, a1, 0, 0, 0, 0);
}
static inline struct of_sbiret of_ecall3(long eid, long fid,
                                          long a0, long a1, long a2) {
    return of_ecall6(eid, fid, a0, a1, a2, 0, 0, 0);
}
static inline struct of_sbiret of_ecall4(long eid, long fid,
                                          long a0, long a1, long a2, long a3) {
    return of_ecall6(eid, fid, a0, a1, a2, a3, 0, 0);
}
static inline struct of_sbiret of_ecall5(long eid, long fid,
                                          long a0, long a1, long a2,
                                          long a3, long a4) {
    return of_ecall6(eid, fid, a0, a1, a2, a3, a4, 0);
}

static inline int of_sbi_ret_int(struct of_sbiret r) {
    return r.error ? (int)r.error : (int)r.value;
}

static inline int32_t of_sbi_ret_i32(struct of_sbiret r) {
    return r.error ? (int32_t)r.error : (int32_t)r.value;
}

/* ----------------------------------------------------------------------------
 * Linux-compat ecall wrappers (used only by stdlib.h's exit/abort).
 * ----------------------------------------------------------------------------
 *
 * Linux RISC-V convention: a7 = syscall number (no FID), return in a0.
 * The kernel routes EIDs below OF_EID_BASE through its Linux dispatcher.
 *
 * Do NOT call these from new code -- use of_ecallN() with a vendor EID.
 */

static inline long __of_linux_syscall0(long n) {
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0");
    __asm__ volatile("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}
static inline long __of_linux_syscall1(long n, long arg0) {
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = arg0;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}
static inline long __of_linux_syscall3(long n, long arg0, long arg1, long arg2) {
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = arg0;
    register long a1 __asm__("a1") = arg1;
    register long a2 __asm__("a2") = arg2;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2) : "memory");
    return a0;
}
static inline long __of_linux_syscall4(long n, long arg0, long arg1,
                                        long arg2, long arg3) {
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = arg0;
    register long a1 __asm__("a1") = arg1;
    register long a2 __asm__("a2") = arg2;
    register long a3 __asm__("a3") = arg3;
    __asm__ volatile("ecall"
        : "+r"(a0)
        : "r"(a7), "r"(a1), "r"(a2), "r"(a3)
        : "memory");
    return a0;
}

#endif /* !OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_SYSCALL_H */
