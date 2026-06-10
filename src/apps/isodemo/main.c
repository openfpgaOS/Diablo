//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * isodemo -- read an ISO 9660 data slot using async file DMA.
 *
 * Add an APF data slot named "isodemo.iso" next to this app.  The demo
 * reads the primary volume descriptor and root directory asynchronously,
 * then prints the root entries.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "of.h"

#define ISO_NAME       "isodemo.iso"
#define ISO_PVD_OFFSET (16u * 2048u)
#define ISO_PVD_SIZE   2048u
#define ISO_DIR_STAGE_SIZE (8u * 1024u)

#ifndef OF_PC
static volatile int async_done;
static volatile int async_token;
static volatile int async_result;
#endif

static void park(void) {
    for (;;) usleep(100 * 1000);
}

#ifndef OF_PC
static void async_read_done(int token, int result) {
    async_token = token;
    async_result = result;
    async_done = 1;
}

static int wait_async(uint32_t timeout_ms) {
    uint32_t start = of_time_ms();
    for (;;) {
        if (async_done)
            return async_result;
        if (of_file_async_poll())
            return async_done ? async_result : 0;
        if (!of_file_async_busy())
            return async_done ? async_result : 0;
        if ((uint32_t)(of_time_ms() - start) >= timeout_ms)
            return OF_ERR_TIMEOUT;
        usleep(1000);
    }
}

static uint32_t rd32le(const unsigned char *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int async_read_slot(int slot, uint32_t offset,
                           void *dest, uint32_t len) {
    async_done = 0;
    async_token = -1;
    async_result = OF_ERR_FAILED;

    int token = of_file_read_async(slot, offset, dest, len, async_read_done);
    if (token < 0)
        return token;

    return wait_async(2000);
}

static int print_iso_dir(const unsigned char *dir, uint32_t len) {
    uint32_t off = 0;
    int count = 0;

    while (off < len) {
        uint8_t rec_len = dir[off];
        if (rec_len == 0) {
            off = (off + 2048u) & ~2047u;
            continue;
        }
        if (rec_len < 34 || off + rec_len > len)
            break;

        const unsigned char *rec = dir + off;
        uint32_t size = rd32le(rec + 10);
        int is_dir = (rec[25] & 0x02) != 0;
        uint8_t name_len = rec[32];
        const unsigned char *name = rec + 33;

        if (!(name_len == 1 && (name[0] == 0 || name[0] == 1))) {
            char nm[33];
            int n = name_len < sizeof(nm) ? name_len : (int)sizeof(nm) - 1;
            memcpy(nm, name, n);
            nm[n] = 0;
            for (int i = 0; nm[i]; i++) {
                if (nm[i] == ';') {
                    nm[i] = 0;
                    break;
                }
            }
            printf("  %-24s %8lu B%s\n",
                   nm, (unsigned long)size, is_dir ? "/" : "");
            count++;
        }

        off += rec_len;
    }

    return count;
}

static void run_async_iso_demo(void) {
    uint32_t slot = 0;
    int rc = of_file_slot_find(ISO_NAME, &slot);
    if (rc < 0) {
        printf("  async slot find failed: %d\n", rc);
        return;
    }

    uint32_t max_read = of_file_async_max_read();
    uint32_t stage_size = ISO_DIR_STAGE_SIZE;
    if (max_read != 0 && stage_size > max_read)
        stage_size = max_read;
    if (stage_size < ISO_PVD_SIZE) {
        printf("  async max read too small: %lu\n", (unsigned long)max_read);
        return;
    }

    unsigned char *stage =
        (unsigned char *)of_file_dma_stage_alloc(stage_size, 2048);
    if (!stage) {
        printf("  async stage alloc failed\n");
        return;
    }

    rc = async_read_slot((int)slot, ISO_PVD_OFFSET, stage, ISO_PVD_SIZE);
    if (rc < 0) {
        printf("  async PVD read failed: %d\n", rc);
        return;
    }
    if (stage[0] != 1 || memcmp(stage + 1, "CD001", 5) != 0) {
        printf("  invalid ISO primary descriptor\n");
        return;
    }

    const unsigned char *root = stage + 156;
    uint32_t root_lba = rd32le(root + 2);
    uint32_t root_size = rd32le(root + 10);
    uint32_t read_len = root_size;
    if (read_len > stage_size)
        read_len = stage_size;

    printf("  async slot %lu root LBA %lu\n\n",
           (unsigned long)slot, (unsigned long)root_lba);

    rc = async_read_slot((int)slot, root_lba * 2048u, stage, read_len);
    if (rc < 0) {
        printf("  async root read failed: %d\n", rc);
        return;
    }

    int count = print_iso_dir(stage, read_len);
    printf("\n  %d entr%s", count, count == 1 ? "y" : "ies");
    if (root_size > read_len)
        printf(" (truncated)");
    printf("\n");
}
#else
static void run_async_iso_demo(void) {
    printf("  async read skipped on PC\n");
}
#endif

int main(void) {
    printf("\033[2J\033[H");
    printf("\n  \033[93mISO Demo\033[0m\n\n");

    run_async_iso_demo();
    park();
    return 0;
}
