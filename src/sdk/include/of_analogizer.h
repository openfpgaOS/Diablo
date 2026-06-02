//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_analogizer.h -- Analogizer API for openfpgaOS
 */

#ifndef OF_ANALOGIZER_H
#define OF_ANALOGIZER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct {
    uint8_t  enabled;
    uint8_t  video_mode;
    uint8_t  snac_type;
    uint8_t  snac_assignment;
    int8_t   h_offset;
    int8_t   v_offset;
} of_analogizer_state_t;

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"

static inline int of_analogizer_enabled(void) {
    return of_sbi_ret_int(of_ecall0(OF_EID_ANALOGIZER,
                                    OF_ANALOGIZER_FID_IS_ENABLED));
}

static inline int of_analogizer_state(of_analogizer_state_t *state) {
    return of_sbi_ret_int(of_ecall1(OF_EID_ANALOGIZER,
                                    OF_ANALOGIZER_FID_GET_STATE,
                                    (long)state));
}

#else /* OF_PC */

int of_analogizer_enabled(void);
int of_analogizer_state(of_analogizer_state_t *state);

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_ANALOGIZER_H */
