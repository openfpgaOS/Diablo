//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

#include "test.h"

void test_string(void) {
    section_start("String");
    ASSERT_EQ("strlen", (int)strlen("hello"), 5);
    ASSERT_EQ("strcmp", strcmp("abc", "abc"), 0);
    ASSERT("strchr", strchr("hello", 'l') != NULL);
    ASSERT("strstr", strstr("hello world", "world") != NULL);
    char d[32] = "foo"; strcat(d, "bar");
    ASSERT("strcat", strcmp(d, "foobar") == 0);
    ASSERT("memcmp", memcmp("abc", "abc", 3) == 0);
    char m[8]; memset(m, 0x42, 8);
    ASSERT("memset", m[0] == 0x42 && m[7] == 0x42);
    section_end();
}

void test_string_edge(void) {
    section_start("String Edge");

    ASSERT_EQ("strlen 0", (int)strlen(""), 0);

    ASSERT("cmp diff", strcmp("abc", "abcd") < 0);
    ASSERT("cmp diff2", strcmp("abcd", "abc") > 0);

    char over[16] = "0123456789";
    memmove(over + 2, over, 8);
    ASSERT("memmove", over[2] == '0' && over[9] == '7');

    ASSERT("strchr miss", strchr("hello", 'z') == NULL);

    ASSERT("strstr miss", strstr("hello", "xyz") == NULL);

    ASSERT("strstr empty", strstr("hello", "") != NULL);

    section_end();
}
