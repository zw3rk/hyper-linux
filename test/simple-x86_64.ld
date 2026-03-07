/* simple-x86_64.ld — Linker script for minimal x86_64-linux test binaries
 *
 * Produces a static ET_EXEC with ELF/program headers in the first
 * PT_LOAD at file offset 0.  Rosetta requires p_offset=0 for the
 * first LOAD segment — without it, rosetta rejects the binary.
 *
 * Uses FILEHDR + PHDRS in the PHDRS command so the linker explicitly
 * places the ELF and program headers into the segment.
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS {
    text PT_LOAD FILEHDR PHDRS FLAGS(5);  /* PF_R | PF_X + ELF headers */
}

SECTIONS {
    . = 0x400000 + SIZEOF_HEADERS;

    .text : { *(.text) *(.text.*) } :text
    .rodata : { *(.rodata) *(.rodata.*) } :text
    .data : { *(.data) *(.data.*) } :text
    .bss  : { *(.bss) *(.bss.*) } :text

    /DISCARD/ : { *(.comment) *(.note.*) }
}
