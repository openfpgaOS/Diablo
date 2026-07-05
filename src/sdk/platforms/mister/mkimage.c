//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * openfpgaOS SDK — MiSTer disk-image builder.
 *
 * Assembles the FAT32 .vhd that the openfpgaOS MiSTer core mounts via
 * the OSD.  Built on FatFs itself (the same implementation the firmware
 * uses to mount the image), so format compatibility is exact and the
 * nonvolatile slot files are preallocated CONTIGUOUSLY with f_expand —
 * the firmware's power-cut safety story depends on never touching FAT
 * metadata during a save write.
 *
 * Image layout (the slot→path contract in targets/mister/file.c):
 *   /os.ini                  OS config        (slot 2)
 *   /app.elf                 default app      (slot 3)
 *   /bank.ofsf               MIDI soundfont   (slot 7, optional)
 *   /config/shared.cfg       SDK shared cfg   (slot 8,  256 KB prealloc)
 *   /config/duke3d.cfg       per-game cfg     (slot 9,  256 KB prealloc)
 *   /saves/slot_0..9.sav     save slots       (10-19,   256 KB prealloc)
 *   /assets/<files>          app data, registered by directory scan
 *
 * Usage:
 *   mkimage [--no-default-nv] <out.vhd> <size_mb> [src=dst]...
 *   mkimage --list <img.vhd>
 *
 * Each src=dst copies a host file into the image (dst is an absolute
 * in-image path, e.g. game.elf=/app.elf or sfx.bin=/assets/sfx.bin).
 *
 * --no-default-nv omits the hardcoded legacy root-level slots (/saves/*,
 * /config/*) so callers can build a pure read-only shell or a pure
 * per-instance saves shell (its only slots come from nv= specs).
 * --list dumps an image's FAT tree with each file's size + contiguity.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "fatfs/ff.h"
#include "fatfs/diskio.h"

#define SECTOR 512u
#define NV_SLOT_BYTES (256u * 1024u)
/* Must match the firmware's OF_TARGET_SAVE_MAX_SLOTS
 * (openfpgaOS: src/firmware/os/targets/mister/target_platform.h) — the
 * runtime maps save slot ids 10..(10+N-1) onto these files and refuses
 * writes to files that don't exist at full size. */
#define NV_SAVE_SLOTS 10

/* ── file-backed diskio ─────────────────────────────────────────── */

static int img_fd = -1;
static LBA_t img_sectors;

DSTATUS disk_status(BYTE pdrv) { (void)pdrv; return img_fd >= 0 ? 0 : STA_NOINIT; }
DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    ssize_t n = pread(img_fd, buff, (size_t)count * SECTOR, (off_t)sector * SECTOR);
    return n == (ssize_t)((size_t)count * SECTOR) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    ssize_t n = pwrite(img_fd, buff, (size_t)count * SECTOR, (off_t)sector * SECTOR);
    return n == (ssize_t)((size_t)count * SECTOR) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    (void)pdrv;
    switch (cmd) {
    case CTRL_SYNC:        return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t *)buff = img_sectors; return RES_OK;
    case GET_SECTOR_SIZE:  *(WORD *)buff = SECTOR; return RES_OK;
    case GET_BLOCK_SIZE:   *(DWORD *)buff = 1; return RES_OK;
    default:               return RES_PARERR;
    }
}

/* ── helpers ────────────────────────────────────────────────────── */

static void die(const char *what, int rc) {
    fprintf(stderr, "mkimage: %s failed (%d)\n", what, rc);
    exit(1);
}

/* Create parent directories for an in-image path — FatFs f_mkdir does not
 * auto-create parents, so nested dsts (e.g. /Doom/Plutonia/slot_0.sav) need
 * each component made in turn.  FR_EXIST is fine and ignored. */
static void mkdirs(const char *dst) {
    char tmp[256];
    size_t n = strlen(dst);
    if (n >= sizeof(tmp)) die("path too long", 0);
    memcpy(tmp, dst, n + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            f_mkdir(tmp);          /* FR_EXIST ignored */
            *p = '/';
        }
    }
}

static void make_nv_slot(const char *path) {
    FIL f;
    mkdirs(path);
    FRESULT fr = f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) die(path, fr);
    fr = f_expand(&f, NV_SLOT_BYTES, 1);     /* contiguous, or fail */
    if (fr != FR_OK) die("f_expand", fr);
    /* Zero the payload so logically-empty slots read deterministically. */
    static BYTE zeros[4096];
    UINT bw;
    for (UINT done = 0; done < NV_SLOT_BYTES; done += sizeof(zeros)) {
        fr = f_write(&f, zeros, sizeof(zeros), &bw);
        if (fr != FR_OK || bw != sizeof(zeros)) die("zero-fill", fr);
    }
    f_close(&f);
    printf("  nv   %s (%u KB, contiguous)\n", path, NV_SLOT_BYTES / 1024);
}

static void copy_in(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) { fprintf(stderr, "mkimage: cannot open %s\n", src); exit(1); }

    FIL f;
    mkdirs(dst);
    FRESULT fr = f_open(&f, dst, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) die(dst, fr);

    static BYTE buf[65536];
    size_t n;
    UINT bw;
    long total = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fr = f_write(&f, buf, (UINT)n, &bw);
        if (fr != FR_OK || bw != n) die("f_write", fr);
        total += (long)n;
    }
    f_close(&f);
    fclose(in);
    printf("  copy %s -> %s (%ld bytes)\n", src, dst, total);
}

/* ── list mode: dump the FAT tree with per-file size + contiguity ──── */

static FATFS list_fs;

/* A file is contiguous iff cluster i of its chain is sclust+i for every i.
 * f_lseek walks the real FAT chain, so fp->clust after seeking into cluster i
 * is that cluster's true number — compare against the contiguous ideal.  (Seek
 * to the LAST byte of each cluster: at fptr 0 fp->clust is undefined, so a
 * non-zero offset is required.) */
static int is_contiguous(FIL *fp, WORD csize) {
    FSIZE_t sz = fp->obj.objsize;
    if (sz == 0) return 1;
    DWORD sclust = fp->obj.sclust;
    FSIZE_t cbytes = (FSIZE_t)csize * SECTOR;
    DWORD nclust = (DWORD)((sz + cbytes - 1) / cbytes);
    for (DWORD i = 0; i < nclust; i++) {
        FSIZE_t last = (FSIZE_t)(i + 1) * cbytes - 1;
        if (last > sz - 1) last = sz - 1;      /* clamp into the file */
        if (f_lseek(fp, last) != FR_OK) return 0;
        if (fp->clust != sclust + i) return 0;
    }
    return 1;
}

static void list_walk(const char *dir) {
    DIR d;
    FILINFO fno;
    if (f_opendir(&d, dir) != FR_OK) return;
    for (;;) {
        if (f_readdir(&d, &fno) != FR_OK || fno.fname[0] == 0) break;
        char child[300];
        snprintf(child, sizeof(child), "%s%s%s",
                 dir, (dir[0] == '/' && dir[1] == 0) ? "" : "/", fno.fname);
        if (fno.fattrib & AM_DIR) {
            printf("  dir   %s/\n", child);
            list_walk(child);
        } else {
            FIL f;
            const char *tag = "?";
            if (f_open(&f, child, FA_READ) == FR_OK) {
                tag = is_contiguous(&f, list_fs.csize) ? "contiguous" : "FRAGMENTED";
                f_close(&f);
            }
            printf("  file  %s  %llu B  %s\n",
                   child, (unsigned long long)fno.fsize, tag);
        }
    }
    f_closedir(&d);
}

static int list_image(const char *path) {
    img_fd = open(path, O_RDONLY);
    if (img_fd < 0) { perror(path); return 1; }
    struct stat st;
    if (fstat(img_fd, &st) != 0) { perror("fstat"); return 1; }
    img_sectors = (LBA_t)(st.st_size / SECTOR);
    FRESULT fr = f_mount(&list_fs, "", 1);
    if (fr != FR_OK) { fprintf(stderr, "mkimage --list: mount failed (%d)\n", fr); return 1; }
    printf("list: %s  (%lld bytes, cluster=%u sectors / %u B)\n",
           path, (long long)st.st_size, list_fs.csize,
           (unsigned)(list_fs.csize * SECTOR));
    list_walk("/");
    f_unmount("");
    close(img_fd);
    printf("list: done\n");
    return 0;
}

/* ── main ───────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    /* List mode: dump an existing image's FAT tree (path + size + contiguity). */
    if (argc >= 2 && strcmp(argv[1], "--list") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: %s --list <img.vhd>\n", argv[0]); return 1; }
        return list_image(argv[2]);
    }

    /* Optional build flags before <out> <size>.  --no-default-nv skips the
     * hardcoded legacy root-level slots (/saves/slot_*.sav, /config/*.cfg) so
     * the caller can build a pure read-only shell (common only) or a pure
     * per-instance saves shell whose only slots come from nv= specs. */
    int no_default_nv = 0;
    int ai = 1;
    for (; ai < argc && argv[ai][0] == '-' && argv[ai][1] == '-'; ai++) {
        if (strcmp(argv[ai], "--no-default-nv") == 0) no_default_nv = 1;
        else { fprintf(stderr, "mkimage: unknown flag '%s'\n", argv[ai]); return 1; }
    }

    if (argc - ai < 2) {
        fprintf(stderr, "usage: %s [--no-default-nv] <out.vhd> <size_mb> [src=dst|nv=/dst]...\n", argv[0]);
        fprintf(stderr, "       %s --list <img.vhd>\n", argv[0]);
        return 1;
    }

    const char *out = argv[ai];
    long mb = strtol(argv[ai + 1], NULL, 10);
    int spec_start = ai + 2;
    if (mb < 48 || mb > 4095) {
        fprintf(stderr, "mkimage: size must be 48..4095 MB (FAT32)\n");
        return 1;
    }

    img_fd = open(out, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (img_fd < 0) { perror(out); return 1; }
    if (ftruncate(img_fd, (off_t)mb * 1024 * 1024) != 0) { perror("ftruncate"); return 1; }
    img_sectors = (LBA_t)((unsigned long long)mb * 1024 * 1024 / SECTOR);

    static FATFS fs;
    static BYTE work[64 * 1024];
    MKFS_PARM parm = { FM_FAT32 | FM_SFD, 0, 0, 0, 0 };  /* no MBR — bare FAT */
    FRESULT fr = f_mkfs("", &parm, work, sizeof(work));
    if (fr != FR_OK) die("f_mkfs", fr);
    fr = f_mount(&fs, "", 1);
    if (fr != FR_OK) die("f_mount", fr);

    printf("mkimage: %s (%ld MB FAT32)\n", out, mb);

    /* Legacy single-image layout: fixed root-level /saves + /config slots.
     * Skipped with --no-default-nv (the per-game S0 boot shell has NO slots;
     * the S1 saves shell carries only per-instance nv= specs at
     * /<Game>/<Instance>/...). */
    if (!no_default_nv) {
        f_mkdir("/saves");
        f_mkdir("/config");
        f_mkdir("/assets");

        /* Nonvolatile slots — preallocated, contiguous, zeroed. */
        char path[32];
        for (int i = 0; i < NV_SAVE_SLOTS; i++) {
            snprintf(path, sizeof(path), "/saves/slot_%d.sav", i);
            make_nv_slot(path);
        }
        make_nv_slot("/config/shared.cfg");
        /* Legacy fixed slot-9 backing name (see mister file.c slot_path). */
        make_nv_slot("/config/duke3d.cfg");
    }

    /* Payload files.  "nv=/path" specs preallocate an extra nonvolatile
     * slot instead of copying — used for the per-app config
     * (nv=/config/<app>.cfg): apps open their settings BY NAME and the
     * OS enumerates /config, so the file just has to exist. */
    for (int i = spec_start; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq || eq == argv[i] || eq[1] != '/') {
            fprintf(stderr, "mkimage: bad spec '%s' (want src=/dst or nv=/dst)\n", argv[i]);
            return 1;
        }
        *eq = '\0';
        if (strcmp(argv[i], "nv") == 0)
            make_nv_slot(eq + 1);
        else
            copy_in(argv[i], eq + 1);
    }

    f_unmount("");
    close(img_fd);
    printf("mkimage: done\n");
    return 0;
}
