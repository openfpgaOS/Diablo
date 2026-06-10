//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

#include "test.h"

void test_interact(void) {
    section_start("Interact");

    uint32_t v0 = of_interact_get(0);
    uint32_t v1 = of_interact_get(1);
    uint32_t v2 = of_interact_get(2);
    test_pass("read[0-2]");

    ASSERT_EQ("oob", (int)of_interact_get(999), 0);

    (void)v0; (void)v1; (void)v2;

    section_end();
}
