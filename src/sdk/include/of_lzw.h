//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_lzw.h -- LZW Compression API for openfpgaOS
 *
 * Build engine compatible LZW compress/uncompress.
 */

#ifndef OF_LZW_H
#define OF_LZW_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

static inline int32_t of_lzw_compress(const uint8_t *in, int32_t in_len,
                                      uint8_t *out) {
    return of_sbi_ret_i32(of_ecall3(OF_EID_LZW, OF_LZW_FID_COMPRESS,
                                    (long)in, in_len, (long)out));
}

static inline int32_t of_lzw_uncompress(const uint8_t *in, int32_t comp_len,
                                        uint8_t *out) {
    return of_sbi_ret_i32(of_ecall3(OF_EID_LZW, OF_LZW_FID_UNCOMPRESS,
                                    (long)in, comp_len, (long)out));
}

#else /* OF_PC */

static inline int32_t of_lzw_compress(const uint8_t *in, int32_t in_len,
                                      uint8_t *out) {
    (void)in; (void)in_len; (void)out;
    return -1;
}

static inline int32_t of_lzw_uncompress(const uint8_t *in, int32_t comp_len,
                                        uint8_t *out) {
    (void)in; (void)comp_len; (void)out;
    return -1;
}

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_LZW_H */
