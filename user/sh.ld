/* Link script for the ring-3 shell (phase U3).
 *
 * Two loadable segments so the kernel can map text read+execute and data
 * read+write: the W^X policy in elf_exec.c derives the flags from the segment
 * itself. The text segment sits exactly where user/user.ld puts every other
 * program; data has its own page one MiB higher.
 */
ENTRY(_start)
PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}
SECTIONS
{
    . = 0x0000008000010000;
    .text : {
        *(.text.start)
        *(.text*)
    } :text
    .rodata : {
        *(.rodata*)
    } :text

    . = 0x0000008000020000;
    .data : {
        *(.data*)
        *(.got*)
    } :data
    .bss : {
        *(.bss*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
