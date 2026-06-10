//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

#include "test.h"

void test_printf(void) {
    section_start("Printf");
    char buf[64];
    ASSERT_EQ("len", snprintf(buf, sizeof(buf), "hello %d", 42), 8);
    ASSERT("str", strcmp(buf, "hello 42") == 0);
    sprintf(buf, "%x", 0xDEAD);
    ASSERT("hex", strcmp(buf, "dead") == 0);
    snprintf(buf, sizeof(buf), "%05d", 42);
    ASSERT("pad", strcmp(buf, "00042") == 0);
    section_end();
}

void test_printf_edge(void) {
    section_start("Printf Edge");

    char buf[128];

    snprintf(buf, sizeof(buf), "%d", 2147483647);
    ASSERT("int max", strcmp(buf, "2147483647") == 0);

    snprintf(buf, sizeof(buf), "%d", -2147483647);
    ASSERT("int min", strcmp(buf, "-2147483647") == 0);

    snprintf(buf, sizeof(buf), "%u", 4294967295u);
    ASSERT("uint max", strcmp(buf, "4294967295") == 0);

    snprintf(buf, sizeof(buf), "%08X", 0xDEADBEEF);
    ASSERT("hex08", strcmp(buf, "DEADBEEF") == 0);

    /* Use volatile pointers so GCC can't constant-fold the format string
     * and trigger -Wformat-zero-length / -Wformat-truncation on these
     * intentional edge-case tests. */
    const char *volatile empty_fmt = "";
    const char *volatile trunc_fmt = "hello world";
    snprintf(buf, sizeof(buf), empty_fmt);
    ASSERT("empty fmt", buf[0] == '\0');

    int n = snprintf(buf, 5, trunc_fmt);
    ASSERT("trunc len", n == 11);
    ASSERT("trunc str", strcmp(buf, "hell") == 0);

    snprintf(buf, sizeof(buf), "100%%");
    ASSERT("percent", strcmp(buf, "100%") == 0);

    section_end();
}
