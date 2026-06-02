//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_timer.h -- Hardware timer interrupt API for openfpgaOS
 *
 * For time queries use clock_gettime() / clock_ms() / clock_us() from <time.h>
 * For delays use usleep() from <unistd.h>
 */

#ifndef OF_TIMER_H
#define OF_TIMER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#ifndef OF_PC

#include "of_services.h"
#include "of_syscall.h"
#include "of_syscall_numbers.h"

/* Set a periodic timer callback at the given frequency (Hz).
 * The callback runs in interrupt context — keep it short.
 * Pass NULL or hz=0 to disable. */
static inline void of_timer_set_callback(void (*fn)(void), uint32_t hz) {
    OF_SVC->timer_set_callback(fn, hz);
}

/* Stop the periodic timer and clear the callback. */
static inline void of_timer_stop(void) {
    OF_SVC->timer_stop();
}

/* Free-running monotonic time queries (in microseconds / milliseconds).
 * Wrap around at ~71 minutes / ~49 days respectively. */
static inline unsigned int of_time_us(void) {
    return (unsigned int)of_ecall0(OF_EID_TIMER, OF_TIMER_FID_GET_US).value;
}
static inline unsigned int of_time_ms(void) {
    return (unsigned int)of_ecall0(OF_EID_TIMER, OF_TIMER_FID_GET_MS).value;
}

#else /* OF_PC */

static inline void of_timer_set_callback(void (*fn)(void), uint32_t hz) { (void)fn; (void)hz; }
static inline void of_timer_stop(void) { }
unsigned int of_time_us(void);
unsigned int of_time_ms(void);

#endif /* OF_PC */

#ifdef __cplusplus
}
#endif

#endif /* OF_TIMER_H */
