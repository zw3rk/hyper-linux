/* simple.ld — Linker script for minimal aarch64-linux test binaries
 *
 * Places .text at 0x400000 (within hl's ELF load region).
 * Produces a static ET_EXEC with a single PT_LOAD segment.
 */

OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
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
        *(.ARM.*)
    }
}
