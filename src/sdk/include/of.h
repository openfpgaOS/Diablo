/*
 * of.h -- openfpgaOS Application API
 *
 * One header that pulls in every openfpgaOS-specific subsystem (video,
 * audio, input, mixer, GPU, ...). Standard C library functions come
 * from musl directly: include <stdio.h>/<string.h>/<math.h>/<stdlib.h>
 * as you would on any Linux system. Apps statically link musl through
 * the bundled riscv32-elf toolchain; the kernel implements the Linux
 * syscall surface that musl emits.
 *
 * On PC, compile with -DOF_PC and link SDL2.
 *
 *   #include "of.h"
 *   #include <stdio.h>
 *
 *   int main(void) {
 *       of_video_init();
 *       printf("openfpgaOS booted!\n");
 *       while (1) {
 *           of_input_poll();
 *           if (of_btn(OF_BTN_A)) shoot();
 *           of_video_clear(0);
 *           draw_world();
 *           of_video_flip();
 *       }
 *   }
 *
 * Platform: Analogue Pocket FPGA, RISC-V (VexiiRiscv) @ 100 MHz
 * Video:    320x240, 8-bit indexed color, triple-buffered
 * Audio:    48 kHz PCM FIFO + 32-voice mixer; sample-based
 *           MIDI synth (SF2 → .ofsf banks)
 * Input:    2 controllers, d-pad + ABXY + L/R + sticks + triggers
 * Memory:   64 MB SDRAM, 16 MB CRAM0
 */

#ifndef OF_H
#define OF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Foundation headers */
#include "of_error.h"
#include "of_version.h"
#include "of_syscall_numbers.h"

#ifndef OF_PC
#include "of_syscall.h"
#endif

/* Per-module headers */
#include "of_video.h"
#include "of_audio.h"
#include "of_input.h"
#include "of_timer.h"
#include "of_file.h"
#include "of_mount.h"
#include "of_net.h"
#include "of_analogizer.h"
#include "of_terminal.h"
#include "of_cache.h"
#include "of_interact.h"
#include "of_mixer.h"
#include "of_codec.h"
#include "of_lzw.h"
#include "of_fastram.h"
#include "of_midi.h"
#include "of_caps.h"
#include "of_services.h"
#include "of_config.h"
/* Note: of_gpu.h is intentionally NOT included here -- it has per-app
 * static state (ring buffer pointers) and must be included from exactly
 * one TU. Standard C library functions come from upstream musl headers
 * (<stdio.h>, <string.h>, <math.h>, ...); the SDK doesn't ship its own. */

/* ======================================================================
 * System
 * ====================================================================== */

#ifndef OF_PC

static inline void of_exit(void) {
    /* Linux-compat path: SYS_exit (93) is in the Linux EID range and
     * is dispatched by the kernel via the Linux convention, not SBI. */
    __of_linux_syscall0(93);
    __builtin_unreachable();
}

#else /* OF_PC */

void of_exit(void);

#endif /* OF_PC */

/* ======================================================================
 * Initialization (OF_PC only -- on FPGA, the kernel handles init)
 * ====================================================================== */

#ifdef OF_PC
static inline void of_init(void) {
    of_video_init();
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* OF_H */
