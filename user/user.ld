/* CrugOS ring-3 program layout (phase U1).
 *
 * Both segments sit inside the user window and are page aligned, because the
 * kernel loader maps whole pages and refuses unaligned p_vaddr values.
 */
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5); /* R+X */
    data PT_LOAD FLAGS(6); /* R+W */
}

SECTIONS {
    . = 0x0000008000010000;
    .text : {
        *(.text.start)
        *(.text*)
    } :text
    .rodata : { *(.rodata*) } :text
    . = ALIGN(4096);
    .data : {
        *(.data*)
        *(.got*)
    } :data
    .bss : {
        *(.bss*)
        *(COMMON)
    } :data
    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
        *(.note*)
        *(.build-id)
    }
}
