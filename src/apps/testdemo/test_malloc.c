//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

#include "test.h"

void test_malloc(void) {
    section_start("Malloc");

    void *p = malloc(64);
    ASSERT("64B", p != NULL);
    if (p) { memset(p, 0xAA, 64); ASSERT("write", ((unsigned char *)p)[63] == 0xAA); free(p); }

    void *c = calloc(10, 4);
    ASSERT("calloc", c != NULL);
    if (c) { int ok = 1; for (int i = 0; i < 40; i++) if (((unsigned char *)c)[i]) ok = 0; ASSERT("zeroed", ok); free(c); }

    void *r = malloc(16);
    if (r) { memset(r, 0x55, 16); void *r2 = realloc(r, 256); ASSERT("realloc", r2 && ((unsigned char *)r2)[0] == 0x55); if (r2) free(r2); }

    void *p1 = malloc(1024);        ASSERT("1K", p1 != NULL);
    void *p2 = malloc(64 * 1024);   ASSERT("64K", p2 != NULL);
    void *p3 = malloc(1024 * 1024); ASSERT("1M", p3 != NULL);
    void *p4 = malloc(4*1024*1024); ASSERT("4M", p4 != NULL);
    void *p8 = malloc(8*1024*1024); ASSERT("8M", p8 != NULL);
    if (p8) free(p8);
    free(p1); free(p2); free(p3); free(p4);

    void *p5 = malloc(2 * 1024 * 1024);
    ASSERT("2M reuse", p5 != NULL);
    if (p5) { ((unsigned char *)p5)[2*1024*1024-1] = 0xBB; ASSERT("2M write", ((unsigned char *)p5)[2*1024*1024-1] == 0xBB); free(p5); }

    {
        uint8_t mb[32] __attribute__((aligned(8)));
        for (int i = 0; i < 16; i++) mb[i] = (uint8_t)(0x11 * (i + 1));

        ASSERT("lw +1", *(volatile uint32_t *)&mb[1] == 0x55443322);
        ASSERT("lw +2", *(volatile uint32_t *)&mb[2] == 0x66554433);
        ASSERT("lw +3", *(volatile uint32_t *)&mb[3] == 0x77665544);

        ASSERT("lh +1", *(volatile uint16_t *)&mb[1] == 0x3322);
        ASSERT("lh +3", *(volatile uint16_t *)&mb[3] == 0x5544);

        mb[0]=0x11; mb[1]=0x22; mb[2]=0x33; mb[3]=0x44;
        mb[4]=0x55; mb[5]=0x66; mb[6]=0x77; mb[7]=0x88;

        *(volatile uint32_t *)&mb[1] = 0xDEADBEEF;
        ASSERT("sw +1", mb[1]==0xEF && mb[2]==0xBE && mb[3]==0xAD && mb[4]==0xDE);
        ASSERT("sw +1 nocrush", mb[0]==0x11 && mb[5]==0x66);

        mb[0]=0x11; mb[1]=0x22; mb[2]=0x33; mb[3]=0x44;
        mb[4]=0x55; mb[5]=0x66; mb[6]=0x77; mb[7]=0x88;

        *(volatile uint32_t *)&mb[2] = 0xCAFEBABE;
        ASSERT("sw +2", mb[2]==0xBE && mb[3]==0xBA && mb[4]==0xFE && mb[5]==0xCA);
        ASSERT("sw +2 nocrush", mb[1]==0x22 && mb[6]==0x77);

        mb[0]=0x11; mb[1]=0x22; mb[2]=0x33; mb[3]=0x44;
        mb[4]=0x55; mb[5]=0x66; mb[6]=0x77; mb[7]=0x88;

        *(volatile uint32_t *)&mb[3] = 0x12345678;
        ASSERT("sw +3", mb[3]==0x78 && mb[4]==0x56 && mb[5]==0x34 && mb[6]==0x12);
        ASSERT("sw +3 nocrush", mb[2]==0x33 && mb[7]==0x88);

        mb[0]=0x11; mb[1]=0x22; mb[2]=0x33; mb[3]=0x44;

        *(volatile uint16_t *)&mb[1] = 0xABCD;
        ASSERT("sh +1", mb[1]==0xCD && mb[2]==0xAB);
        ASSERT("sh +1 nocrush", mb[0]==0x11 && mb[3]==0x44);

        uint8_t grp[16] __attribute__((aligned(4)));
        memcpy(grp, "FILENAMEXT  ", 12);
        grp[12] = 0x00; grp[13] = 0x10; grp[14] = 0x00; grp[15] = 0x00;
        uint32_t file_size_val = *(volatile uint32_t *)&grp[12];
        ASSERT("grp idx", file_size_val == 0x00001000);
    }

    size_t max_sz = 0;
    for (size_t sz = 1024*1024; sz <= 48*1024*1024; sz += 1024*1024) {
        void *q = malloc(sz); if (!q) break; max_sz = sz; free(q);
    }
    snprintf(__buf, sizeof(__buf), "max %dMB", (int)(max_sz/(1024*1024)));
    test_pass(__buf);

    static const size_t sizes[] = { 32, 64, 128, 256, 512, 1024, 4096, 16384, 65536 };
    int ok = 1;
    for (int i = 0; i < 500; i++) {
        size_t sz = sizes[i % 9];
        void *q = malloc(sz); if (!q) { ok = 0; break; }
        ((unsigned char *)q)[sz-1] = (unsigned char)i; free(q);
    }
    ASSERT("500x stress", ok);

    section_end();
}

void test_malloc_edge(void) {
    section_start("Malloc Edge");

    void *p = malloc(0);
    if (p) free(p);
    test_pass("malloc 0");

    free(NULL);
    test_pass("free NULL");

    p = realloc(NULL, 64);
    ASSERT("realloc NULL", p != NULL);
    if (p) free(p);

    p = malloc(64);
    if (p) {
        void *r = realloc(p, 0);
        if (r) free(r);
        test_pass("realloc 0");
    }

    int align_ok = 1;
    for (int i = 0; i < 20; i++) {
        void *q = malloc(1 + i * 7);
        if (q && ((uintptr_t)q & 7) != 0) align_ok = 0;
        if (q) free(q);
    }
    ASSERT("alignment", align_ok);

    section_end();
}

void test_malloc_free(void) {
    section_start("Malloc Free");

    /* Allocate, free, reallocate — same size */
    void *p1 = malloc(1024);
    ASSERT("alloc 1K", p1 != NULL);
    if (p1) { memset(p1, 0xAA, 1024); free(p1); }

    void *p2 = malloc(1024);
    ASSERT("realloc 1K", p2 != NULL);
    if (p2) { memset(p2, 0xBB, 1024); free(p2); }

    /* Alternating sizes (like tiles.c: short[] then int32[]) */
    {
        int ok = 1;
        for (int i = 0; i < 20; i++) {
            short *sw = (short *)malloc(256 * sizeof(short));
            int32_t *si = (int32_t *)malloc(256 * sizeof(int32_t));
            if (!sw || !si) { ok = 0; free(sw); free(si); break; }
            memset(sw, i, 256 * sizeof(short));
            memset(si, i, 256 * sizeof(int32_t));
            free(si);
            free(sw);
        }
        ASSERT("20x alt sizes", ok);
    }

    /* Large allocation (matches art file tile data ~18KB) */
    {
        void *big = malloc(18000);
        ASSERT("18K", big != NULL);
        if (big) { memset(big, 0xCC, 18000); free(big); }
    }

    /* Verify heap integrity — allocate after all frees */
    {
        void *final = malloc(4096);
        ASSERT("post-free 4K", final != NULL);
        if (final) { memset(final, 0xDD, 4096); free(final); }
    }

    section_end();
}
