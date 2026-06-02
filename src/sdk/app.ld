/*----------------------------------------------------------------------------*/
/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileType: SOURCE */
/* SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com> */
/*----------------------------------------------------------------------------*/

/*
 * openfpgaOS Application Linker Script
 *
 * Places SDK apps at the address the kernel ELF loader expects, with
 * an optional BRAM hot-path section for performance-critical code.
 *
 * Apps statically link upstream musl, so the entry point is musl's
 * own _start (from crt1.o), which calls __libc_start_main, which calls
 * main(). The kernel loader sets up argc/argv/envp on the stack in
 * standard SysV ABI format before jumping to _start.
 *
 * BRAM hot path: mark functions with OF_FASTTEXT (or OF_FASTDATA /
 * OF_FASTRODATA) -- see api/of_fastram.h. Marked symbols land in BRAM
 * (~23 KB, zero wait state). The OS ELF loader detects BRAM-targeted
 * PT_LOAD segments and copies them at load time. Apps that don't use
 * the macros are unaffected -- the BRAM section stays empty and no
 * extra PT_LOAD is generated.
 */

ENTRY(_start)

MEMORY {
    APP_BRAM (rwx) : ORIGIN = 0x00004000, LENGTH = 0x3800
    /* Ends at 0x13A7C000: below the 4 MB OS file cache, dynamic audio
     * reservation, and 512 KB app stack in the no-SoundFont case. */
    SDRAM    (rwx) : ORIGIN = 0x10400000, LENGTH = 0x0367C000
}

SECTIONS {
    . = 0x10400000;

    .text : {
        *(.text.start)
        *(.text*)
        *(.rodata*)
        *(.srodata*)
        . = ALIGN(4);
    } > SDRAM

    .eh_frame : {
        . = ALIGN(4);
        PROVIDE(__EH_FRAME_BEGIN__ = .);
        KEEP(*(.eh_frame))
        KEEP(*(.eh_frame.*))
        LONG(0);
        PROVIDE(__FRAME_END__ = .);
        . = ALIGN(4);
    } > SDRAM

    /* C++ static constructors / destructors -- musl's __libc_start_main
     * walks these arrays. Keep them even if --gc-sections fires. */
    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > SDRAM

    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > SDRAM

    /* musl _init / _fini: crti.o opens the function frames, the libc
     * payload contributes its bits, crtn.o emits the closing rets. */
    .init : { KEEP(*(.init)) } > SDRAM
    .fini : { KEEP(*(.fini)) } > SDRAM

    .data : {
        *(.data*)
        . = ALIGN(8);
        /* __global_pointer$ -- musl's _start (crt1.o) sets gp to this
         * symbol so gp-relative loads/stores in libc can address the
         * 4 KB window centered on it (.sdata + first .sbss + tail of
         * .data) with signed 12-bit immediates. Without this symbol,
         * the lla gp instruction in _start emits a placeholder
         * relocation that the linker silently leaves as zero, gp
         * becomes a garbage address, and every gp-relative access
         * faults or corrupts memory. The app then hangs in __init_libc
         * before any user code runs. */
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.* .gnu.linkonce.s.*)
        *(.sbss .sbss.* .gnu.linkonce.sb.*)
        *(.scommon)
        . = ALIGN(4);
    } > SDRAM

    .dynamic : {
        *(.dynamic)
    } > SDRAM

    .rela.dyn : {
        *(.rela*)
    } > SDRAM

    .bss : {
        __bss_start = .;
        *(.bss*)
        *(.sbss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > SDRAM

    /* BRAM hot code: VMA in BRAM, LMA stored in ELF after SDRAM sections.
     * Empty for apps that don't use OF_FASTTEXT -- no PT_LOAD generated. */
    .app_fasttext : {
        *(.app_fasttext*)
        *(.app_fastrodata*)
    } > APP_BRAM AT> SDRAM

    .app_fastdata : {
        *(.app_fastdata*)
    } > APP_BRAM AT> SDRAM

    /DISCARD/ : {
        *(.note.*)
        *(.comment)
        *(.riscv.attributes)
    }
}
