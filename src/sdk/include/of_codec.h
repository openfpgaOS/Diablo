//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_codec.h -- Audio Codec API for openfpgaOS
 *
 * Parsers for VOC and WAV audio formats.
 */

#ifndef OF_CODEC_H
#define OF_CODEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct {
    const uint8_t *pcm;
    uint32_t pcm_len;
    uint32_t sample_rate;
    uint8_t  bits_per_sample;
    uint8_t  channels;
} of_codec_result_t;

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

static inline int of_codec_parse_voc(const uint8_t *data, uint32_t size,
                                     of_codec_result_t *out) {
    return of_sbi_ret_int(of_ecall3(OF_EID_CODEC, OF_CODEC_FID_PARSE_VOC,
                                    (long)data, size, (long)out));
}

static inline int of_codec_parse_wav(const uint8_t *data, uint32_t size,
                                     of_codec_result_t *out) {
    return of_sbi_ret_int(of_ecall3(OF_EID_CODEC, OF_CODEC_FID_PARSE_WAV,
                                    (long)data, size, (long)out));
}

#else /* OF_PC */

static inline int of_codec_parse_voc(const uint8_t *data, uint32_t size,
                                     of_codec_result_t *out) {
    (void)data; (void)size; (void)out;
    return -1;
}

static inline int of_codec_parse_wav(const uint8_t *data, uint32_t size,
                                     of_codec_result_t *out) {
    (void)data; (void)size; (void)out;
    return -1;
}

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_CODEC_H */
