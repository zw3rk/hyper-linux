/* simple-x86_64.ld — Linker script for minimal x86_64-linux test binaries
 *
 * Places .text at 0x400000 (standard x86_64 load base).
 * Produces a static ET_EXEC with a single PT_LOAD segment.
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS {
    . = 0x400000;

    .text : {
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    .bss : {
        *(.bss)
        *(.bss.*)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
    }
}
