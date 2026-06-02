//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * of_syscall_numbers.h -- openfpgaOS SBI Extension/Function IDs
 *
 * Single source of truth for the openfpgaOS vendor-extension namespace
 * of the RISC-V SBI calling convention. See of_syscall.h for the ABI
 * spec and docs/syscall-abi.md for the long-form rationale.
 *
 * ============================================================================
 * EID layout
 * ============================================================================
 *
 *   0x00000000 .. 0x000001FF   Linux RISC-V syscalls (musl POSIX)
 *   0x00000200 .. 0x0FFFFFFF   Reserved (do not use)
 *   0x10000000 .. 0x2FFFFFFF   Reserved for future standard SBI extensions
 *   0xC0DE0000 .. 0xC0DE00FF   openfpgaOS vendor extensions  <-- this file
 *
 * The 0xC0DE0000 prefix has the high bit set, putting it unambiguously
 * in the SBI vendor range and well above any plausible Linux syscall
 * number expansion.
 *
 * EIDs are append-only: never reuse a retired EID, never insert a new
 * EID in the middle of the list.
 *
 * ============================================================================
 * FID layout
 * ============================================================================
 *
 * Each subsystem has its own enum of function IDs, starting at 0 and
 * growing append-only. There is no shared FID namespace -- a FID is
 * only meaningful in combination with its EID.
 *
 * Reserve no slots for "future use" -- just append. Linux did this and
 * it works fine; reserved gaps invariably get reused incorrectly.
 */

#ifndef OF_SYSCALL_NUMBERS_H
#define OF_SYSCALL_NUMBERS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Vendor extension base
 * ============================================================================ */

#define OF_EID_BASE_VALUE   0xC0DE0000u

/* Test whether an EID belongs to the openfpgaOS vendor range. */
#define OF_EID_IS_VENDOR(eid) \
    (((unsigned long)(eid) & 0xFFFFFF00u) == OF_EID_BASE_VALUE)

/* ============================================================================
 * Extension IDs (EIDs)
 *
 * Append-only. Numbered sequentially within the vendor base. Each EID
 * gets its own enum of FIDs below.
 * ============================================================================ */

#define OF_EID_BASE         (OF_EID_BASE_VALUE + 0x00)  /* version, caps    */
#define OF_EID_VIDEO        (OF_EID_BASE_VALUE + 0x01)
#define OF_EID_AUDIO        (OF_EID_BASE_VALUE + 0x02)
#define OF_EID_INPUT        (OF_EID_BASE_VALUE + 0x03)
#define OF_EID_ANALOGIZER   (OF_EID_BASE_VALUE + 0x04)
#define OF_EID_NET          (OF_EID_BASE_VALUE + 0x05)
#define OF_EID_TIMER        (OF_EID_BASE_VALUE + 0x06)
/* 0x07/0x08 retired (tile/sprite engine) */
#define OF_EID_MEMORY       (OF_EID_BASE_VALUE + 0x09)
#define OF_EID_MIXER        (OF_EID_BASE_VALUE + 0x0A)
#define OF_EID_CODEC        (OF_EID_BASE_VALUE + 0x0B)
#define OF_EID_LZW          (OF_EID_BASE_VALUE + 0x0C)
#define OF_EID_INTERACT     (OF_EID_BASE_VALUE + 0x0D)
#define OF_EID_FILE         (OF_EID_BASE_VALUE + 0x0E)

/* ============================================================================
 * Function IDs (FIDs) -- one enum per subsystem.
 *
 * APPEND ONLY. Never reorder, renumber, or delete entries -- old binaries
 * still call by FID number. If a function is retired, leave the slot
 * occupied with an unused enumerator (or a `_RESERVED_N` placeholder)
 * and add the replacement at the next free slot.
 * ============================================================================ */

/* -- OF_EID_BASE -- */
enum of_base_fid {
    OF_BASE_FID_GET_VERSION = 0,
};

/* -- OF_EID_VIDEO -- */
enum of_video_fid {
    OF_VIDEO_FID_INIT                = 0,
    OF_VIDEO_FID_FLIP                = 1,
    OF_VIDEO_FID_WAIT_FLIP           = 2,
    OF_VIDEO_FID_SET_PALETTE         = 3,
    OF_VIDEO_FID_GET_SURFACE         = 4,
    OF_VIDEO_FID_SET_DISPLAY_MODE    = 5,
    OF_VIDEO_FID_CLEAR               = 6,
    OF_VIDEO_FID_SET_PALETTE_BULK    = 7,
    OF_VIDEO_FID_FLUSH_CACHE         = 8,
    OF_VIDEO_FID_SET_COLOR_MODE      = 9,
    OF_VIDEO_FID_SET_PALETTE_VGA4    = 10,
    OF_VIDEO_FID_VSYNC               = 11,
    OF_VIDEO_FID_SET_VSYNC_CALLBACK  = 12,
    OF_VIDEO_FID_SET_MODE            = 13,
    OF_VIDEO_FID_GET_MODE            = 14,
    OF_VIDEO_FID_GET_MODE_COUNT      = 15,
    OF_VIDEO_FID_GET_MODE_INFO       = 16,
    OF_VIDEO_FID_GET_CAPS            = 17,
    OF_VIDEO_FID_CHECK_MODE          = 18,
};

/* -- OF_EID_AUDIO -- */
enum of_audio_fid {
    OF_AUDIO_FID_INIT      = 0,
    OF_AUDIO_FID_WRITE     = 1,
    OF_AUDIO_FID_GET_FREE  = 2,
};

/* -- OF_EID_INPUT -- */
enum of_input_fid {
    OF_INPUT_FID_POLL         = 0,
    OF_INPUT_FID_GET_STATE    = 1,
    OF_INPUT_FID_SET_DEADZONE = 2,
    OF_INPUT_FID_POLL_P0      = 3,
    OF_INPUT_FID_GET_KEYBOARD_STATE = 4,
    OF_INPUT_FID_READ_MOUSE_STATE   = 5,
};

/* -- OF_EID_ANALOGIZER -- */
enum of_analogizer_fid {
    OF_ANALOGIZER_FID_GET_STATE  = 0,
    OF_ANALOGIZER_FID_IS_ENABLED = 1,
};

/* -- OF_EID_NET -- */
enum of_net_fid {
    OF_NET_FID_HOST_START   = 0,
    OF_NET_FID_JOIN         = 1,
    OF_NET_FID_STOP         = 2,
    OF_NET_FID_STATUS       = 3,
    OF_NET_FID_CLIENT_COUNT = 4,
    OF_NET_FID_SEND_TO      = 5,
    OF_NET_FID_RECV_FROM    = 6,
    OF_NET_FID_BROADCAST    = 7,
    OF_NET_FID_SEND         = 8,
    OF_NET_FID_RECV         = 9,
    OF_NET_FID_POLL         = 10,
};

/* -- OF_EID_TIMER -- */
enum of_timer_fid {
    OF_TIMER_FID_SET_CALLBACK = 0,
    OF_TIMER_FID_STOP         = 1,
    OF_TIMER_FID_GET_US       = 2,
    OF_TIMER_FID_GET_MS       = 3,
    OF_TIMER_FID_DELAY_US     = 4,
};

/* -- OF_EID_MEMORY -- */
enum of_memory_fid {
    OF_MEMORY_FID_MALLOC  = 0,
    OF_MEMORY_FID_FREE    = 1,
    OF_MEMORY_FID_REALLOC = 2,
    OF_MEMORY_FID_CALLOC  = 3,
};

/* -- OF_EID_MIXER -- */
enum of_mixer_fid {
    OF_MIXER_FID_INIT              = 0,
    OF_MIXER_FID_PLAY              = 1,
    OF_MIXER_FID_STOP              = 2,
    OF_MIXER_FID_STOP_ALL          = 3,
    OF_MIXER_FID_SET_VOLUME        = 4,
    OF_MIXER_FID_PUMP              = 5,
    OF_MIXER_FID_VOICE_ACTIVE      = 6,
    OF_MIXER_FID_SET_PAN           = 7,
    OF_MIXER_FID_SET_LOOP          = 8,
    OF_MIXER_FID_SET_RATE          = 9,
    OF_MIXER_FID_SET_VOL_LR        = 10,
    OF_MIXER_FID_SET_BIDI          = 11,
    OF_MIXER_FID_GET_POSITION      = 12,
    OF_MIXER_FID_SET_POSITION      = 13,
    OF_MIXER_FID_SET_VOICE         = 14,
    OF_MIXER_FID_ALLOC_SAMPLES     = 15,
    OF_MIXER_FID_FREE_SAMPLES      = 16,
    OF_MIXER_FID_SET_RATE_RAW      = 17,
    OF_MIXER_FID_SET_VOICE_RAW     = 18,
    OF_MIXER_FID_SET_VOLUME_RAMP   = 19,
    OF_MIXER_FID_POLL_ENDED        = 20,
    OF_MIXER_FID_SET_END_CALLBACK  = 21,
    OF_MIXER_FID_RETRIGGER         = 22,
    OF_MIXER_FID_PLAY_8BIT         = 23,
    OF_MIXER_FID_SET_GROUP         = 24,
    OF_MIXER_FID_SET_GROUP_VOLUME  = 25,
    OF_MIXER_FID_SET_MASTER_VOLUME = 26,
};

/* -- OF_EID_CODEC -- */
enum of_codec_fid {
    OF_CODEC_FID_PARSE_VOC = 0,
    OF_CODEC_FID_PARSE_WAV = 1,
};

/* -- OF_EID_LZW -- */
enum of_lzw_fid {
    OF_LZW_FID_COMPRESS   = 0,
    OF_LZW_FID_UNCOMPRESS = 1,
};

/* -- OF_EID_INTERACT -- */
enum of_interact_fid {
    OF_INTERACT_FID_GET = 0,
};

/* -- OF_EID_FILE -- */
enum of_file_fid {
    OF_FILE_FID_READ_ASYNC = 0,
    OF_FILE_FID_ASYNC_POLL = 1,
    OF_FILE_FID_ASYNC_BUSY = 2,
    OF_FILE_FID_MOUNT = 3,
    OF_FILE_FID_UMOUNT = 4,
    OF_FILE_FID_SLOT_FIND = 5,
    OF_FILE_FID_DMA_STAGE_ALLOC = 6,
    OF_FILE_FID_DMA_STAGE_RESET = 7,
    OF_FILE_FID_ASYNC_MAX_READ = 8,
    OF_FILE_FID_DMA_STAGE_SIZE = 9,
};

#ifdef __cplusplus
}
#endif

#endif /* OF_SYSCALL_NUMBERS_H */
