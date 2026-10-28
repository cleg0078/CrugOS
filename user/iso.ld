/* CrugOS isolation probe layout (phase U2).
 *
 * Three page-aligned segments: code, one data page and the single .iso word on
 * a page of its own at 0x8000030000. The kernel loader maps whole pages, so
 * every segment start must be page aligned.
 */
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5); /* R+X */
    data PT_LOAD FLAGS(6); /* R+W */
    iso  PT_LOAD FLAGS(6); /* R+W, one page at a fixed address */
}

SECTIONS {
    . = 0x0000008000010000;
    .text : {
        *(.text.start)
        *(.text*)
    } :text
    .rodata : { *(.rodata*) } :text
    . = 0x0000008000020000;
    .data : {
        *(.data*)
        *(.got*)
    } :data
    .bss : {
        *(.bss*)
        *(COMMON)
    } :data
    . = 0x0000008000030000;
    .iso : { *(.iso) } :iso
    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
        *(.note*)
        *(.build-id)
    }
}