/* CrugOS kernel image: loaded and entered at its physical address. */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_BASE = 0x00100000;

SECTIONS
{
    . = KERNEL_BASE;

    __text_start = .;
    .text : ALIGN(4096)
    {
        *(.text .text.*)
    }
    __text_end = .;

    .rodata : ALIGN(4096)
    {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(4096)
    {
        *(.data .data.*)
    }

    __bss_start = .;
    .bss : ALIGN(4096)
    {
        *(COMMON)
        *(.bss .bss.*)
    }
    __bss_end = .;

    __kernel_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.rela .rela.*)
        *(.dynsym .dynstr .hash .gnu.hash)
        *(.dynamic .interp)
    }
}
