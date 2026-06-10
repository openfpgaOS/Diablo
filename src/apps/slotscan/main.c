//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * slotscan — probe APF data slots 0..63 to find the host's data-slot ceiling
 *
 * Why this exists
 * ---------------
 * openfpgaOS reads each data slot's metadata from the APF datatable, which is
 * indexed by *array position*, not by slot id.  The old id->position map in
 * targets/pocket/file.c only covered ids 0-19, so the OS simply could not
 * address higher slots.  file.c now falls back to a datatable *scan* — each
 * entry's word0[15:0] holds that entry's slot id — so any id the fixed map
 * rejects is still resolvable.
 *
 * That removes openfpgaOS's own ceiling.  The remaining unknown is the
 * Analogue Pocket *host* firmware: it parses the core's data.json and DMAs a
 * data-slot table into the datatable BRAM (which holds 512 entries), but only
 * for as many slots as the host's internal table supports.  That host cap is
 * not visible from our code — the only way to measure it is empirically.
 *
 * How it measures
 * ---------------
 * fopen("slot:N") succeeds only when the host actually populated slot N with a
 * non-empty file: the kernel open path checks of_file_flags(N)==N and
 * of_file_size64(N)>0, both routed through the new datatable scan.  The
 * instance.json binds tiny marker files (sNN.bin) to a spread of high slots;
 * the highest *bound* slot that reports present is the host's ceiling.  Each
 * marker file names its own slot, so a present slot whose marker matches its
 * id proves the scan returned the correct datatable entry (no off-by-one).
 *
 * Controls: none — prints once and parks.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "of.h"

#define MAX_PROBE 40

/* APF allows a maximum of 32 data slots (any 16-bit id).  This core declares
 * the standard ids 0-8,10-19 plus regular (deferload) data slots at 20,24,28
 * — 22 slots total, comfortably under the 32 cap.  If they read present, the
 * host populates high-id regular data slots and the OS datatable scan reads
 * them; we can then use ids up to a 32-slot budget (no need to refit into
 * 0-19).  Exceeding 32 slots is what triggers the host's "can't match slot
 * id" — keep the declared count <= 32. */
static const int test_ids[] = { 20, 24, 28 };
#define N_TEST ((int)(sizeof(test_ids) / sizeof(test_ids[0])))

static int is_test_id(int n) {
    for (int i = 0; i < N_TEST; i++)
        if (test_ids[i] == n)
            return 1;
    return 0;
}

/* Returns file size if slot N is present (host-populated, non-empty), else -1.
 * Writes a NUL-terminated first-line marker into marker_out when present. */
static long probe_slot(int n, char *marker_out, size_t marker_max) {
    char path[16];
    snprintf(path, sizeof(path), "slot:%d", n);

    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (marker_out && marker_max) {
        size_t got = fread(marker_out, 1, marker_max - 1, f);
        marker_out[got] = '\0';
        for (char *p = marker_out; *p; p++)
            if (*p == '\n' || *p == '\r') { *p = '\0'; break; }
    }
    fclose(f);
    return sz;
}

static void park(void) { for (;;) usleep(100 * 1000); }

int main(void) {
    printf("\033[2J\033[H");
    printf("\n  \033[93mSlot Scan — APF data-slot ceiling probe\033[0m\n\n");

    int present = 0, highest_any = -1, highest_test = -1;

    printf("  Present data slots (fopen \"slot:N\" succeeded):\n");
    for (int n = 0; n < MAX_PROBE; n++) {
        char marker[24] = {0};
        long sz = probe_slot(n, marker, sizeof(marker));
        if (sz < 0)
            continue;
        present++;
        highest_any = n;
        if (is_test_id(n) && n > highest_test)
            highest_test = n;
        printf("    slot %2d  %7ld B  %s%s\n", n, sz, marker,
               is_test_id(n) ? "  \033[96m[test]\033[0m" : "");
    }

    printf("\n  Bound test slots (the ceiling is the highest present one):\n   ");
    for (int i = 0; i < N_TEST; i++) {
        long sz = probe_slot(test_ids[i], NULL, 0);
        printf(" %d:%s", test_ids[i],
               sz >= 0 ? "\033[92mok\033[0m" : "\033[91m--\033[0m");
    }
    printf("\n");

    printf("\n  %d slot(s) populated; highest id = %d\n", present, highest_any);
    if (highest_test >= 20)
        printf("  \033[92m=> host loads slots past id 19; ceiling is at least id %d\033[0m\n",
               highest_test);
    else
        printf("  \033[91m=> no test slot above 19 loaded — host caps at <=19 (or scan off)\033[0m\n");
    if (highest_test == test_ids[N_TEST - 1])
        printf("  => reached id %d with no gap — try declaring even more slots\n",
               highest_test);

    park();
    return 0;
}
