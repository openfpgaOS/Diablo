//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * slotdemo — list every file the launcher exposed to this app
 *
 * Canonical example of:
 *   - POSIX opendir/readdir/stat against the kernel's virtual root
 *   - Reading the APF data-slot ID from `dirent.d_ino` (kernel maps
 *     `slot:N` files at inode `N+1` so id 1..N are first-class)
 *
 * Apps don't need to call of_file_slot_register() to enumerate slots
 * — the launcher's instance.json already mapped each filename to a
 * slot, and the kernel discovers them at boot.  Use this to confirm
 * that your instance.json data_slots list matches what the running
 * app sees.
 *
 * Controls: none — prints once and parks.
 */

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "of.h"

static void park(void) {
    for (;;) usleep(100 * 1000);
}

int main(void) {
    printf("\033[2J\033[H");
    printf("\n  \033[93mSlot Demo — File Discovery\033[0m\n\n");

    DIR *d = opendir("/");
    if (!d) { printf("  opendir failed\n"); park(); }

    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        /* d_ino is N+1 for slot:N — subtract one to get the APF slot id
         * that's printable next to the filename. */
        int slot = (int)e->d_ino - 1;
        struct stat st;
        long sz = (stat(e->d_name, &st) == 0) ? (long)st.st_size : -1;
        printf("  slot %2d: %-16s %8ld B\n", slot, e->d_name, sz);
        count++;
    }
    closedir(d);

    printf("\n  %d file(s)\n", count);
    park();
    return 0;
}
