//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

#include "test.h"

#define LZW_TEST_SIZE 256
static uint8_t lzw_orig[LZW_TEST_SIZE];
static uint8_t lzw_comp[LZW_TEST_SIZE + (LZW_TEST_SIZE >> 4) + 16];
static uint8_t lzw_decomp[LZW_TEST_SIZE];

void test_lzw(void) {
    section_start("LZW");

    /* Round-trip: compressible pattern */
    for (int i = 0; i < LZW_TEST_SIZE; i++)
        lzw_orig[i] = (uint8_t)(i & 0x1F);

    int32_t comp_len = of_lzw_compress(lzw_orig, LZW_TEST_SIZE, lzw_comp);
    ASSERT("compress ok", comp_len > 0);
    ASSERT("compress smaller", comp_len < LZW_TEST_SIZE);

    if (comp_len > 0) {
        int32_t decomp_len = of_lzw_uncompress(lzw_comp, comp_len, lzw_decomp);
        ASSERT_EQ("decomp len", decomp_len, LZW_TEST_SIZE);
        ASSERT("round-trip", memcmp(lzw_orig, lzw_decomp, LZW_TEST_SIZE) == 0);
    }

    /* All-zeros: should compress well */
    memset(lzw_orig, 0, LZW_TEST_SIZE);
    comp_len = of_lzw_compress(lzw_orig, LZW_TEST_SIZE, lzw_comp);
    ASSERT("zeros ok", comp_len > 0);

    if (comp_len > 0) {
        int32_t decomp_len = of_lzw_uncompress(lzw_comp, comp_len, lzw_decomp);
        ASSERT_EQ("zeros len", decomp_len, LZW_TEST_SIZE);
        ASSERT("zeros match", memcmp(lzw_orig, lzw_decomp, LZW_TEST_SIZE) == 0);
    }

    /* Random-ish data: must still round-trip */
    {
        uint32_t seed = 0xDEADBEEF;
        for (int i = 0; i < LZW_TEST_SIZE; i++) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            lzw_orig[i] = (uint8_t)(seed & 0xFF);
        }
    }
    comp_len = of_lzw_compress(lzw_orig, LZW_TEST_SIZE, lzw_comp);
    ASSERT("rand ok", comp_len > 0);

    if (comp_len > 0) {
        int32_t decomp_len = of_lzw_uncompress(lzw_comp, comp_len, lzw_decomp);
        ASSERT_EQ("rand len", decomp_len, LZW_TEST_SIZE);
        ASSERT("rand match", memcmp(lzw_orig, lzw_decomp, LZW_TEST_SIZE) == 0);
    }

    section_end();
}
