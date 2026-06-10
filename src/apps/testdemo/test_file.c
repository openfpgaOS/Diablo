//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

#include "test.h"
#include <time.h>

void test_file_slots(void) {
    section_start("File Slots");

    /* fopen on unknown file should return NULL */
    ASSERT("ghost null", fopen("ghost.bin", "rb") == NULL);

    section_end();
}

void test_file_negative(void) {
    section_start("File Neg");

    /* fclose NULL — should not crash (musl handles it) */
    /* Note: fclose(NULL) is undefined in C, skip to avoid potential crash */

    /* Empty string */
    ASSERT("empty str", fopen("", "rb") == NULL);

    /* fread size=0 */
    FILE *f = fopen("slot:1", "rb");
    if (f) {
        uint8_t buf[4];
        ASSERT_EQ("fread sz0", (int)fread(buf, 0, 4, f), 0);
        ASSERT_EQ("fread cnt0", (int)fread(buf, 1, 0, f), 0);
        fclose(f);
    }

    /* fopen on unknown filename should return NULL */
    ASSERT("ghost null", fopen("ghost.dat", "rb") == NULL);

    section_end();
}

void test_file_io(void) {
    section_start("File I/O");

    /* Unregistered filenames should return NULL */
    ASSERT("bad name", fopen("nonexistent.xyz", "rb") == NULL);
    ASSERT("bad .grp", fopen("GAME.GRP", "rb") == NULL);
    ASSERT("bad path", fopen("/data/file.bin", "rb") == NULL);
    ASSERT("empty", fopen("", "rb") == NULL);

    /* Note: fopen("slot:99") would succeed (kernel doesn't validate slot IDs)
     * but fread would hang for ~2s waiting for DMA timeout. Skip this test. */

    FILE *f = fopen("slot:1", "rb");
    ASSERT("slot:1", f != NULL);
    if (f) {
        unsigned char buf[64];
        ASSERT("read 4", fread(buf, 1, 4, f) == 4);
        fclose(f);
    }

    f = fopen("slot:1", "rb");
    ASSERT("slot:1 seek", f != NULL);
    if (f) {
        unsigned char b1[64], b2[64];
        fread(b1, 1, 64, f);
        fseek(f, 0, SEEK_SET);
        fread(b2, 1, 64, f);
        ASSERT("seek match", memcmp(b1, b2, 64) == 0);
        ASSERT_EQ("ftell", (int)ftell(f), 64);
        fclose(f);
    }

    /* Repeated open/read/close — stress the fd table and bridge */
    {
        int repeat_ok = 1;
        for (int i = 0; i < 20; i++) {
            f = fopen("slot:1", "rb");
            if (!f) { repeat_ok = 0; break; }
            unsigned char buf[16];
            size_t n = fread(buf, 1, 16, f);
            if (n != 16) {
                snprintf(__buf, sizeof(__buf), "i=%d n=%d", i, (int)n);
                test_fail("20x orc", __buf);
                repeat_ok = 0;
                fclose(f);
                break;
            }
            fclose(f);
        }
        if (repeat_ok) test_pass("20x open/read/close");
    }

    /* Multiple files open simultaneously */
    FILE *f1 = fopen("slot:1", "rb");
    FILE *f2 = fopen("slot:2", "rb");
    ASSERT("multi f1", f1 != NULL);
    ASSERT("multi f2", f2 != NULL);
    if (f1 && f2) {
        unsigned char a[4], b[4];
        fread(a, 1, 4, f1);
        fread(b, 1, 4, f2);
        ASSERT("multi distinct", memcmp(a, b, 4) != 0);
    }
    if (f1) fclose(f1);
    if (f2) fclose(f2);

    /* Cache coherency test: fill buffer with known pattern,
     * then read file data over it, verify we see file data
     * (not stale cached pattern) */
    {
        static uint8_t coh_buf[4096];

        /* Fill with pattern — this goes into D-cache */
        memset(coh_buf, 0xAA, sizeof(coh_buf));
        ASSERT("pre-fill", coh_buf[0] == 0xAA && coh_buf[4095] == 0xAA);

        /* Read file data over the buffer via DMA */
        f = fopen("slot:1", "rb");
        if (f) {
            size_t n = fread(coh_buf, 1, 4096, f);
            fclose(f);

            /* Verify we see file data, not the 0xAA pattern.
             * slot:1 is os.bin — first bytes should NOT be 0xAA */
            ASSERT("coherency sz", n > 0);
            int stale = 1;
            for (int i = 0; i < 16; i++)
                if (coh_buf[i] != 0xAA) stale = 0;
            ASSERT("coherency !stale", !stale);

            /* Read again into same buffer to test second DMA */
            memset(coh_buf, 0x55, sizeof(coh_buf));
            f = fopen("slot:1", "rb");
            if (f) {
                fread(coh_buf, 1, 4096, f);
                fclose(f);
                int stale2 = 1;
                for (int i = 0; i < 16; i++)
                    if (coh_buf[i] != 0x55) stale2 = 0;
                ASSERT("coherency 2nd", !stale2);
            }

            /* Mock fopen: raw open+close via POSIX, no musl FILE/malloc */
            {
                int mock_fd = open("slot:1", O_RDONLY);
                if (mock_fd >= 0) close(mock_fd);
            }

            /* Large read coherency: 64KB */
            static uint8_t big_coh[65536];
            memset(big_coh, 0xCC, 4096);
            f = fopen("slot:1", "rb");
            if (f) {
                fread(big_coh, 1, 65536, f);
                fclose(f);
                /* Check first, middle, and last cached regions */
                int ok = (big_coh[0] != 0xCC) &&
                         (big_coh[32768] != 0xCC) &&
                         (big_coh[65535] != 0xCC);
                ASSERT("coherency 64K", ok);
            }
        }
    }

    /* Sequential read coherency: read multiple chunks into the same
     * buffer, verify each read returns correct data, not stale bytes
     * from the previous read. */
    {
        static uint8_t seq_buf[4096];

        /* Read offset 0 */
        f = fopen("slot:1", "rb");
        ASSERT("seq open", f != NULL);
        if (f) {
            fread(seq_buf, 1, 64, f);
            uint8_t first[64];
            memcpy(first, seq_buf, 64);

            /* Read offset 64 into same buffer — should be different */
            fread(seq_buf, 1, 64, f);
            ASSERT("seq diff", memcmp(first, seq_buf, 64) != 0);

            /* Seek back and re-read offset 0 — must match original */
            fseek(f, 0, SEEK_SET);
            fread(seq_buf, 1, 64, f);
            ASSERT("seq reread", memcmp(first, seq_buf, 64) == 0);

            /* Many small sequential reads */
            int seq_ok = 1;
            fseek(f, 0, SEEK_SET);
            for (int i = 0; i < 100; i++) {
                size_t n = fread(seq_buf, 1, 32, f);
                if (n != 32) { seq_ok = 0; break; }
            }
            ASSERT("100x 32B reads", seq_ok);

            fclose(f);
        }
    }

    /* Compare direct DMA vs bounce buffer — both should
     * return identical data for the same file offset */
    {
        static uint8_t buf_a[4096];
        static uint8_t buf_b[4096];

        /* First read: goes through sys_read (direct DMA or bounce) */
        f = fopen("slot:1", "rb");
        if (f) {
            fread(buf_a, 1, 4096, f);
            fclose(f);
        }

        /* Second read: same data, same offset */
        f = fopen("slot:1", "rb");
        if (f) {
            fread(buf_b, 1, 4096, f);
            fclose(f);
        }

        ASSERT("dma consistency", memcmp(buf_a, buf_b, 4096) == 0);

        /* Third read after writing to buffer — tests cache eviction */
        memset(buf_a, 0xFF, 4096);
        f = fopen("slot:1", "rb");
        if (f) {
            fread(buf_a, 1, 4096, f);
            fclose(f);
        }
        ASSERT("dma after write", memcmp(buf_a, buf_b, 4096) == 0);
    }

    /* Direct DMA coherency: write to a buffer, then DMA over it and
     * read back. Tests that cache flush before/after DMA works
     * correctly. */
    {
        static uint8_t dma_buf[65536];

        /* Write pattern that will be in D-cache */
        for (int i = 0; i < 65536; i++)
            dma_buf[i] = (uint8_t)(i & 0xFF);

        /* DMA file data over it (goes through direct DMA path) */
        f = fopen("slot:1", "rb");
        if (f) {
            fread(dma_buf, 1, 65536, f);
            fclose(f);

            /* Every byte must be file data, not our pattern.
             * Check multiple cache-line-aligned offsets */
            int ok = 1;
            for (int off = 0; off < 65536; off += 64) {
                if (dma_buf[off] == (uint8_t)(off & 0xFF)) {
                    /* Could be coincidence — check neighbors too */
                    if (dma_buf[off+1] == (uint8_t)((off+1) & 0xFF) &&
                        dma_buf[off+2] == (uint8_t)((off+2) & 0xFF)) {
                        ok = 0;  /* 3 matching bytes = stale cache line */
                        break;
                    }
                }
            }
            ASSERT("direct DMA 64K", ok);
        }

        /* Repeated lseek+read pattern for packed-file access */
        f = fopen("slot:1", "rb");
        if (f) {
            uint8_t a[256], b[256], c[256];

            /* Read at offset 0 */
            fseek(f, 0, SEEK_SET);
            fread(a, 1, 256, f);

            /* Read at offset 1024 */
            fseek(f, 1024, SEEK_SET);
            fread(b, 1, 256, f);

            /* Read at offset 0 again — must match first read */
            fseek(f, 0, SEEK_SET);
            fread(c, 1, 256, f);
            ASSERT("seek+read match", memcmp(a, c, 256) == 0);

            /* Reads at different offsets must differ */
            ASSERT("seek+read diff", memcmp(a, b, 256) != 0);

            /* Interleaved small reads at various offsets */
            int interleave_ok = 1;
            for (int i = 0; i < 50; i++) {
                uint32_t off = (uint32_t)(i * 137) % 4096;
                uint8_t r1[16], r2[16];
                fseek(f, off, SEEK_SET);
                fread(r1, 1, 16, f);
                fseek(f, off, SEEK_SET);
                fread(r2, 1, 16, f);
                if (memcmp(r1, r2, 16) != 0) {
                    interleave_ok = 0;
                    break;
                }
            }
            ASSERT("50x seek+read", interleave_ok);

            fclose(f);
        }
    }

    /* Speed test: read 64KB from slot:1 and measure throughput */
    {
        static uint8_t speed_buf[65536];
        uint32_t t_start = of_time_ms();
        f = fopen("slot:1", "rb");
        if (f) {
            fread(speed_buf, 1, 65536, f);
            fclose(f);
        }
        uint32_t t_elapsed = of_time_ms() - t_start;
        ASSERT("speed", t_elapsed < 5000);
        (void)t_elapsed;
    }

    section_end();
}

void test_posix_file_io(void) {
    section_start("POSIX File");

    /* Step 1: open via POSIX open() */
    int fd = open("slot:1", O_RDONLY);
    ASSERT("open", fd >= 0);
    if (fd < 0) { section_end(); return; }

    /* Step 2: read header — first 16 bytes */
    uint8_t hdr[16];
    int n = read(fd, hdr, 16);
    ASSERT_EQ("read hdr", n, 16);

    /* Step 3: sequential read (simulates index load) */
    uint8_t idx[256];
    n = read(fd, idx, 256);
    ASSERT_EQ("read idx", n, 256);
    /* File position should now be 272 */

    /* Step 4: rewind after loading the index */
    lseek(fd, 0, SEEK_SET);

    /* Verify rewind: re-read header must match */
    uint8_t hdr2[16];
    n = read(fd, hdr2, 16);
    ASSERT_EQ("re-read hdr", n, 16);
    ASSERT("hdr match", memcmp(hdr, hdr2, 16) == 0);

    /* Verify sequential position after rewind+read: bytes 16..31
     * should match idx[0..15] */
    uint8_t seq_check[16];
    n = read(fd, seq_check, 16);
    ASSERT_EQ("seq after rw", n, 16);
    ASSERT("seq data", memcmp(seq_check, idx, 16) == 0);

    /* Step 5: basic seek+read consistency at offset 0 */
    {
        uint8_t r1[32], r2[32];
        lseek(fd, 0, SEEK_SET);
        int n1 = read(fd, r1, 32);
        lseek(fd, 0, SEEK_SET);
        int n2 = read(fd, r2, 32);
        if (n1 != 32 || n2 != 32) {
            snprintf(__buf, sizeof(__buf), "n1=%d n2=%d", n1, n2);
            test_fail("seek0 read", __buf);
        } else if (memcmp(r1, r2, 32) != 0) {
            /* Diagnostic: show first differing byte */
            int d = -1;
            for (int i = 0; i < 32; i++) {
                if (r1[i] != r2[i]) { d = i; break; }
            }
            /* Encode the r1=hdr / r2=hdr diagnostic into the failure
             * detail itself so it lands in the paged summary instead
             * of bleeding into the column layout via stray printfs. */
            int r1ok = (memcmp(r1, hdr, 16) == 0);
            int r2ok = (memcmp(r2, hdr, 16) == 0);
            snprintf(__buf, sizeof(__buf), "d@%d %02x!=%02x %s%s",
                     d, r1[d], r2[d],
                     r1ok ? "r1ok" : "r1NO",
                     r2ok ? " r2ok" : " r2NO");
            test_fail("seek0 data", __buf);
        } else {
            test_pass("seek0 data");
        }
        /* Check it matches original header */
        ASSERT("seek0=hdr", memcmp(r1, hdr, 16) == 0);
    }

    /* Step 6: read at offset 1024 then back to 0 */
    {
        uint8_t a[32], b[32];
        lseek(fd, 1024, SEEK_SET);
        read(fd, a, 32);
        lseek(fd, 0, SEEK_SET);
        read(fd, b, 32);
        ASSERT("1024!=0", memcmp(a, b, 32) != 0);
        ASSERT("back to 0", memcmp(b, hdr, 16) == 0);
    }

    /* Step 7: cross read-ahead boundary (offset 4096+) */
    {
        uint8_t a[32], b[32];
        lseek(fd, 8192, SEEK_SET);
        read(fd, a, 32);
        lseek(fd, 8192, SEEK_SET);
        read(fd, b, 32);
        ASSERT("8192 match", memcmp(a, b, 32) == 0);

        /* Back to 0 after crossing boundary */
        lseek(fd, 0, SEEK_SET);
        read(fd, a, 16);
        ASSERT("post-8192 hdr", memcmp(a, hdr, 16) == 0);
    }

    /* Step 8: repeated reads from offset 0 — find when data diverges */
    {
        uint8_t t[8][16];
        for (int i = 0; i < 8; i++) {
            lseek(fd, 0, SEEK_SET);
            read(fd, t[i], 16);
        }
        int rep_ok = 1;
        for (int i = 1; i < 8; i++) {
            if (memcmp(t[0], t[i], 16) != 0) {
                snprintf(__buf, sizeof(__buf), "read#%d: %02x%02x!=%02x%02x",
                         i, t[i][0], t[i][1], t[0][0], t[0][1]);
                test_fail("8x read0", __buf);
                rep_ok = 0;
                break;
            }
        }
        if (rep_ok) test_pass("8x read0");
        ASSERT("8x=hdr", memcmp(t[0], hdr, 16) == 0);
    }

    /* Step 9: seek to 4096 (outside ra_buf), then back to 0 — repeat */
    {
        uint8_t a[16], b[16];
        int cross_ok = 1;
        for (int i = 0; i < 10; i++) {
            /* Read from beyond ra_buf range to force refill */
            lseek(fd, 4096, SEEK_SET);
            read(fd, a, 16);
            /* Read back from 0 — must match header */
            lseek(fd, 0, SEEK_SET);
            read(fd, b, 16);
            if (memcmp(b, hdr, 16) != 0) {
                snprintf(__buf, sizeof(__buf), "i=%d: %02x%02x!=%02x%02x",
                         i, b[0], b[1], hdr[0], hdr[1]);
                test_fail("10x cross", __buf);
                cross_ok = 0;
                break;
            }
        }
        if (cross_ok) test_pass("10x cross");
    }

    /* Step 10: original 9x loop — seek(X), read, seek(X), read, compare
     * This is the pattern that was failing. Test both r1==r2 and r1==ref. */
    {
        uint32_t offsets[] = { 0, 1024, 512, 4096, 2048, 0, 8192, 256, 0 };
        /* Capture reference data for each offset first */
        uint8_t ref[9][32];
        for (int i = 0; i < 9; i++) {
            lseek(fd, offsets[i], SEEK_SET);
            read(fd, ref[i], 32);
        }
        /* Now do the seek-seek-compare pattern */
        int loop_ok = 1;
        for (int i = 0; i < 9; i++) {
            uint8_t r1[32], r2[32];
            lseek(fd, offsets[i], SEEK_SET);
            read(fd, r1, 32);
            lseek(fd, offsets[i], SEEK_SET);
            read(fd, r2, 32);
            int r1_ref = (memcmp(r1, ref[i], 32) == 0);
            int r2_ref = (memcmp(r2, ref[i], 32) == 0);
            int r1_r2  = (memcmp(r1, r2, 32) == 0);
            if (!r1_r2) {
                snprintf(__buf, sizeof(__buf),
                    "i=%d off=%lu r1=ref:%d r2=ref:%d",
                    i, (unsigned long)offsets[i], r1_ref, r2_ref);
                test_fail("9x loop", __buf);
                loop_ok = 0;
                break;
            }
        }
        if (loop_ok) test_pass("9x loop");
    }

    /* Step 11: seek to various offsets then back to 0 */
    {
        uint32_t offsets[] = { 1024, 4096, 8192, 2048, 512, 16384, 256 };
        uint8_t tmp[16], z[16];
        int var_ok = 1;
        for (int i = 0; i < 7; i++) {
            lseek(fd, offsets[i], SEEK_SET);
            read(fd, tmp, 16);
            lseek(fd, 0, SEEK_SET);
            read(fd, z, 16);
            if (memcmp(z, hdr, 16) != 0) {
                snprintf(__buf, sizeof(__buf), "off=%lu: %02x%02x!=%02x%02x",
                         (unsigned long)offsets[i], z[0], z[1], hdr[0], hdr[1]);
                test_fail("7x varied", __buf);
                var_ok = 0;
                break;
            }
        }
        if (var_ok) test_pass("7x varied");
    }

    close(fd);
    section_end();
}

void test_lseek_readahead(void) {
    section_start("Seek Rdahd");

    /* Test: lseek must invalidate read-ahead buffer.
     * Opens a data slot, reads N bytes, seeks back to 0, reads again.
     * The second read must match the first. With broken read-ahead,
     * the second read returns stale buffered data from the wrong offset. */
    int fd = open("slot:1", O_RDONLY);
    ASSERT("open", fd >= 0);
    if (fd < 0) { section_end(); return; }

    uint8_t buf1[256], buf2[256];

    /* Read first 256 bytes */
    int r1 = read(fd, buf1, 256);
    ASSERT_EQ("read1", r1, 256);

    /* Read 48KB to advance past the read-ahead buffer (32KB) */
    uint8_t discard[1024];
    for (int i = 0; i < 48; i++)
        read(fd, discard, 1024);

    /* Seek back to start */
    lseek(fd, 0, SEEK_SET);

    /* Read first 256 bytes again — must match buf1 */
    int r2 = read(fd, buf2, 256);
    ASSERT_EQ("read2", r2, 256);
    ASSERT("match", memcmp(buf1, buf2, 256) == 0);

    /* Also verify seeking to a mid-file offset after readahead */
    lseek(fd, 0, SEEK_SET);
    uint8_t ref[64];
    read(fd, ref, 64);  /* bytes 0-63 */

    /* Advance past readahead again */
    for (int i = 0; i < 48; i++)
        read(fd, discard, 1024);

    /* Seek to offset 0, re-read, must match */
    lseek(fd, 0, SEEK_SET);
    uint8_t check[64];
    read(fd, check, 64);
    ASSERT("mid match", memcmp(ref, check, 64) == 0);

    /* Seek to different offsets and verify consistency */
    {
        uint8_t a[32], b[32];
        int seek_ok = 1;
        uint32_t offsets[] = { 0, 4096, 8192, 0, 16384, 0, 32768, 0 };
        for (int i = 0; i < 8; i += 2) {
            lseek(fd, offsets[i], SEEK_SET);
            read(fd, a, 32);
            lseek(fd, offsets[i], SEEK_SET);
            read(fd, b, 32);
            if (memcmp(a, b, 32) != 0) {
                snprintf(__buf, sizeof(__buf), "off=%d", (int)offsets[i]);
                test_fail("seek pairs", __buf);
                seek_ok = 0;
                break;
            }
        }
        if (seek_ok) test_pass("seek pairs");
    }

    close(fd);
    section_end();
}

void test_lseek_large_read(void) {
    section_start("Seek Large");

    /* Simulates art-file loading: read a large block, seek back,
     * read again at a different offset. Exercises read-ahead
     * invalidation with reads that span multiple refill windows. */
    int fd = open("slot:1", O_RDONLY);
    ASSERT("open", fd >= 0);
    if (fd < 0) { section_end(); return; }

    uint8_t *buf1 = malloc(65536);
    uint8_t *buf2 = malloc(65536);
    ASSERT("alloc", buf1 != NULL && buf2 != NULL);
    if (!buf1 || !buf2) { free(buf1); free(buf2); close(fd); section_end(); return; }

    /* Canary writes — fault here (cause 7) means malloc straddled the heap edge. */
    buf1[0] = 0x11;
    buf1[65535] = 0x11;
    buf2[0] = 0x22;
    buf2[65535] = 0x22;

    /* Read 64KB to fill and refill read-ahead multiple times */
    int r1 = read(fd, buf1, 65536);
    ASSERT_EQ("read 64K", r1, 65536);

    /* Seek to offset 1024 (within first read-ahead window) */
    lseek(fd, 1024, SEEK_SET);

    /* Large read that spans multiple read-ahead refills */
    int r2 = read(fd, buf2, 65536);
    ASSERT_EQ("reread 64K", r2, 65536);

    /* Verify: buf2 should match buf1 starting at offset 1024 */
    if (memcmp(buf2, buf1 + 1024, 65536 - 1024) != 0) {
        /* Find first mismatch */
        int bad = 0;
        for (int i = 0; i < 65536 - 1024; i++) {
            if (buf2[i] != buf1[1024 + i]) { bad = i; break; }
        }
        snprintf(__buf, sizeof(__buf), "@%d: got=%02x exp=%02x",
                 bad, buf2[bad], buf1[1024 + bad]);
        test_fail("seek+64K", __buf);
    } else {
        test_pass("seek+64K");
    }

    /* Seek back to 0, read 64KB again — must match original */
    lseek(fd, 0, SEEK_SET);
    int r3 = read(fd, buf2, 65536);
    ASSERT_EQ("reread from 0", r3, 65536);
    ASSERT("full match", memcmp(buf1, buf2, 65536) == 0);

    /* Interleaved: read 32KB, seek to 4096, read 32KB */
    lseek(fd, 0, SEEK_SET);
    read(fd, buf1, 32768);
    lseek(fd, 4096, SEEK_SET);
    read(fd, buf2, 32768);
    /* buf2 should match buf1 starting at offset 4096 */
    ASSERT("interleave", memcmp(buf2, buf1 + 4096, 32768 - 4096) == 0);

    free(buf1);
    free(buf2);
    close(fd);
    section_end();
}

void test_dma_cache(void) {
    section_start("DMA Cache");

    /* 8KB buffer — large enough to trigger the fast DMA path (>=4KB) */
    static uint8_t buf_a[8192];
    static uint8_t buf_b[8192];

    /* Read first 8KB of os.bin as reference */
    int fd = open("slot:1", O_RDONLY);
    ASSERT("open", fd >= 0);
    if (fd < 0) { section_end(); return; }

    int n = read(fd, buf_a, 8192);
    ASSERT_EQ("ref read", n, 8192);

    /* Poison buf_b with a known pattern */
    memset(buf_b, 0xAA, 8192);

    /* Read same data into buf_b — DMA must overwrite the poison */
    lseek(fd, 0, SEEK_SET);
    n = read(fd, buf_b, 8192);
    ASSERT_EQ("dma read", n, 8192);

    /* buf_b must match buf_a, not the 0xAA poison */
    ASSERT("no poison", buf_b[0] != 0xAA || buf_a[0] == 0xAA);
    ASSERT("dma match", memcmp(buf_a, buf_b, 8192) == 0);

    /* Repeat: poison, seek, read, verify — 5 times */
    int repeat_ok = 1;
    for (int i = 0; i < 5; i++) {
        memset(buf_b, 0x55 + i, 8192);
        lseek(fd, 0, SEEK_SET);
        n = read(fd, buf_b, 8192);
        if (n != 8192 || memcmp(buf_a, buf_b, 8192) != 0) {
            int d = -1;
            for (int j = 0; j < 8192; j++)
                if (buf_a[j] != buf_b[j]) { d = j; break; }
            snprintf(__buf, sizeof(__buf), "i=%d d@%d: %02x!=%02x poison=%02x",
                     i, d, buf_b[d], buf_a[d], (uint8_t)(0x55 + i));
            test_fail("5x dma", __buf);
            repeat_ok = 0;
            break;
        }
    }
    if (repeat_ok) test_pass("5x dma");

    /* Cross-offset DMA: read from offset 0, then 4096, then 0 again */
    lseek(fd, 0, SEEK_SET);
    read(fd, buf_a, 8192);  /* reference from offset 0 */

    static uint8_t buf_c[8192];
    lseek(fd, 4096, SEEK_SET);
    read(fd, buf_c, 8192);  /* different data from offset 4096 */
    ASSERT("cross diff", memcmp(buf_a, buf_c, 8192) != 0);

    memset(buf_b, 0xBB, 8192);
    lseek(fd, 0, SEEK_SET);
    read(fd, buf_b, 8192);  /* back to offset 0 */
    ASSERT("cross back", memcmp(buf_a, buf_b, 8192) == 0);

    /* Large read: 64KB (exercises multiple DMA chunks if applicable) */
    {
        static uint8_t big_a[65536];
        static uint8_t big_b[65536];
        lseek(fd, 0, SEEK_SET);
        int na = read(fd, big_a, 65536);
        memset(big_b, 0xCC, 65536);
        lseek(fd, 0, SEEK_SET);
        int nb = read(fd, big_b, 65536);
        ASSERT_EQ("64k a", na, 65536);
        ASSERT_EQ("64k b", nb, 65536);
        if (na == 65536 && nb == 65536)
            ASSERT("64k match", memcmp(big_a, big_b, 65536) == 0);
    }

    close(fd);
    section_end();
}

void test_file_limit(void) {
    section_start("File Limit");

    /*
     * Stripped-down fopen/fclose cycle counter.  Earlier investigation
     * noted "CPU hard-stalls after ~35 cycles" but that was actually
     * a terminal-scroll bug in the instrumentation itself — a '\n'
     * in the per-iteration trace was causing of_term_scroll() to
     * trip an unrelated sync-burst hazard.  Without the '\n' emit
     * (no trace at all) the loop runs to completion cleanly.
     */
    const int MAX_CYCLES = 100;
    int i;
    for (i = 0; i < MAX_CYCLES; i++) {
        FILE *f = fopen("slot:1", "rb");
        if (!f) {
            snprintf(__buf, sizeof(__buf), "fopen NULL @ %d", i);
            test_fail("limit", __buf);
            break;
        }
        uint8_t buf[16];
        size_t n = fread(buf, 1, 16, f);
        if (n != 16) {
            snprintf(__buf, sizeof(__buf), "fread=%d @ %d", (int)n, i);
            test_fail("limit rd", __buf);
            fclose(f);
            break;
        }
        fclose(f);
    }
    snprintf(__buf, sizeof(__buf), "%d/%d cycles", i, MAX_CYCLES);
    if (i == MAX_CYCLES) test_pass(__buf);

    section_end();
}

void test_oversize_read(void) {
    section_start("Oversize Rd");

    /* Read with a buffer much larger than the file.
     * os.bin is ~112KB. Request 256KB — fread must return the
     * actual file size, not hang or return < 14 bytes.
     * This is the pattern mididemo uses: fread(buf, 1, MAX, f). */
    uint8_t *big = malloc(262144);
    ASSERT("alloc 256K", big != NULL);
    if (!big) { section_end(); return; }

    FILE *f = fopen("slot:1", "rb");
    ASSERT("open", f != NULL);
    if (f) {
        size_t n = fread(big, 1, 262144, f);
        ASSERT("got data", n >= 1024);
        snprintf(__buf, sizeof(__buf), "fread=%d", (int)n);
        test_pass(__buf);

        /* Verify it's valid data (not zeros or garbage) */
        ASSERT("not zeros", big[0] != 0 || big[1] != 0 || big[2] != 0 || big[3] != 0);

        /* Read again with exact returned size — must match */
        uint8_t *ref = malloc(n);
        if (ref) {
            fseek(f, 0, SEEK_SET);
            size_t n2 = fread(ref, 1, n, f);
            ASSERT_EQ("reread sz", (int)n2, (int)n);
            if (n2 == n)
                ASSERT("reread match", memcmp(big, ref, n) == 0);
            free(ref);
        }

        fclose(f);
    }

    /* slot:3 = testdemo.elf (~240KB). Request 256KB — bigger overshoot.
     * This is closer to the mididemo scenario (5.8KB file, 256KB request).
     * (slot:2 is now os.ini under the APF_VER_1 contract; the ELF moved
     * to slot:3.) */
    f = fopen("slot:3", "rb");
    ASSERT("open slot:3", f != NULL);
    if (f) {
        size_t n = fread(big, 1, 262144, f);
        ASSERT("slot:3 data", n >= 1024);
        snprintf(__buf, sizeof(__buf), "slot:3=%d", (int)n);
        test_pass(__buf);
        fclose(f);
    }

    /* Also test via POSIX read() with oversize request */
    int fd = open("slot:1", O_RDONLY);
    ASSERT("posix open", fd >= 0);
    if (fd >= 0) {
        int n = read(fd, big, 262144);
        ASSERT("posix data", n >= 1024);

        /* Seek back and verify header matches */
        lseek(fd, 0, SEEK_SET);
        uint8_t hdr[16];
        int n2 = read(fd, hdr, 16);
        ASSERT_EQ("posix hdr sz", n2, 16);
        ASSERT("posix hdr match", memcmp(big, hdr, 16) == 0);

        close(fd);
    }

    free(big);
    section_end();
}
