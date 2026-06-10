//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

#include "test.h"

/* System register block at 0x40000000; shutdown status/ack at offset 0xB0 */
#define SYSREG_BASE     0x40000000
#define SYSREG_SHUTDOWN (SYSREG_BASE + 0xB0)

void test_shutdown(void) {
    section_start("Shutdown");

    volatile uint32_t *shutdown_reg = (volatile uint32_t *)SYSREG_SHUTDOWN;
    uint32_t val = *shutdown_reg;
    ASSERT("not pending", (val & 1) == 0);

    *shutdown_reg = 1;
    test_pass("ack write");

    section_end();
}
