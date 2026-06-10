//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

#include "test.h"

void test_memset_stack(void) {
    section_start("Memset Stack");

    char buf[2048];

    memset(buf, 0, sizeof(buf));
    ASSERT("zero[0]", buf[0] == 0);
    ASSERT("zero[2047]", buf[2047] == 0);

    memset(buf, 0xAA, sizeof(buf));
    ASSERT("fill[0]", (unsigned char)buf[0] == 0xAA);
    ASSERT("fill[1023]", (unsigned char)buf[1023] == 0xAA);
    ASSERT("fill[2047]", (unsigned char)buf[2047] == 0xAA);

    section_end();
}
