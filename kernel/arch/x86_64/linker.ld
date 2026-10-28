OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

KERNEL_BASE = 0x00100000;

SECTIONS
{
    . = KERNEL_BASE;
    __kernel_start = .;

    .text : ALIGN(4096)
    {
        __text_start = .;
        *(.text .text.*)
        __text_end = .;
    }

    .rodata : ALIGN(4096)
    {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    }

    .data : ALIGN(4096)
    {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    }

    .bss : ALIGN(4096)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)

        . = ALIGN(4096);
        __stack_guard = .;
        . += 4096;
        __stack_bottom = .;
        . += 32768;
        __stack_top = .;

        /* Phase H3 IST stacks: a fault on a broken stack still needs a
           writable one.  #DF gets 16 KiB, the other three 8 KiB each. */
        . = ALIGN(4096);
        __df_stack_bottom = .;
        . += 16384;
        __df_stack_top = .;

        . = ALIGN(4096);
        __nmi_stack_bottom = .;
        . += 8192;
        __nmi_stack_top = .;

        . = ALIGN(4096);
        __mc_stack_bottom = .;
        . += 8192;
        __mc_stack_top = .;

        . = ALIGN(4096);
        __db_stack_bottom = .;
        . += 8192;
        __db_stack_top = .;
        __bss_end = .;
    }

    . = ALIGN(4096);
    __kernel_end = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame.*)
        *(.rela .rela.*)
        *(.dynsym)
        *(.dynstr)
        *(.hash)
        *(.gnu.hash)
        *(.dynamic)
        *(.interp)
    }
}
