//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * save_test.h -- shared save cross-pollution test logic
 *
 * Canonical example of:
 *   - Naming saves `<APP>_<N>.sav` so each app gets its own save namespace
 *     in the APF data slots (slot:10..slot:19)
 *   - Using POSIX fopen/fread/fwrite/fseek for save I/O — the kernel
 *     bounces through CRAM0 and into the APF data slot transparently
 *   - The four equivalent paths the kernel accepts for a save: the
 *     registered filename, `save:N`, `save_N`, and `slot:10+N`.  Apps
 *     should pick one form and stick to it; this test exercises all four
 *     to verify the VFS surface is consistent.
 *   - CRC32 + magic-on-disk patterns for detecting cross-app corruption
 *     (a bug elsewhere that lets app A overwrite app B's slot would
 *     surface as a magic / app_id mismatch on next boot).
 *
 * Used by `savea/` and `saveb/`.  Each app defines APP_ID, APP_NAME,
 * STEP and (optionally) VSAVE_SIZE_EVEN before including this header,
 * then calls save_test_main() from main().  Running the apps
 * alternately rotates saves and rechecks identity, so any cross-app
 * corruption shows up as the wrong APP_ID in a slot the other app
 * just wrote.
 */

#ifndef OF_SAVE_TEST_H
#define OF_SAVE_TEST_H

#include "of.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#ifndef APP_ID
#error "APP_ID must be defined before including save_test.h"
#endif
#ifndef APP_NAME
#error "APP_NAME must be defined before including save_test.h"
#endif
#ifndef STEP
#error "STEP must be defined before including save_test.h"
#endif

#ifndef NUM_VSAVES
#define NUM_VSAVES 10
#endif
#ifndef VSAVE_SIZE_EVEN
#define VSAVE_SIZE_EVEN 16384
#endif
#ifndef VSAVE_SIZE_ODD
#define VSAVE_SIZE_ODD 8192
#endif

#define VSAVE_SIZE      VSAVE_SIZE_EVEN  /* max, for buffer sizing */
#define SLOT_SIZE       VSAVE_SIZE_EVEN  /* one vsave per slot */
#define MAGIC           0x5356           /* "SV" */
#define PAYLOAD_OFFSET  16

/* Each virtual save layout:
 *   [0..1]   magic (0x5356)
 *   [2]      app_id
 *   [3]      slot_index
 *   [4..7]   iteration counter (increments each rotation)
 *   [8..11]  CRC32 of bytes [PAYLOAD_OFFSET..size-1]
 *   [12..15] reserved (0)
 *   [16..size-1]  random data seeded from (app_id, slot_index, iteration)
 */
typedef struct {
    uint16_t magic;
    uint8_t  app_id;
    uint8_t  slot_index;
    uint32_t iteration;
    uint32_t crc;
    uint32_t reserved;
} vsave_header_t;

static uint32_t vsave_data_size(int idx) {
    return (idx & 1) ? VSAVE_SIZE_ODD : VSAVE_SIZE_EVEN;
}
#define PAYLOAD_SIZE(sz) ((sz) - PAYLOAD_OFFSET)

static void make_save_path(int slot, char *path, uint32_t path_len) {
    snprintf(path, path_len, "%s_%d.sav", APP_NAME, slot);
}

static int save_read(int slot, void *buf, uint32_t offset, uint32_t len) {
    char path[32];
    make_save_path(slot, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (offset && fseek(f, offset, SEEK_SET) != 0) { fclose(f); return -1; }
    int n = (int)fread(buf, 1, len, f);
    fclose(f);
    return n;
}

static int save_write(int slot, const void *buf, uint32_t offset, uint32_t len) {
    char path[32];
    make_save_path(slot, path, sizeof(path));
    FILE *f = fopen(path, offset == 0 ? "wb" : "r+b");
    if (!f) f = fopen(path, "wb");
    if (!f) return -1;
    if (offset && fseek(f, offset, SEEK_SET) != 0) { fclose(f); return -1; }
    int n = (int)fwrite(buf, 1, len, f);
    if (fclose(f) != 0)
        return -1;
    return n;
}

static int read_header_path(const char *path, vsave_header_t *hdr) {
    memset(hdr, 0, sizeof(*hdr));
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int n = (int)fread(hdr, 1, sizeof(*hdr), f);
    fclose(f);
    return n == (int)sizeof(*hdr) ? 0 : -1;
}

static int check_header_path(const char *label, const char *path) {
    vsave_header_t hdr;
    int rc = read_header_path(path, &hdr);
    if (rc < 0) {
        printf("  \033[91mFAIL\033[0m %s open/read\n", label);
        return 0;
    }
    if (hdr.magic != MAGIC || hdr.app_id != APP_ID || hdr.slot_index != 0) {
        printf("  \033[91mFAIL\033[0m %s header\n", label);
        return 0;
    }
    printf("  %s ok\n", label);
    return 1;
}

static int check_size_path(const char *label, const char *path, uint32_t want) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("  \033[91mFAIL\033[0m %s open\n", label);
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        printf("  \033[91mFAIL\033[0m %s seek\n", label);
        return 0;
    }
    long got = ftell(f);
    fclose(f);
    if (got != (long)want) {
        printf("  \033[91mFAIL\033[0m %s size got=%ld want=%ld\n",
               label, got, (long)want);
        return 0;
    }
    printf("  %s size=%ld ok\n", label, got);
    return 1;
}

static int check_readonly_data_slot(void) {
    FILE *f = fopen("slot:2", "wb");
    if (f) {
        fclose(f);
        printf("  \033[91mFAIL\033[0m slot:2 accepted write open\n");
        return 0;
    }
    printf("  slot:2 write denied ok\n");
    return 1;
}

static int test_vfs_surface(void) {
    char path[32];
    int ok = 1;

    make_save_path(0, path, sizeof(path));

    printf("\n  POSIX/VFS checks...\n");
    ok &= check_header_path("filename", path);
    ok &= check_header_path("save:0 alias", "save:0");
    ok &= check_header_path("save_0 alias", "save_0");
    ok &= check_header_path("slot:10 save id", "slot:10");
    ok &= check_size_path("filename", path, vsave_data_size(0));
    ok &= check_readonly_data_slot();

    return ok ? 0 : -1;
}

/* Simple CRC32 (no table, small code) */
static uint32_t crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

/* Deterministic PRNG seeded from app_id + slot + iteration */
static uint32_t xorshift_state;
static void seed_rng(uint8_t app_id, uint8_t slot, uint32_t iter) {
    xorshift_state = ((uint32_t)app_id << 24) ^ ((uint32_t)slot << 16) ^ iter ^ 0x12345678;
    if (xorshift_state == 0) xorshift_state = 1;
}
static uint32_t xorshift32(void) {
    uint32_t x = xorshift_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    xorshift_state = x;
    return x;
}

/* Map virtual save index to (slot, offset). One vsave per slot in current
 * configuration; the math generalizes if SLOT_SIZE > VSAVE_SIZE. */
static void vsave_location(int vsave_idx, int *slot, uint32_t *offset) {
    int vsaves_per_slot = SLOT_SIZE / VSAVE_SIZE;
    if (vsaves_per_slot < 1) vsaves_per_slot = 1;
    *slot = vsave_idx / vsaves_per_slot;
    *offset = (vsave_idx % vsaves_per_slot) * VSAVE_SIZE;
}

static uint8_t buf[VSAVE_SIZE];

static void generate_payload(uint8_t app_id, uint8_t slot_index,
                             uint32_t iteration, uint32_t payload_size) {
    seed_rng(app_id, slot_index, iteration);
    for (uint32_t i = 0; i < payload_size; i += 4) {
        uint32_t r = xorshift32();
        uint32_t remaining = payload_size - i;
        uint32_t take = (remaining >= 4) ? 4 : remaining;
        for (uint32_t j = 0; j < take; j++)
            buf[PAYLOAD_OFFSET + i + j] = (uint8_t)(r >> (j * 8));
    }
}

static int write_vsave(int vsave_idx, uint32_t iteration) {
    int slot;
    uint32_t offset;
    vsave_location(vsave_idx, &slot, &offset);

    uint32_t sz  = vsave_data_size(vsave_idx);
    uint32_t psz = PAYLOAD_SIZE(sz);

    vsave_header_t *hdr = (vsave_header_t *)buf;
    hdr->magic      = MAGIC;
    hdr->app_id     = APP_ID;
    hdr->slot_index = (uint8_t)vsave_idx;
    hdr->iteration  = iteration;
    hdr->reserved   = 0;

    generate_payload(APP_ID, (uint8_t)vsave_idx, iteration, psz);
    hdr->crc = crc32(&buf[PAYLOAD_OFFSET], psz);

    int rc = save_write(slot, buf, offset, sz);
    return (rc == (int)sz) ? 0 : -1;
}

static int verify_vsave(int vsave_idx, uint32_t *out_iteration) {
    int slot;
    uint32_t offset;
    vsave_location(vsave_idx, &slot, &offset);

    uint32_t sz  = vsave_data_size(vsave_idx);
    uint32_t psz = PAYLOAD_SIZE(sz);

    int rc = save_read(slot, buf, offset, sz);
    if (rc != (int)sz) return -1;

    vsave_header_t *hdr = (vsave_header_t *)buf;
    if (hdr->magic != MAGIC) return -2;
    if (hdr->app_id != APP_ID) return -3;
    if (hdr->slot_index != (uint8_t)vsave_idx) return -4;

    uint32_t stored_crc = hdr->crc;
    uint32_t actual_crc = crc32(&buf[PAYLOAD_OFFSET], psz);
    if (actual_crc != stored_crc) return -5;

    uint32_t iter = hdr->iteration;
    seed_rng(APP_ID, (uint8_t)vsave_idx, iter);
    for (uint32_t i = 0; i < psz; i += 4) {
        uint32_t r = xorshift32();
        uint32_t remaining = psz - i;
        uint32_t check = (remaining >= 4) ? 4 : remaining;
        for (uint32_t j = 0; j < check; j++) {
            if (buf[PAYLOAD_OFFSET + i + j] != (uint8_t)(r >> (j * 8)))
                return -6;
        }
    }

    if (out_iteration) *out_iteration = iter;
    return 0;
}

static int is_virgin(void) {
    uint8_t hdr[4] = {0};
    int rc = save_read(0, hdr, 0, 4);
    if (rc != 4)
        return 1;
    uint16_t magic = hdr[0] | (hdr[1] << 8);
    uint8_t app_id = hdr[2];
    return (magic != MAGIC || app_id != APP_ID);
}

static int initialize_all_vsaves(void) {
    int ok = 1;

    for (int i = 0; i < NUM_VSAVES; i++) {
        int rc = write_vsave(i, 0);
        if (rc < 0) {
            ok = 0;
            printf("  \033[91mFAIL\033[0m write vsave %d (rc=%d)\n", i, rc);
            continue;
        }

        uint32_t iter;
        int vrc = verify_vsave(i, &iter);
        uint32_t sz = vsave_data_size(i);
        if (vrc < 0) {
            ok = 0;
            printf("  [%d] %dB write ok, \033[91mreadback FAIL rc=%d\033[0m\n",
                   i, (int)sz, vrc);
        } else {
            printf("  [%d] %dB ok\n", i, (int)sz);
        }
    }

    if (test_vfs_surface() < 0) {
        ok = 0;
        printf("\n  \033[91mVFS checks failed\033[0m\n");
    }

    return ok ? 0 : -1;
}

static int save_test_main(void) {
    printf("\033[2J\033[H");
    printf("\033[93m  %s Save Test (step=%d)\033[0m\n\n", APP_NAME, STEP);

    /* Nothing to test without persistent save storage. */
    if (!of_has_feature(OF_HW_SAVE_SLOTS)) {
        printf("  No save storage on this platform (OF_HW_SAVE_SLOTS clear)\n");
        for (;;) usleep(100 * 1000);
    }

    printf("  Save sizes: even=%dB odd=%dB\n",
           (int)VSAVE_SIZE_EVEN, (int)VSAVE_SIZE_ODD);
    int virgin = is_virgin();
    printf("  virgin=%d\n\n", virgin);

    if (virgin) {
        printf("  First run -- initializing %d virtual saves...\n", NUM_VSAVES);
        if (initialize_all_vsaves() == 0)
            printf("\n  \033[92mInitialized. Run again to test rotation.\033[0m\n");
        else
            printf("\n  \033[91mINITIALIZATION FAILED\033[0m\n");
        goto done;
    }

    printf("  Verifying all %d virtual saves...\n", NUM_VSAVES);

    uint32_t iterations[NUM_VSAVES];
    int all_ok = 1;
    int recoverable_stale_set = 1;
    for (int i = 0; i < NUM_VSAVES; i++) {
        int rc = verify_vsave(i, &iterations[i]);
        if (rc < 0) {
            all_ok = 0;
            if (rc != -1 && rc != -2)
                recoverable_stale_set = 0;
            const char *reason;
            switch (rc) {
                case -2: reason = "bad magic"; break;
                case -3: reason = "WRONG APP_ID (cross-pollution!)"; break;
                case -4: reason = "wrong slot_index"; break;
                case -5: reason = "CRC MISMATCH (data corruption!)"; break;
                case -6: reason = "PAYLOAD MISMATCH (bit rot!)"; break;
                default: reason = "read error"; break;
            }
            printf("  \033[91mFAIL\033[0m vsave %d (%dB): %s\n",
                   i, (int)vsave_data_size(i), reason);
        } else {
            printf("  [%d] %dB ok (iter=%d)\n",
                   i, (int)vsave_data_size(i), (int)iterations[i]);
        }
    }

    if (!all_ok) {
        printf("\n  \033[91mVERIFICATION FAILED\033[0m\n");
        if (recoverable_stale_set) {
            printf("  \033[93mReinitializing the full save set to clear stale corrupt files.\033[0m\n");
            printf("  \033[93mRun again; repeated reinitialization means load/writeback is still corrupting data.\033[0m\n\n");
            if (initialize_all_vsaves() == 0)
                printf("\n  \033[92mReinitialized. Run again to test rotation.\033[0m\n");
            else
                printf("\n  \033[91mREINITIALIZATION FAILED\033[0m\n");
        }
        goto done;
    }

    if (test_vfs_surface() < 0) {
        printf("\n  \033[91mVFS checks failed\033[0m\n");
        goto done;
    }

    printf("\n  Rotating by step=%d...\n", STEP);

    uint32_t new_iters[NUM_VSAVES];
    for (int i = 0; i < NUM_VSAVES; i++) {
        int src = ((i - STEP) % NUM_VSAVES + NUM_VSAVES) % NUM_VSAVES;
        new_iters[i] = iterations[src] + 1;
    }

    for (int i = 0; i < NUM_VSAVES; i++) {
        int rc = write_vsave(i, new_iters[i]);
        if (rc < 0) {
            printf("  \033[91mFAIL\033[0m write vsave %d\n", i);
            goto done;
        }
    }

    int verify_ok = 1;
    for (int i = 0; i < NUM_VSAVES; i++) {
        int rc = verify_vsave(i, NULL);
        if (rc < 0) {
            verify_ok = 0;
            printf("  \033[91mFAIL\033[0m post-rotate verify vsave %d (rc=%d)\n", i, rc);
        }
    }

    if (verify_ok)
        printf("\n  \033[92mPASS\033[0m -- all %d vsaves rotated and verified\n", NUM_VSAVES);
    else
        printf("\n  \033[91mFAIL\033[0m -- post-rotate verification failed\n");

done:
    while (1)
        usleep(100 * 1000);
    return 0;
}

#endif /* OF_SAVE_TEST_H */
