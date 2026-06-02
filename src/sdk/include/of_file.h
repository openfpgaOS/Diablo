//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_file.h -- File I/O for openfpgaOS
 *
 * Use standard C fopen/fread/fwrite/fclose/fseek/ftell.
 *   fopen("data.bin", "rb")       -- opens a read-only APF data file
 *   fopen("MyGame_0.sav", "wb")  -- opens a read/write save file
 *
 * Use opendir/readdir to discover available files.
 */

#ifndef OF_FILE_H
#define OF_FILE_H

#include <stdint.h>
#include <stddef.h>  /* offsetof, for the service-table index guard */

/* Standard file I/O is via POSIX (fopen/fread/fwrite/fclose).
 * The of_file_* helpers below are advanced async DMA reads only. */

#ifndef OF_PC

#include "of_syscall.h"
#include "of_syscall_numbers.h"
#include "of_services.h"

/* Register a filename→data-slot binding for fopen() by name.
 *
 * The kernel auto-discovers APF filenames at boot. Apps can still call
 * this when they want to provide or override a binding explicitly.
 * Overwrites any prior mapping with the same name.
 * Max 32 registrations total.
 *
 *   of_file_slot_register(3, "data.bin");
 *   FILE *f = fopen("data.bin", "rb");
 */
static inline void of_file_slot_register(uint32_t slot_id, const char *filename) {
    OF_SVC->file_slot_register(slot_id, filename);
}

/* Resolve a filename to its APF data-slot id.
 * The match is case-insensitive and uses the basename, matching fopen()
 * resolution. Returns 0 on success and writes *slot_id_out when non-NULL;
 * returns <0 when the file is not registered/found. */
static inline int of_file_slot_find(const char *filename,
                                    uint32_t *slot_id_out) {
    struct of_sbiret r = of_ecall1(OF_EID_FILE, OF_FILE_FID_SLOT_FIND,
                                   (long)filename);
    if (r.error)
        return (int)r.error;
    if (slot_id_out)
        *slot_id_out = (uint32_t)r.value;
    return 0;
}

/* Allocate CRAM0 staging memory suitable for of_file_read_async().
 * The returned pointer is CPU-addressable CRAM0 and remains valid until
 * of_file_dma_stage_reset() or app exit. Allocate a few fixed-size buffers at
 * startup, then reuse them for async reads. Returns NULL on failure. */
static inline void *of_file_dma_stage_alloc(uint32_t size, uint32_t align) {
    struct of_sbiret r = of_ecall2(OF_EID_FILE, OF_FILE_FID_DMA_STAGE_ALLOC,
                                   (long)size, (long)align);
    if (r.error)
        return (void *)0;
    return (void *)r.value;
}

/* Reset the app DMA staging allocator. Fails with OF_ERR_BUSY if an async
 * read is currently in flight. */
static inline int of_file_dma_stage_reset(void) {
    struct of_sbiret r = of_ecall0(OF_EID_FILE, OF_FILE_FID_DMA_STAGE_RESET);
    return (int)r.error;
}

/* Maximum byte count accepted by one of_file_read_async() request. */
static inline uint32_t of_file_async_max_read(void) {
    struct of_sbiret r = of_ecall0(OF_EID_FILE, OF_FILE_FID_ASYNC_MAX_READ);
    return r.error ? 0u : (uint32_t)r.value;
}

/* Total bytes reserved for app-visible CRAM0 async staging. */
static inline uint32_t of_file_dma_stage_size(void) {
    struct of_sbiret r = of_ecall0(OF_EID_FILE, OF_FILE_FID_DMA_STAGE_SIZE);
    return r.error ? 0u : (uint32_t)r.value;
}

/* Start a non-blocking file read from a data slot.
 * dest can point to normal SDRAM.  CRAM0 destinations allocated with
 * of_file_dma_stage_alloc() use the zero-copy direct path; other destinations
 * are copied from an internal CRAM0 bounce buffer on completion.
 * callback(token, result) fires from the data-slot completion IRQ:
 * result=0 success, <0 error.
 * Only one async read in flight at a time (bridge limitation).
 * Returns token >= 0 on success, < 0 if busy or error. */
static inline int of_file_read_async(int slot_id, uint32_t offset,
                                     void *dest, uint32_t length,
                                     void (*callback)(int token, int result)) {
    return of_sbi_ret_int(of_ecall5(OF_EID_FILE, OF_FILE_FID_READ_ASYNC,
                                    slot_id, (long)offset, (long)dest,
                                    (long)length, (long)callback));
}

/* Poll async read progress. Optional on Pocket: completion is IRQ-driven.
 * Returns 1 once per completed read, 0 otherwise. */
static inline int of_file_async_poll(void) {
    return of_sbi_ret_int(of_ecall0(OF_EID_FILE, OF_FILE_FID_ASYNC_POLL));
}

/* Check if an async read is in flight. */
static inline int of_file_async_busy(void) {
    return of_sbi_ret_int(of_ecall0(OF_EID_FILE, OF_FILE_FID_ASYNC_BUSY));
}

/* Index of a service-table function pointer (relative to the first entry),
 * matching the kernel's count = (sizeof(table) - header)/sizeof(void*). Used
 * to gate a call on OF_SVC->count for forward/backward ABI compatibility. */
#ifndef OF_FILE_SVC_INDEX
#define OF_FILE_SVC_INDEX(field) \
    ((uint32_t)((offsetof(struct of_services_table, field) - \
                 offsetof(struct of_services_table, video_init)) / sizeof(void *)))
#endif

/* Register a hook invoked while a BLOCKING file read waits on its DMA, so the
 * app can keep the audio ring fed from already-decoded data and music does not
 * stall during SD access. The hook MUST NOT itself issue a blocking file read
 * (kernel suppresses nested invocation). Pass NULL to clear. No-op on an older
 * OS that predates this service-table entry. */
static inline void of_file_set_idle_hook(void (*hook)(void)) {
    if (OF_SVC->count > OF_FILE_SVC_INDEX(file_set_idle_hook) &&
        OF_SVC->file_set_idle_hook) {
        OF_SVC->file_set_idle_hook(hook);
    }
}

#endif /* OF_PC */

#endif /* OF_FILE_H */
