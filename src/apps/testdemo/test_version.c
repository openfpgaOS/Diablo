//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

#include "test.h"

void test_version(void) {
    section_start("Version");
    uint32_t ver = of_get_version();
    ASSERT("nonzero", ver != 0);
    section_end();
}
